#include "ServerCore.h"
#include "../net/NetBackendFactory.h"
#include "../session/Session.h"
#include "TransportSession.h"
#include "../cluster/BackplaneFactory.h"
#include "../log/Log.h"

#include "helix/server_app.h"
#include "helix/session.h"

#include "../net/PlatformNet.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <string>

namespace helix::core
{
	ServerCore::ServerCore(const ServerCoreConfig& cfg)
		: _cfg(cfg)
	{
	}

	ServerCore::~ServerCore()
	{
		Stop();

		Run();    // 드레이닝 + join + 정리 (Run()이 이미 끝났다면 no-op)
	}

	bool ServerCore::Start(helix::IServerApp* app)
	{
		if (_running.exchange(true, std::memory_order_acq_rel))
		{
			return true;
		}

		_app = app;

		WSADATA wsa{};
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
		{
			_wsaInit = true;
		}

		// scale-out 백플레인(선택, TCP/KCP 공통). 연결 실패 시 단일 노드로 계속.
		if (_cfg.clusterEnabled)
		{
			_backplane = cluster::CreateBackplane(cluster::ParseBackplaneKind(_cfg.clusterBackend));
			if (_backplane != nullptr)
			{
				cluster::BackplaneConfig bp{};
				bp.url = _cfg.clusterUrl;
				bp.nodeId = _cfg.nodeId;
				bp.sessionTtlSec = _cfg.sessionTtlSec;
				if (!_backplane->Connect(bp, *this))
				{
					HLOG_WARN("backplane connect failed — continuing standalone (node=" + std::to_string(_cfg.nodeId) + ")");
					_backplane.reset();
				}
			}
			else
			{
				HLOG_WARN("unknown cluster backend '" + _cfg.clusterBackend + "' — standalone");
			}
		}

		_txMode = (_cfg.transport != helix::TransportType::Tcp);

		// === KCP/QUIC 경로 (ITransport, 자체 I/O) ===
		if (_txMode)
		{
			_transport = transport::CreateTransport(_cfg.transport);
			if (_transport == nullptr)
			{
				HLOG_ERROR("transport not available for selected kind (kcp/quic)");
				if (_backplane) { _backplane->Disconnect(); _backplane.reset(); }
				_running.store(false, std::memory_order_release);
				return false;
			}
			transport::TransportConfig tc{};
			tc.bind = _cfg.bind;
			tc.backlog = _cfg.backlog;
			tc.maxConnections = _cfg.maxConnections;
			if (!_transport->Init(tc, *this, nullptr) || !_transport->Listen())
			{
				HLOG_ERROR("transport init/listen failed");
				_transport.reset();
				if (_backplane) { _backplane->Disconnect(); _backplane.reset(); }
				_running.store(false, std::memory_order_release);
				return false;
			}
			if (_backplane != nullptr)
			{
				_hbRunning.store(true, std::memory_order_release);
				_hbThread = std::thread([this] { HeartbeatLoop(); });
			}
			HLOG_INFO(std::string("listening on ") + _cfg.bind.ip + ":" + std::to_string(_cfg.bind.port)
			          + " transport=" + (_cfg.transport == helix::TransportType::Kcp ? "kcp" : "quic")
			          + (_backplane != nullptr
			                 ? (" cluster=on node=" + std::to_string(_cfg.nodeId))
			                 : std::string(" cluster=off")));
			return true;
		}

		// === TCP 경로 (INetBackend / IOCP) ===
		net::BackendConfig bcfg{};
		bcfg.kind = _cfg.backend;
		bcfg.maxConnections = _cfg.maxConnections;
		bcfg.rioBufferSize = _cfg.rioBufferSize;
		bcfg.rioCompletionQueueSize = _cfg.rioCompletionQueueSize;

		_backend = net::CreateNetBackend(bcfg);
		if (_backend == nullptr)
		{
			if (_backplane) { _backplane->Disconnect(); _backplane.reset(); }
			_running.store(false, std::memory_order_release);
			return false;
		}

		SessionFactoryConfig factory{};
		factory.recvBufSize = _cfg.recvBufSize;
		factory.maxSendQueue = _cfg.maxSendQueue;
		factory.frame = _cfg.frame;

		// 디렉터리 등록은 백플레인이 있을 때만 옵저버로 받는다.
		ISessionObserver* observer = (_backplane != nullptr) ? this : nullptr;
		_sessions = std::make_unique<SessionManager>(*_backend, _cfg.maxConnections, factory, _app,
		                                             _cfg.nodeId, observer);
		_acceptor = std::make_unique<net::Acceptor>(*_backend, *_sessions);

		if (!_acceptor->Listen(_cfg.bind, _cfg.backlog, _cfg.concurrentAccepts))
		{
			// 워커가 아직 없어 Run()의 정리 경로를 타지 않으므로 여기서 백플레인을 정리.
			if (_backplane) { _backplane->Disconnect(); _backplane.reset(); }
			_backend->Shutdown();
			_running.store(false, std::memory_order_release);
			return false;
		}

		std::uint32_t threads = _cfg.ioThreads;
		if (threads == 0)
		{
			threads = std::max<std::uint32_t>(1, std::thread::hardware_concurrency());
		}

		_workers.reserve(threads);
		for (std::uint32_t i = 0; i < threads; ++i)
		{
			_workers.emplace_back([this, i]() { WorkerLoop(i); });
		}

		// 클러스터면 디렉터리 TTL 갱신 스레드 기동.
		if (_backplane != nullptr)
		{
			_hbRunning.store(true, std::memory_order_release);
			_hbThread = std::thread([this] { HeartbeatLoop(); });
		}

		HLOG_INFO("listening on " + _cfg.bind.ip + ":" + std::to_string(_cfg.bind.port)
		          + " backend=" + _backend->Name()
		          + " io_threads=" + std::to_string(threads)
		          + (_backplane != nullptr
		                 ? (" cluster=on node=" + std::to_string(_cfg.nodeId))
		                 : std::string(" cluster=off")));
		return true;
	}

