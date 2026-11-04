#include "ITransport.h"
#include "kcp/KcpTransport.h"

namespace helix::transport
{
	// TransportType → 구현. TCP는 코어의 기존 INetBackend 경로를 쓰므로 여기서
	// 생성하지 않는다(nullptr). QUIC은 후속(msquic) 단계에서 추가.
	std::unique_ptr<ITransport> CreateTransport(helix::TransportType kind)
	{
		switch (kind)
		{
		case helix::TransportType::Kcp:
			return std::make_unique<KcpTransport>();
		case helix::TransportType::Quic:   // TODO(Phase 7b): msquic
		case helix::TransportType::Tcp:    // 코어 기존 경로
		default:
			return nullptr;
		}
	}
}
