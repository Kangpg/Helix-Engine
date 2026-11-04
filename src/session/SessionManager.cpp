#include "SessionManager.h"
#include "Session.h"
#include "ISessionObserver.h"

#include "helix/server_app.h"

#include <vector>

namespace helix::core
{
	SessionManager::SessionManager(net::INetBackend& backend, std::uint32_t maxConnections,
	                               const SessionFactoryConfig& factory, helix::IServerApp* app,
	                               std::uint32_t nodeId, ISessionObserver* observer)
		: _backend(backend), _maxConnections(maxConnections), _factory(factory), _app(app),
		  _nodeId(nodeId), _observer(observer)
	{
	}

	SessionManager::~SessionManager()
	{
		// 정상 경로에서는 CloseAll 후 세션이 모두 자기 삭제되어 맵이 비어 있어야 한다.
	}

	Session* SessionManager::Create(net::NativeHandle sock, const helix::Endpoint& remote)
	{
		Session* s = nullptr;
		helix::SessionId id = helix::kInvalidSessionId;
		{
			std::lock_guard<std::mutex> guard(_lock);
			if (_sessions.size() >= _maxConnections)
			{
				return nullptr;
			}
			const std::uint64_t seq = _nextSeq.fetch_add(1, std::memory_order_relaxed);
			id = helix::MakeSessionId(_nodeId, seq);
			s = new Session(id, sock, _backend, *this,
			                _factory.recvBufSize, _factory.frame, _app, remote,
			                _factory.maxSendQueue);
			_sessions.emplace(id, s);
		}
		// 디렉터리 등록은 맵 락 밖에서(옵저버가 네트워크 호출일 수 있음).
		if (_observer != nullptr) { _observer->OnSessionOpened(id); }
		return s;
	}

	Session* SessionManager::Find(helix::SessionId id)
	{
		std::lock_guard<std::mutex> guard(_lock);
		const auto it = _sessions.find(id);
		return it == _sessions.end() ? nullptr : it->second;
	}

	void SessionManager::Remove(helix::SessionId id)
	{
		{
			std::lock_guard<std::mutex> guard(_lock);
			_sessions.erase(id);
		}
		// 디렉터리 제거도 맵 락 밖에서.
		if (_observer != nullptr) { _observer->OnSessionClosed(id); }
	}

	std::size_t SessionManager::Count() const
	{
		std::lock_guard<std::mutex> guard(_lock);
		return _sessions.size();
	}

	void SessionManager::CloseAll()
	{
		std::vector<Session*> snapshot;
		{
			std::lock_guard<std::mutex> guard(_lock);
			snapshot.reserve(_sessions.size());
			for (auto& [id, s] : _sessions)
			{
				s->AddRef();
				snapshot.push_back(s);
			}
		}
		for (auto* s : snapshot)
		{
			s->Close(helix::SessionCloseReason::ServerShutdown);
			s->Release();
		}
	}
}