	void ServerCore::HeartbeatLoop()
	{
		// heartbeatMs 동안 잠들되, 종료 신호에 빠르게 반응하도록 짧게 쪼개 확인.
		const std::uint32_t step = 100;
		std::uint32_t elapsed = 0;
		while (_hbRunning.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(step));
			elapsed += step;
			if (elapsed >= _cfg.heartbeatMs)
			{
				elapsed = 0;
				if (_backplane != nullptr) { _backplane->Heartbeat(); }
			}
		}
	}

	void ServerCore::WorkerLoop(std::uint32_t /*index*/)
	{
		constexpr int kBatch = 64;
		std::array<net::IoCompletion, kBatch> comps{};

		while (_running.load(std::memory_order_acquire))
		{
			const int n = _backend->Poll(comps.data(), kBatch, 1000);
			for (int i = 0; i < n; ++i)
			{
				net::IoCompletion& c = comps[i];
				if (c.ctx == nullptr)
				{
					continue;  // Wake()
				}
				switch (c.ctx->op)
				{
				case net::IoOp::Accept:
					_acceptor->OnAcceptComplete(c.ctx, c.error);
					break;
				case net::IoOp::Recv:
					static_cast<Session*>(c.ctx->user)->OnRecvComplete(c.error ? 0u : c.bytesTransferred);
					break;
				case net::IoOp::Send:
					static_cast<Session*>(c.ctx->user)->OnSendComplete(c.error ? 0u : c.bytesTransferred);
					break;
				default:
					break;
				}
			}
		}
	}

	void ServerCore::Run()
	{
		// === KCP/QUIC 경로: 전송이 자체 스레드를 돌리므로 워커 없음 ===
		if (_txMode)
		{
			if (_transport == nullptr) { return; }  // 시작 안 됨/이미 정리됨

			{
				std::unique_lock<std::mutex> lk(_stopMx);
				_stopCv.wait(lk, [this] { return _stopRequested; });
			}

			// 전송 중단 → 모든 연결 OnClosed → 앱 OnDisconnect.
			_transport->Stop();

			_hbRunning.store(false, std::memory_order_release);
			if (_hbThread.joinable()) { _hbThread.join(); }
			if (_backplane) { _backplane->Disconnect(); _backplane.reset(); }

			{
				std::lock_guard<std::mutex> lk(_txMx);
				_txSessions.clear();
				_connToSid.clear();
			}
			_transport.reset();
			if (_wsaInit) { ::WSACleanup(); _wsaInit = false; }
			HLOG_INFO("stopped");
			return;
		}

		if (_workers.empty())
		{
			return;  // 시작 안 됐거나 이미 정리됨
		}

		// 1) Stop 요청 대기(블로킹).
		{
			std::unique_lock<std::mutex> lk(_stopMx);
			_stopCv.wait(lk, [this] { return _stopRequested; });
		}

		// 2) 드레이닝: 진행 중 완료를 워커가 배수해 세션이 모두 정리될 때까지 대기(타임아웃).
		//    이 동안 _running은 여전히 true → 워커는 close 완료를 계속 처리한다.
		const auto deadline = std::chrono::steady_clock::now()
		                    + std::chrono::milliseconds(_cfg.drainTimeoutMs);
		bool warned = false;
#if defined(_WIN32)
		while (SessionCount() > 0 || (_acceptor && _acceptor->PendingCount() > 0))
#else
		while (SessionCount() > 0 && std::chrono::steady_clock::now() < deadline)
#endif
		{
			if (!warned && std::chrono::steady_clock::now() >= deadline)
			{
				HLOG_WARN("drain budget exceeded; retaining I/O owners until cancellations complete");
				warned = true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (SessionCount() > 0)
		{
			HLOG_WARN("drain timeout — forcing shutdown, remaining=" + std::to_string(SessionCount()));
		}

		// 3) 워커 종료.
		_running.store(false, std::memory_order_release);
		if (_backend) { _backend->Wake(static_cast<int>(_workers.size())); }
		for (auto& t : _workers)
		{
			if (t.joinable()) { t.join(); }
		}
		_workers.clear();

		// 4) heartbeat + 백플레인 종료. 구독 스레드를 먼저 join 해야 이후 OnRouted가
		//    파괴된 SessionManager를 건드리지 않는다(드레이닝으로 이미 세션은 0).
		_hbRunning.store(false, std::memory_order_release);
		if (_hbThread.joinable()) { _hbThread.join(); }
		if (_backplane) { _backplane->Disconnect(); _backplane.reset(); }

		// 5) 정리.
		_acceptor.reset();
		_sessions.reset();
		if (_backend) { _backend->Shutdown(); _backend.reset(); }
		if (_wsaInit) { ::WSACleanup(); _wsaInit = false; }
		HLOG_INFO("stopped");
	}

	void ServerCore::Stop()
	{
		// 신호만 — 신규 accept 중단 + 세션 close + 워커 깨우기. 드레이닝/join은 Run()이.
		{
			std::lock_guard<std::mutex> lk(_stopMx);
			if (_stopRequested) { return; }
			_stopRequested = true;
		}
		HLOG_INFO("stop requested (draining) — sessions=" + std::to_string(SessionCount()));
		if (_acceptor) { _acceptor->Stop(); }
		if (_sessions) { _sessions->CloseAll(); }
		if (_backend) { _backend->Wake(static_cast<int>(_workers.size())); }
		_stopCv.notify_all();
	}

	helix::Session* ServerCore::Find(helix::SessionId id)
	{
		if (_txMode)
		{
			std::lock_guard<std::mutex> lk(_txMx);
			const auto it = _txSessions.find(id);
			return it != _txSessions.end() ? it->second.get() : nullptr;
		}
		return _sessions ? _sessions->Find(id) : nullptr;
	}

	std::size_t ServerCore::SessionCount() const
	{
		if (_txMode)
		{
			std::lock_guard<std::mutex> lk(_txMx);
			return _txSessions.size();
		}
		return _sessions ? _sessions->Count() : 0;
	}

	const char* ServerCore::BackendName() const noexcept
	{
		return _backend ? _backend->Name() : "none";
	}

	// --- scale-out --------------------------------------------------------------

	helix::Status ServerCore::RouteToSession(helix::SessionId target, helix::ByteSpan message)
	{
		// 1) 로컬 우선: 대상이 이 노드 소유이거나 실제로 존재하면 직접 송신.
		if (helix::NodeOf(target) == _cfg.nodeId)
		{
			helix::Session* s = Find(target);
			return s ? s->Send(message) : helix::Status::InvalidSession;
		}
		if (helix::Session* s = Find(target))
		{
			return s->Send(message);
		}
		// 2) 다른 노드 → 백플레인으로 전달.
		if (_backplane == nullptr)
		{
			return helix::Status::InvalidSession; // 단일 노드인데 로컬에 없음
		}
		return _backplane->RouteToSession(target, message)
		           ? helix::Status::Ok
		           : helix::Status::TransportError;
	}

	void ServerCore::PublishDataVersion(std::string_view channel, std::uint64_t version)
	{
		if (_backplane != nullptr)
		{
			// 전 노드에 브로드캐스트(자기 노드도 구독 중이라 OnDataVersion으로 되돌아온다).
			_backplane->PublishDataVersion(channel, version);
		}
		else if (_app != nullptr)
		{
			// 단일 노드: 로컬에 직접 통지(동작 일관성).
			_app->OnDataVersion(channel, version);
		}
	}

	bool ServerCore::KvSet(std::string_view key, std::string_view value, std::uint32_t ttlSec)
	{
		return _backplane ? _backplane->KvSet(key, value, ttlSec) : false;
	}

	std::optional<std::string> ServerCore::KvGet(std::string_view key)
	{
		return _backplane ? _backplane->KvGet(key) : std::nullopt;
	}

	// --- IBackplaneSink (구독 스레드에서 호출) ---

	void ServerCore::OnRouted(helix::SessionId target, helix::ByteSpan payload)
	{
		if (helix::Session* s = Find(target))
		{
			s->Send(payload);
		}
		// 없으면 드롭(세션이 떠남) — at-most-once.
	}

	void ServerCore::OnDataVersion(std::string_view channel, std::uint64_t version)
	{
		if (_app != nullptr)
		{
			_app->OnDataVersion(channel, version);
		}
	}

	// --- ISessionObserver (워커 스레드에서 호출) ---

	void ServerCore::OnSessionOpened(helix::SessionId id)
	{
		if (_backplane != nullptr) { _backplane->RegisterSession(id); }
	}

	void ServerCore::OnSessionClosed(helix::SessionId id)
	{
		if (_backplane != nullptr) { _backplane->UnregisterSession(id); }
	}

	// --- ITransportSink (KCP/QUIC 전송 스레드에서 호출) ---

	void ServerCore::OnAccepted(transport::ConnId conn, const helix::Endpoint& remote)
	{
		const std::uint64_t seq = _txSeq.fetch_add(1, std::memory_order_relaxed);
		const helix::SessionId sid = helix::MakeSessionId(_cfg.nodeId, seq);

		auto sp = std::make_shared<TransportSession>(sid, *_transport, conn, _cfg.frame, remote, _cfg.transport);
		{
			std::lock_guard<std::mutex> lk(_txMx);
			_txSessions.emplace(sid, sp);
			_connToSid.emplace(conn, sid);
		}
		if (_backplane != nullptr) { _backplane->RegisterSession(sid); }  // 디렉터리(scale-out)
		if (_app != nullptr) { _app->OnConnect(*sp); }
	}

	void ServerCore::OnData(transport::ConnId conn, helix::ByteSpan bytes)
	{
		// 세션을 shared_ptr로 잡아 콜백 도중 재진입 Close에도 살아있게 한다.
		std::shared_ptr<TransportSession> sp;
		{
			std::lock_guard<std::mutex> lk(_txMx);
			const auto c = _connToSid.find(conn);
			if (c == _connToSid.end()) { return; }
			const auto s = _txSessions.find(c->second);
			if (s != _txSessions.end()) { sp = s->second; }
		}
		if (!sp) { return; }

		const bool ok = sp->Reader().Feed(bytes.data(), static_cast<std::uint32_t>(bytes.size()),
			[this, &sp](const std::byte* p, std::uint32_t n)
			{
				if (_app != nullptr) { _app->OnMessage(*sp, helix::ByteSpan(p, n)); }
			});
		if (!ok)
		{
			sp->Close(helix::SessionCloseReason::ProtocolError);  // 과대/손상 프레임
		}
	}

	void ServerCore::OnClosed(transport::ConnId conn, helix::SessionCloseReason reason)
	{
		std::shared_ptr<TransportSession> sp;
		helix::SessionId sid = helix::kInvalidSessionId;
		{
			std::lock_guard<std::mutex> lk(_txMx);
			const auto c = _connToSid.find(conn);
			if (c == _connToSid.end()) { return; }
			sid = c->second;
			const auto s = _txSessions.find(sid);
			if (s != _txSessions.end()) { sp = s->second; _txSessions.erase(s); }
			_connToSid.erase(c);
		}
		if (!sp) { return; }
		if (sp->MarkClosed())  // OnDisconnect 1회 보장
		{
			if (_backplane != nullptr) { _backplane->UnregisterSession(sid); }
			if (_app != nullptr) { _app->OnDisconnect(*sp, reason); }
		}
		// sp가 스코프를 벗어나며 마지막 참조면 소멸(OnDisconnect 이후 정리).
	}
}
