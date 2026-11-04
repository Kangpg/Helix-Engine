#pragma once

// =============================================================================
// ServerCore (내부 엔진 러너)
//
// 백엔드 + Acceptor + SessionManager + I/O 워커 스레드를 묶는다. Phase 2부터는
// IServerApp를 받아 OnConnect/OnMessage/OnDisconnect 를 호출하고, 프레이밍
// 설정(FrameCodec)을 세션에 주입한다. 공개 helix::Server(pimpl)가 이 코어를 감싼다.
//
// Winsock 수명(WSAStartup/Cleanup)도 여기서 관리한다.
// 미검증: Windows/MSVC 전용.
// =============================================================================

#include "../net/INetBackend.h"
#include "../net/Acceptor.h"
#include "../session/SessionManager.h"
#include "../session/ISessionObserver.h"
#include "../proto/FrameCodec.h"
#include "../cluster/IBackplane.h"
#include "../transport/ITransport.h"
#include "helix/types.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace helix { class IServerApp; class Session; }

namespace helix::core
{
	class TransportSession;

	struct ServerCoreConfig
	{
		helix::Endpoint bind{ "0.0.0.0", 5000 };
		net::IoBackendKind backend = net::IoBackendKind::Auto;
		std::uint32_t maxConnections = 10000;
		std::uint32_t backlog = 128;
		std::uint32_t ioThreads = 0;            // 0 = hardware_concurrency
		int concurrentAccepts = 16;
		std::uint32_t rioBufferSize = 16384;
		std::uint32_t rioCompletionQueueSize = 32768;
		std::uint32_t recvBufSize = 8192;
		std::uint32_t maxSendQueue = 1024;
		std::uint32_t drainTimeoutMs = 5000;   // graceful shutdown 시 완료 배수 대기 한계
		proto::FrameCodec frame{};

		// --- 전송 선택 (Phase 7) ---  Tcp=기존 INetBackend 경로 / Kcp·Quic=ITransport
		helix::TransportType transport = helix::TransportType::Tcp;

		// --- scale-out (Phase 6) ---
		bool clusterEnabled = false;
		std::string clusterBackend = "redis";  // redis (현재)
		std::string clusterUrl = "redis://127.0.0.1:6379/0";
		std::uint32_t nodeId = 0;
		std::uint32_t sessionTtlSec = 30;
		std::uint32_t heartbeatMs = 10000;
	};

	// ServerCore는 백플레인 수신(IBackplaneSink) + 세션 수명 옵저버(ISessionObserver)
	// + 전송 수신(ITransportSink)을 구현한다. TCP는 INetBackend 경로(Acceptor/Session),
	// KCP/QUIC는 ITransport 경로(TransportSession)로 동일 IServerApp을 구동한다.
	class ServerCore final
		: public cluster::IBackplaneSink
		, public ISessionObserver
		, public transport::ITransportSink
	{
	public:
		explicit ServerCore(const ServerCoreConfig& cfg);
		~ServerCore() override;

		bool Start(helix::IServerApp* app);   // 초기화 + 워커 기동
		void Run();                           // 워커 join까지 블로킹 + 정리
		void Stop();                          // graceful shutdown 신호

		[[nodiscard]] helix::Session* Find(helix::SessionId id);
		[[nodiscard]] std::size_t SessionCount() const;
		[[nodiscard]] const char* BackendName() const noexcept;

		// --- scale-out 공개 동작(공개 helix::Server가 위임) ---
		helix::Status RouteToSession(helix::SessionId target, helix::ByteSpan message);
		void PublishDataVersion(std::string_view channel, std::uint64_t version);
		bool KvSet(std::string_view key, std::string_view value, std::uint32_t ttlSec);
		[[nodiscard]] std::optional<std::string> KvGet(std::string_view key);

		// --- IBackplaneSink ---
		void OnRouted(helix::SessionId target, helix::ByteSpan payload) override;
		void OnDataVersion(std::string_view channel, std::uint64_t version) override;

		// --- ISessionObserver ---
		void OnSessionOpened(helix::SessionId id) override;
		void OnSessionClosed(helix::SessionId id) override;

		// --- ITransportSink (KCP/QUIC 경로, 전송 스레드에서 호출) ---
		void OnAccepted(transport::ConnId conn, const helix::Endpoint& remote) override;
		void OnData(transport::ConnId conn, helix::ByteSpan bytes) override;
		void OnClosed(transport::ConnId conn, helix::SessionCloseReason reason) override;

	private:
		void WorkerLoop(std::uint32_t index);
		void HeartbeatLoop();

		ServerCoreConfig _cfg;
		helix::IServerApp* _app = nullptr;
		std::unique_ptr<net::INetBackend> _backend;
		std::unique_ptr<cluster::IBackplane> _backplane;
		std::unique_ptr<SessionManager> _sessions;
		std::unique_ptr<net::Acceptor> _acceptor;
		std::vector<std::thread> _workers;
		std::atomic<bool> _running{ false };
		bool _wsaInit = false;

		// --- 전송(ITransport) 경로 (Phase 7) — _txMode일 때만 사용 ---
		bool _txMode = false;
		std::unique_ptr<transport::ITransport> _transport;
		mutable std::mutex _txMx;
		std::unordered_map<helix::SessionId, std::shared_ptr<TransportSession>> _txSessions;
		std::unordered_map<transport::ConnId, helix::SessionId> _connToSid;
		std::atomic<std::uint64_t> _txSeq{ 1 };

		// heartbeat(디렉터리 TTL 갱신) 전용 스레드.
		std::thread _hbThread;
		std::atomic<bool> _hbRunning{ false };

		// graceful drain: Stop()이 요청을 표시하면 Run()이 드레이닝 후 종료.
		std::mutex _stopMx;
		std::condition_variable _stopCv;
		bool _stopRequested = false;
	};
}
