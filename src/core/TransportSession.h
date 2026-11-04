#pragma once

// =============================================================================
// Phase 7 — TransportSession (ITransport 기반 세션, 공개 helix::Session 구현)
//
// TCP 경로의 core::Session 이 소켓+INetBackend에 직접 묶이는 것과 달리, KCP/QUIC
// 같은 ITransport는 ConnId 기반 콜백(OnAccepted/OnData/OnClosed)으로 동작한다.
// 이 세션은 그 위에서 공개 helix::Session 계약을 구현한다:
//   - Send: 프레이밍(길이 헤더) 후 ITransport::Send 로 전달
//   - 수신: ServerCore(ITransportSink)가 OnData를 받아 FrameReader로 디프레이밍
//   - 수명: ServerCore가 shared_ptr로 소유(콜백 재진입 시 use-after-free 방지)
//
// 코어가 OnConnect/OnMessage/OnDisconnect 호출을 주도하므로 이 클래스는 콜백을
// 직접 부르지 않는다(SRP).
// =============================================================================

#include "helix/session.h"
#include "helix/types.h"

#include "../proto/FrameCodec.h"
#include "../transport/ITransport.h"

#include <atomic>

namespace helix::core
{
	class TransportSession final : public helix::Session
	{
	public:
		TransportSession(helix::SessionId id, transport::ITransport& t, transport::ConnId conn,
		                 const proto::FrameCodec& codec, const helix::Endpoint& remote,
		                 helix::TransportType kind);

		// --- 공개 helix::Session ---
		[[nodiscard]] helix::SessionId Id() const noexcept override { return _id; }
		helix::Status Send(helix::ByteSpan message) override;
		void Close(helix::SessionCloseReason reason) override;
		[[nodiscard]] helix::Endpoint Remote() const override { return _remote; }
		[[nodiscard]] helix::TransportType Transport() const noexcept override { return _kind; }
		void SetUserData(void* p) noexcept override { _userData = p; }
		[[nodiscard]] void* GetUserData() const noexcept override { return _userData; }

		// --- 내부(ServerCore 전용) ---
		[[nodiscard]] transport::ConnId Conn() const noexcept { return _conn; }
		[[nodiscard]] proto::FrameReader& Reader() noexcept { return _reader; }
		// 종료를 1회만 확정(OnDisconnect 중복 방지). 최초 호출 true.
		bool MarkClosed() noexcept { return !_closed.exchange(true, std::memory_order_acq_rel); }

	private:
		helix::SessionId _id;
		transport::ITransport& _t;
		transport::ConnId _conn;
		proto::FrameCodec _codec;
		proto::FrameReader _reader;
		helix::Endpoint _remote;
		helix::TransportType _kind;
		void* _userData = nullptr;
		std::atomic<bool> _closed{ false };
	};
}
