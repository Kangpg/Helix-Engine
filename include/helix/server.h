#pragma once

// =============================================================================
// Helix public SDK — Server (부트스트랩)
//
// 사용 예 (구매 업체가 작성하는 전부):
//
//   class MyApp : public helix::IServerApp {
//     void OnMessage(helix::Session& s, helix::ByteSpan m) override { s.Send(m); }
//   };
//
//   int main() {
//     return helix::Server::LoadConfig("./config/helix.toml")
//                .Run(std::make_unique<MyApp>());
//   }
//
// 구현은 pimpl 뒤에 숨긴다 — 이 헤더에는 저수준/플랫폼 타입이 없다.
// =============================================================================

#include "types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace helix
{
	class Session;
	class IServerApp;

	using ServerAppPtr = std::unique_ptr<IServerApp>;

	class Server
	{
	public:
		// 설정 파일(TOML) + 환경변수 오버라이드로 서버를 구성한다.
		// 파일이 없거나 검증 실패 시, 이후 Run()이 Status::ConfigError로 종료.
		[[nodiscard]] static Server LoadConfig(std::string_view configPath = "./config/helix.toml");

		Server(Server&&) noexcept;
		Server& operator=(Server&&) noexcept;
		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;
		~Server();

		// 앱을 붙이고 실행한다. Stop() 또는 시그널(SIGINT/SIGTERM)까지 블로킹.
		// 반환값은 프로세스 종료코드로 그대로 쓸 수 있다(0 = 정상).
		int Run(ServerAppPtr app);

		// 다른 스레드/시그널 핸들러에서 graceful shutdown을 트리거.
		void Stop();

		// SessionId로 활성 세션 조회(없으면 nullptr). 크로스스레드 안전.
		[[nodiscard]] Session* FindSession(SessionId id) noexcept;

		// 활성 세션 수(통계/헬스용).
		[[nodiscard]] std::size_t SessionCount() const noexcept;

		// --- scale-out (Phase 6) — 클러스터 비활성 시에도 로컬에 한해 동작 ---

		// SessionId로 메시지 전송(노드 투명). 대상이 이 노드면 직접 송신하고,
		// 다른 노드면 백플레인(pub/sub)으로 전달한다. 프레이밍은 엔진이 부착.
		// 반환: Ok / InvalidSession(미발견) / TransportError(백플레인 오류) 등.
		Status RouteToSession(SessionId target, ByteSpan message);

		// 런타임 데이터 버전을 전 노드에 브로드캐스트(핫스왑 트리거).
		// 각 노드의 IServerApp::OnDataVersion이 호출된다(자기 노드 포함).
		void PublishDataVersion(std::string_view channel, std::uint64_t version);

		// 클러스터 공유 KV(룸/매치 등). 클러스터 비활성 시 Set=false / Get=nullopt.
		bool ClusterKvSet(std::string_view key, std::string_view value, std::uint32_t ttlSec = 0);
		[[nodiscard]] std::optional<std::string> ClusterKvGet(std::string_view key);

	private:
		Server() noexcept;
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
