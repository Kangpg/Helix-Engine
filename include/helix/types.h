#pragma once

// =============================================================================
// Helix public SDK — core value types
//
// 이 헤더에는 플랫폼 타입들이 노출되지않고, 사용자는 해당 헤더를 보고 서버를 작성한다.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace helix
{
	using uint64 = std::uint64_t;
	using uint32 = std::uint32_t;
	using uint16 = std::uint16_t;
	using uint8 = std::uint8_t;
	
	using int64 = std::int64_t;
	using int32 = std::int32_t;
	using int16 = std::int16_t;
	using int8 = std::int8_t;

	// 세션 고유 식별자
	using SessionId = uint64;

	inline constexpr SessionId kInvalidSessionId = 0;

	// 상위 24비트 = NodeId, 하위 40비트 = 노드 로컬 시퀀스(노드당 ~1조 세션)
	inline constexpr int kSessionNodeShift = 40;
	inline constexpr SessionId kSessionSeqMask = (SessionId{ 1 } << kSessionNodeShift) - 1;

	[[nodiscard]] inline constexpr SessionId MakeSessionId(uint32 nodeId, uint64 seq) noexcept
	{
		return (static_cast<SessionId>(nodeId) << kSessionNodeShift) | (seq & kSessionSeqMask);
	}

	// SessionId에서 소유 NodeId 추출
	[[nodiscard]] inline constexpr uint32 NodeOf(SessionId id) noexcept
	{
		return static_cast<uint32>(id >> kSessionNodeShift);
	}

	// 애플리케이션에 송신할 바이트 묶음.
	using ByteSpan = std::span<const std::byte>;

	// 지원 프로토콜
	enum class TransportType : uint8
	{
		Tcp = 0,

		// UDP 위에 신뢰성, 순서 보장을 직접 얹은 프로토콜
		// "임베드"는 ikcp.c(skywind3000 / kcp, MIT)를 외부 의존성 없이 소스째 넣었다는 뜻(src / transport / kcp / )
		// TCP보다 재전송을 공격적으로 해서 지연을 낮추고 HoL blocking을 회피
		// 대신 대역폭을 더 쓰고 연결당 상태 관리 비용이 큼
		// FPS·MOBA·액션 MMO처럼 틱레이트 높고 지연이 곧 체감 품질
		Kcp,

		// UDP 기반 표준 프로토콜 (Microsoft의 msquic 라이브러리를 랩핑)
		// TLS 1.3 암호화가 프로토콜에 내장
		// 한 연결 안에서 여러 스트림을 독립적으로 굴리는 스트림 멀티플렉싱과, IP가 바뀌어도 연결을 유지하는 connection migration(모바일 망 전환에 유리)을 제공
		Quic,
	};

	// 세션 종료 사유
	enum class SessionCloseReason : uint8
	{
		Normal = 0,		// 정상 종료
		PeerClosed,		// 상대가 연결을 닫음
		IdleTimeout,		// idle 타임아웃
		IoError,			// 소켓/전송 오류
		ProtocolError,	// 프레이밍/프로토콜 위반 (예: 최대 패킷 초과)
		ServerShutdown,	// 서버 graceful shutdown / 드레이닝
		Kicked,			// 애플리케이션이 Session::Close 호출
	};

	// 공개 API 결과 코드
	enum class Status : int32
	{
		Ok = 0,
		ConfigError,        // 설정 파일/검증 실패
		AlreadyRunning,
		NotRunning,
		InvalidSession,
		BackpressureFull,   // 송신 큐 한계 (백프레셔)
		TransportError,
		InternalError,
	};
	// 원격 피어 주소(읽기용). 저수준 sockaddr를 노출하지 않는 추상 표현.
	struct Endpoint
	{
		std::string ip;
		uint16 port = 0;
	};

	[[nodiscard]] constexpr std::string_view ToString(SessionCloseReason reason) noexcept
	{
		switch (reason)
		{
		case SessionCloseReason::Normal:         return "Normal";
		case SessionCloseReason::PeerClosed:     return "PeerClosed";
		case SessionCloseReason::IdleTimeout:    return "IdleTimeout";
		case SessionCloseReason::IoError:        return "IoError";
		case SessionCloseReason::ProtocolError:  return "ProtocolError";
		case SessionCloseReason::ServerShutdown: return "ServerShutdown";
		case SessionCloseReason::Kicked:         return "Kicked";
		}
		return "Unknown";
	}
}
