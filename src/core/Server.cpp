// =============================================================================
// Phase 3 — 공개 helix::Server 구현 (pimpl → ServerCore)
//
// LoadConfig: TOML/INI-subset 파일 + 환경변수 오버라이드 로드 → 검증 → 로거 초기화
// → ServerCoreConfig 매핑. 검증 실패 시 Run()이 ConfigError로 종료.
// =============================================================================

#include "helix/server.h"
#include "helix/server_app.h"
#include "helix/session.h"

#include "ServerCore.h"
#include "../config/EngineConfig.h"
#include "../log/Log.h"

#include <string>

namespace helix
{
	namespace
	{
		net::IoBackendKind ParseBackend(const std::string& s) noexcept
		{
			if (s == "iocp")     { return net::IoBackendKind::Iocp; }
			if (s == "iocp_rio") { return net::IoBackendKind::IocpRio; }
			if (s == "epoll")    { return net::IoBackendKind::Epoll; }
			return net::IoBackendKind::Auto;
		}

		helix::TransportType ParseTransport(const std::string& s) noexcept
		{
			if (s == "kcp")  { return helix::TransportType::Kcp; }
			if (s == "quic") { return helix::TransportType::Quic; }
			return helix::TransportType::Tcp;
		}
	}

	struct Server::Impl
	{
		std::string configPath;
		core::ServerCoreConfig cfg;
		std::unique_ptr<core::ServerCore> core;
		std::unique_ptr<IServerApp> app;
		bool configError = false;
		std::string configErrMsg;
	};

	Server::Server() noexcept : _impl(std::make_unique<Impl>()) {}
	Server::Server(Server&&) noexcept = default;
	Server& Server::operator=(Server&&) noexcept = default;
	Server::~Server() = default;

	Server Server::LoadConfig(std::string_view configPath)
	{
		Server s;
		s._impl->configPath = std::string(configPath);

		config::ConfigLoader loader;
		auto loaded = loader.Load(s._impl->configPath);

		config::EngineConfig ec;  // 기본값
		if (loaded.has_value())
		{
			ec = *loaded;
		}
		else
		{
			s._impl->configError = true;
			s._impl->configErrMsg = loader.GetError();
		}

		// 로거 초기화(설정 오류여도 기본 레벨로 초기화해 진단 출력 가능).
		log::LogConfig lc;
		lc.level = log::ParseLevel(ec.logLevel);
		lc.toStdout = (ec.logSink == "stdout" || ec.logSink == "both");
		lc.toFile = (ec.logSink == "file" || ec.logSink == "both");
		lc.file = ec.logFile;
		log::Init(lc);

		if (s._impl->configError)
		{
			HLOG_ERROR("config validation failed: " + s._impl->configErrMsg);
		}
		else
		{
			HLOG_INFO("config loaded: " + (s._impl->configPath.empty() ? std::string("(defaults)") : s._impl->configPath));
		}

		// EngineConfig → ServerCoreConfig 매핑.
		auto& c = s._impl->cfg;
		c.bind = helix::Endpoint{ ec.ip, ec.port };
		c.transport = ParseTransport(ec.transport);
		c.backend = ParseBackend(ec.backend);
		c.maxConnections = ec.maxConnections;
		c.backlog = ec.backlog;
		c.ioThreads = ec.ioThreads;
		c.concurrentAccepts = static_cast<int>(ec.concurrentAccepts);
		c.rioBufferSize = ec.rioBufferSize;
		c.rioCompletionQueueSize = ec.rioCompletionQueueSize;
		c.recvBufSize = ec.recvBufSize;
		c.maxSendQueue = ec.maxSendQueue;
		c.frame.headerSize = ec.frameHeaderSize;
		c.frame.bigEndian = ec.frameBigEndian;
		c.frame.maxMessageSize = ec.maxMessageSize;
		c.drainTimeoutMs = ec.drainTimeoutMs;
		c.clusterEnabled = ec.clusterEnabled;
		c.clusterBackend = ec.clusterBackend;
		c.clusterUrl = ec.clusterUrl;
		c.nodeId = ec.nodeId;
		c.sessionTtlSec = ec.sessionTtlSec;
		c.heartbeatMs = ec.heartbeatMs;
		return s;
	}

	int Server::Run(ServerAppPtr app)
	{
		if (_impl->configError)
		{
			HLOG_ERROR("refusing to start due to config error");
			return static_cast<int>(Status::ConfigError);
		}

		_impl->app = std::move(app);
		_impl->core = std::make_unique<core::ServerCore>(_impl->cfg);

		if (!_impl->core->Start(_impl->app.get()))
		{
			HLOG_ERROR("server failed to start (bind/backend)");
			return static_cast<int>(Status::TransportError);
		}
		if (_impl->app) { _impl->app->OnStart(*this); }

		_impl->core->Run();   // Stop()/시그널까지 블로킹

		if (_impl->app) { _impl->app->OnStop(); }
		return 0;
	}

	void Server::Stop()
	{
		if (_impl->core) { _impl->core->Stop(); }
	}

	Session* Server::FindSession(SessionId id) noexcept
	{
		return _impl->core ? _impl->core->Find(id) : nullptr;
	}

	std::size_t Server::SessionCount() const noexcept
	{
		return _impl->core ? _impl->core->SessionCount() : 0;
	}

	Status Server::RouteToSession(SessionId target, ByteSpan message)
	{
		return _impl->core ? _impl->core->RouteToSession(target, message) : Status::NotRunning;
	}

	void Server::PublishDataVersion(std::string_view channel, std::uint64_t version)
	{
		if (_impl->core) { _impl->core->PublishDataVersion(channel, version); }
	}

	bool Server::ClusterKvSet(std::string_view key, std::string_view value, std::uint32_t ttlSec)
	{
		return _impl->core ? _impl->core->KvSet(key, value, ttlSec) : false;
	}

	std::optional<std::string> Server::ClusterKvGet(std::string_view key)
	{
		return _impl->core ? _impl->core->KvGet(key) : std::nullopt;
	}
}
