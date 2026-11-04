#pragma once

// =============================================================================
// Phase 6 — 자체 RESP(REdis Serialization Protocol) 클라이언트 (의존성 0)
//
// 외부 Redis 클라이언트 라이브러리(hiredis 등)를 끌어오지 않고, 엔진이 기존에
// TOML 파서를 직접 구현한 것과 같은 철학으로 최소 RESP2 클라이언트를 직접 둔다.
//   - 신규 CMake 빌드를 vcpkg 의존성 없이 유지(현재 ws2_32만 링크)
//   - 풀 컨트롤(타임아웃/재연결/파이프라인을 우리 방식대로)
//
// 범위(RESP2): 단순문자열(+) · 에러(-) · 정수(:) · 벌크($, 바이너리 안전) ·
// 배열(*) · null. pub/sub 푸시 메시지도 일반 배열로 수신한다.
//
// 블로킹 소켓 기반(요청/응답 + 구독 루프). SOLID상 "프레이밍/프로토콜"만 담당하고
// (RespConn), 백플레인 의미(디렉터리/라우팅)는 RedisBackplane이 가진다.
//
// 플랫폼: 현재 Windows(WinSock2) 전용 — 엔진 신규 코어가 Phase 4까지 Windows
// 전용인 것과 동일. Winsock 초기화(WSAStartup)는 ServerCore가 이미 수행한다.
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace helix::cluster
{
	// 파싱된 RESP 응답 1건. 벌크/문자열은 바이너리 안전(str에 \0 포함 가능).
	struct RespValue
	{
		enum class Type : std::uint8_t { Nil, Simple, Error, Integer, Bulk, Array };

		Type type = Type::Nil;
		std::string str;                 // Simple / Error / Bulk
		long long integer = 0;           // Integer
		std::vector<RespValue> elements; // Array

		[[nodiscard]] bool IsNil() const noexcept { return type == Type::Nil; }
		[[nodiscard]] bool IsError() const noexcept { return type == Type::Error; }
	};

	// 단일 Redis 연결(블로킹). 한 인스턴스는 한 스레드에서만(또는 외부 mutex로
	// 직렬화해) 사용한다 — 명령 연결은 RedisBackplane이 mutex로 보호하고,
	// 구독 연결은 전용 스레드가 단독 사용한다.
	class RespConn
	{
	public:
		RespConn() = default;
		~RespConn();

		RespConn(const RespConn&) = delete;
		RespConn& operator=(const RespConn&) = delete;

		// TCP 연결. 성공 시 true. recvTimeoutMs>0 이면 수신 타임아웃 설정.
		bool Connect(const std::string& host, std::uint16_t port, int recvTimeoutMs = 0);
		void Close() noexcept;
		[[nodiscard]] bool IsOpen() const noexcept;

		// 요청(args를 RESP 배열로 인코딩) → 응답 1건. IO 오류 시 false(연결 닫힘).
		bool Command(const std::vector<std::string>& args, RespValue& out);

		// 요청만 전송(파이프라인/구독용). 응답은 ReadReply로 따로 읽는다.
		bool SendCommand(const std::vector<std::string>& args);

		// 응답 1건 파싱(구독 루프/파이프라인 회수용). IO/프로토콜 오류 시 false.
		bool ReadReply(RespValue& out);

	private:
		bool SendAll(const char* data, std::size_t len);
		bool Fill();                       // 소켓에서 버퍼로 추가 수신
		bool ReadLine(std::string& line);  // CRLF 종단 한 줄(종단 제외)
		bool ReadBytes(std::size_t n, std::string& out); // n바이트 + 후행 CRLF 소비
		bool ParseValue(RespValue& out);

		std::uintptr_t _sock = ~static_cast<std::uintptr_t>(0); // INVALID_SOCKET (헤더에 WinSock 비노출)
		std::string _rx;          // 수신 버퍼
		std::size_t _pos = 0;     // _rx 내 읽기 커서
	};
}
