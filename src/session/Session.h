#pragma once

// =============================================================================
// Phase 2 — Session (내부 구현, 공개 helix::Session 구현체)
//
// Phase 1의 단일 refcount 수명관리는 그대로. 추가:
//  - 공개 helix::Session 인터페이스 구현(Send/Close/Remote/Transport/UserData)
//  - 수신: FrameReader로 완결 메시지를 잘라 IServerApp::OnMessage 로 전달
//  - 송신: Send(ByteSpan)이 길이 헤더를 붙여 큐잉(세션별 직렬화 + 백프레셔)
//  - 수명 이벤트: Start()에서 OnConnect, Close()에서 OnDisconnect(1회)
//
// 콜백 스레딩(주의): 단일 outstanding recv라 OnMessage는 순서 보장. 단 송신
// 오류 등으로 인한 OnDisconnect가 다른 워커의 OnMessage와 겹칠 수 있음 →
// per-session 콜백 직렬화는 Phase 4 보강 항목. (에코 등 무상태 앱은 무해)
//
// 미검증: Windows/MSVC 전용.
// =============================================================================

#include "helix/types.h"
#include "helix/session.h"
#include "../net/INetBackend.h"
#include "../proto/FrameCodec.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace helix { class IServerApp; }

namespace helix::core
{
	class SessionManager;

	class Session final : public helix::Session
	{
	public:
		Session(helix::SessionId id, net::NativeHandle sock,
		        net::INetBackend& backend, SessionManager& mgr,
		        std::uint32_t recvBufSize, const proto::FrameCodec& codec,
		        helix::IServerApp* app, const helix::Endpoint& remote,
		        std::uint32_t maxSendQueue);
		~Session() override;

		// --- 공개 helix::Session ---
		[[nodiscard]] helix::SessionId Id() const noexcept override { return _id; }
		helix::Status Send(helix::ByteSpan message) override;
		void Close(helix::SessionCloseReason reason) override;
		[[nodiscard]] helix::Endpoint Remote() const override { return _remote; }
		[[nodiscard]] helix::TransportType Transport() const noexcept override { return helix::TransportType::Tcp; }
		void SetUserData(void* p) noexcept override { _userData = p; }
		[[nodiscard]] void* GetUserData() const noexcept override { return _userData; }

		// --- 내부 ---
		[[nodiscard]] net::NativeHandle Socket() const noexcept { return _sock; }
		bool Start();   // OnConnect + 첫 recv

		void AddRef() noexcept;
		void Release() noexcept;

		void OnRecvComplete(std::uint32_t bytes);
		void OnSendComplete(std::uint32_t bytes);

	private:
		bool PostRecv();
		// 이미 프레이밍된 바이트를 큐잉. 백프레셔/포스트 실패 시 false(보상 Release는 내부 처리).
		bool EnqueueRaw(std::vector<std::byte>&& framed);
		bool StartNextSendLocked();
		bool PostSendLocked();
		void OnData(const std::byte* data, std::uint32_t len);  // 프레이밍 → OnMessage

		helix::SessionId _id;
		std::atomic<net::NativeHandle> _sock;
		net::INetBackend& _backend;
		SessionManager& _mgr;

		proto::FrameCodec _codec;
		proto::FrameReader _reader;
		helix::IServerApp* _app;
		helix::Endpoint _remote;
		void* _userData = nullptr;
		std::uint32_t _maxSendQueue;

		net::IoContext* _recvCtx = nullptr;
		net::IoContext* _sendCtx = nullptr;
		std::vector<std::byte> _recvBuf;

		std::mutex _sendLock;
		std::mutex _ioLock; // serialize socket close against new posts
		std::deque<std::vector<std::byte>> _sendQueue;
		std::vector<std::byte> _sendInFlight;
		bool _sendBusy = false;
		std::size_t _sendOffset = 0;

		std::atomic<int> _ref{ 1 };
		std::atomic<bool> _closed{ false };
	};
}
