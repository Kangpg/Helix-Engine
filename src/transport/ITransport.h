#pragma once

// =============================================================================
// 내부 추상화 #2 — 전송 계층 (TCP / KCP / QUIC)
//
// 세션/프레이밍 위에서 전송을 갈아끼운다. 동일 IServerApp이 전송과 무관하게
// 동작한다.
//   - TCP : INetBackend(IOCP/epoll)의 raw 소켓을 사용.
//   - KCP : ikcp.c 임베드 + UDP 소켓(INetBackend 위).
//   - QUIC: msquic 래핑. msquic은 자체 I/O 루프를 가지므로 INetBackend 아래가
//           아니라 별도 전송으로 병렬 배치된다(이 비대칭을 인터페이스가 흡수:
//           TCP/KCP는 backend를 주입받고, QUIC 구현은 무시한다).
//
// 콜백은 ITransportSink(코어의 SessionManager가 구현)로 올라간다.
// =============================================================================

#include "helix/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace helix::net { class INetBackend; }

namespace helix::transport
{
	// 전송 수준 연결 식별자(코어의 SessionId와 1:1 매핑되기 전 단계).
	using ConnId = std::uint64_t;
	inline constexpr ConnId kInvalidConn = 0;

	// 전송 → 코어 방향 콜백.
	class ITransportSink
	{
	public:
		virtual ~ITransportSink() = default;
		virtual void OnAccepted(ConnId conn, const helix::Endpoint& remote) = 0;
		// 프레이밍 이전의 raw 바이트 스트림(또는 KCP/QUIC 단위). 누적/프레이밍은 코어가 담당.
		virtual void OnData(ConnId conn, helix::ByteSpan bytes) = 0;
		virtual void OnClosed(ConnId conn, helix::SessionCloseReason reason) = 0;
	};

	struct TransportConfig
	{
		helix::Endpoint bind{};            // 리슨 주소
		std::uint32_t backlog = 128;
		std::uint32_t maxConnections = 10000;
		// QUIC/KCP 등 전송별 세부는 구현에서 설정 객체를 추가로 읽는다.
	};

	class ITransport
	{
	public:
		virtual ~ITransport() = default;

		// backend: TCP/KCP는 사용, QUIC 구현은 nullptr 허용.
		virtual bool Init(const TransportConfig& cfg, ITransportSink& sink,
		                   helix::net::INetBackend* backend) = 0;
		virtual bool Listen() = 0;
		virtual void Stop() = 0;

		// 전송 큐로 바이트 송신(세션별 직렬화/백프레셔는 코어 또는 구현에서).
		virtual bool Send(ConnId conn, helix::ByteSpan bytes) = 0;
		virtual void Close(ConnId conn, helix::SessionCloseReason reason) = 0;

		[[nodiscard]] virtual helix::TransportType Kind() const noexcept = 0;
	};

	// 팩토리: 설정의 TransportKind에 따라 구현 생성.
	std::unique_ptr<ITransport> CreateTransport(helix::TransportType kind);
}
