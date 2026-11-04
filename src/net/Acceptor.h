#pragma once

// =============================================================================
// Phase 1 — Acceptor (listen + AcceptEx 루프)
//
// listen 소켓을 만들어 백엔드에 등록하고, AcceptEx를 N개 동시 사전 게시한다.
// 수락 완료 시 SO_UPDATE_ACCEPT_CONTEXT 적용 → 백엔드 등록 → 세션 생성/시작 →
// 같은 컨텍스트로 새 수락 소켓을 만들어 재무장.
//
// 미검증: Windows/MSVC 전용. VS(x64) 빌드 필요.
// =============================================================================

#include "INetBackend.h"
#include "helix/types.h"

#include <cstdint>
#include <vector>
#include <atomic>
#include <mutex>

namespace helix::core { class SessionManager; }

namespace helix::net
{
	class Acceptor
	{
	public:
		Acceptor(INetBackend& backend, helix::core::SessionManager& mgr);
		~Acceptor();

		bool Listen(const helix::Endpoint& ep, std::uint32_t backlog, int concurrentAccepts);
		void Stop();
		std::uint32_t PendingCount() const noexcept { return _pendingAccepts.load(); }

		// IO 루프(ServerCore)에서 op==Accept 완료 시 호출.
		void OnAcceptComplete(IoContext* ctx, const std::error_code& ec);

	private:
		bool PostOneAccept(IoContext* ctx);
		[[nodiscard]] NativeHandle CreateAcceptSocket() const;

		INetBackend& _backend;
		helix::core::SessionManager& _mgr;
		NativeHandle _listen = kInvalidHandle;
		std::vector<IoContext*> _acceptCtxs;
		bool _stopped = false;
		std::mutex _acceptLock;
		std::atomic<std::uint32_t> _pendingAccepts{0};
	};
}
