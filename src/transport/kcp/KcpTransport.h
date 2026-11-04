#pragma once

// =============================================================================
// Phase 7 — KCP 전송 (ITransport 구현, 저지연 신뢰 UDP)
//
// 원본 ikcp.c(skywind3000/kcp, MIT)를 임베드하고 그 위에 서버측 디먹스를 얹는다.
//   - UDP 소켓 1개에 다수 클라가 datagram 송신 → remote 주소로 연결 구분.
//   - 새 주소 = 새 연결(OnAccepted). ikcp_input→ikcp_recv 로 조립된 바이트를 OnData.
//   - 송신은 ikcp_send + flush, output 콜백이 sendto 로 내보냄.
//   - update 스레드가 주기적으로 ikcp_update(전 연결) + idle 타임아웃 정리.
//
// QUIC와 마찬가지로 자체 I/O(전용 UDP 소켓 + recv/update 스레드)를 가지며
// INetBackend(IOCP) 아래가 아니라 병렬로 배치된다(backend 인자는 무시).
// 프레이밍은 코어(FrameReader)가 그대로 담당 — KCP는 바이트만 운반.
//
// 플랫폼: 현재 Windows(WinSock2) 전용. 헤더에 WinSock 타입 비노출.
// =============================================================================

#include "../ITransport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace helix::transport
{
	struct KcpConn;   // .cpp 정의(ikcpcb + sockaddr 보관)

	class KcpTransport final : public ITransport
	{
	public:
		KcpTransport();
		~KcpTransport() override;

		bool Init(const TransportConfig& cfg, ITransportSink& sink,
		          helix::net::INetBackend* backend) override;   // backend 무시
		bool Listen() override;
		void Stop() override;

		bool Send(ConnId conn, helix::ByteSpan bytes) override;
		void Close(ConnId conn, helix::SessionCloseReason reason) override;

		[[nodiscard]] helix::TransportType Kind() const noexcept override
		{
			return helix::TransportType::Kcp;
		}

	private:
		void RecvLoop();
		void UpdateLoop();
		[[nodiscard]] std::uint32_t NowMs() const noexcept;

		TransportConfig _cfg;
		ITransportSink* _sink = nullptr;

		std::uintptr_t _sock;             // UDP 소켓 (INVALID_SOCKET 초기화는 .cpp)
		std::atomic<bool> _running{ false };
		std::thread _recvThread;
		std::thread _updateThread;

		std::mutex _mx;                   // _conns/_byKey + 모든 ikcpcb 접근 직렬화(ikcp는 비스레드안전)
		std::unordered_map<ConnId, std::unique_ptr<KcpConn>> _conns;
		std::unordered_map<std::string, ConnId> _byKey;  // sockaddr 바이트 → ConnId
		std::atomic<ConnId> _nextConn{ 1 };

		std::uint64_t _startTick = 0;     // NowMs 기준점
		std::uint32_t _idleTimeoutMs = 30000;
	};
}
