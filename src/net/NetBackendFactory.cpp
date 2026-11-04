#include "NetBackendFactory.h"

#if defined(_WIN32)
#  include "WindowsIocpBackend.h"
#  include "WindowsRioBackend.h"
#else
#  include "EpollBackend.h"   // Phase 4: Linux 백엔드
#endif

namespace helix::net
{
	std::unique_ptr<INetBackend> CreateNetBackend(const BackendConfig& cfg)
	{
#if defined(_WIN32)
		if (cfg.kind == IoBackendKind::Epoll) { return nullptr; }
		if (cfg.kind == IoBackendKind::IocpRio)
		{
			auto backend = std::make_unique<WindowsRioBackend>();
			if (!backend->Init(cfg)) { return nullptr; }
			return backend;
		}
		auto backend = std::make_unique<WindowsIocpBackend>();
		if (!backend->Init(cfg)) { return nullptr; }
		return backend;
#else
		if (cfg.kind != IoBackendKind::Auto && cfg.kind != IoBackendKind::Epoll) { return nullptr; }
		auto backend = std::make_unique<EpollBackend>();
		if (!backend->Init(cfg)) { return nullptr; }
		return backend;
#endif
	}
}
