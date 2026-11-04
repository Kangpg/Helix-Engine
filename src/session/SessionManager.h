#pragma once

// =============================================================================
// Phase 2 — SessionManager
//
// 세션 생성/조회/제거. 단일 refcount 소유(맵은 raw 포인터, Remove는 삭제 안 함).
// 생성 시 FrameCodec / IServerApp / 원격 Endpoint / 송신 큐 한계를 주입한다.
// MVP는 unordered_map + mutex. 사전할당 풀은 Phase 4.
// =============================================================================

#include "helix/types.h"
#include "../net/INetBackend.h"
#include "../proto/FrameCodec.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace helix { class IServerApp; }

namespace helix::core
{
	class Session;
	class ISessionObserver;

	struct SessionFactoryConfig
	{
		std::uint32_t recvBufSize = 8192;
		std::uint32_t maxSendQueue = 1024;
		proto::FrameCodec frame{};
	};

	class SessionManager
	{
	public:
		// nodeId: SessionId 상위 비트에 임베드(scale-out 전역 유일성). 단일 노드는 0.
		// observer: 세션 수명 알림(디렉터리 등록/해제). 단일 노드는 nullptr.
		SessionManager(net::INetBackend& backend, std::uint32_t maxConnections,
		               const SessionFactoryConfig& factory, helix::IServerApp* app,
		               std::uint32_t nodeId = 0, ISessionObserver* observer = nullptr);
		~SessionManager();

		// 수락된 소켓으로 세션 생성. 한계 초과 시 nullptr.
		[[nodiscard]] Session* Create(net::NativeHandle sock, const helix::Endpoint& remote);

		[[nodiscard]] Session* Find(helix::SessionId id);

		// Session::Release(ref==0)에서만 호출. 맵에서 제거(삭제는 호출자).
		void Remove(helix::SessionId id);

		[[nodiscard]] std::size_t Count() const;

		void CloseAll();

	private:
		net::INetBackend& _backend;
		std::uint32_t _maxConnections;
		SessionFactoryConfig _factory;
		helix::IServerApp* _app;
		std::uint32_t _nodeId;
		ISessionObserver* _observer;

		std::atomic<std::uint64_t> _nextSeq{ 1 };
		mutable std::mutex _lock;
		std::unordered_map<helix::SessionId, Session*> _sessions;
	};
}
