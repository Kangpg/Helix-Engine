#include "TransportSession.h"

#include <cstring>
#include <vector>

namespace helix::core
{
	TransportSession::TransportSession(helix::SessionId id, transport::ITransport& t,
	                                   transport::ConnId conn, const proto::FrameCodec& codec,
	                                   const helix::Endpoint& remote, helix::TransportType kind)
		: _id(id), _t(t), _conn(conn), _codec(codec), _reader(codec), _remote(remote), _kind(kind)
	{
	}

	helix::Status TransportSession::Send(helix::ByteSpan message)
	{
		if (_closed.load(std::memory_order_acquire))
		{
			return helix::Status::InvalidSession;
		}

		if (message.empty())
		{
			return helix::Status::Ok;
		}

		const auto len = static_cast<std::uint32_t>(message.size());
		if (len > _codec.maxMessageSize)
		{
			return helix::Status::TransportError;
		}

		std::vector<std::byte> framed(static_cast<std::size_t>(_codec.headerSize) + len);
		if (!proto::EncodeHeader(_codec, len, framed.data()))
		{
			return helix::Status::TransportError;
		}

		std::memcpy(framed.data() + _codec.headerSize, message.data(), len);

		return _t.Send(_conn, helix::ByteSpan(framed.data(), framed.size()))
		           ? helix::Status::Ok
		           : helix::Status::TransportError;
	}

	void TransportSession::Close(helix::SessionCloseReason reason)
	{
		if (_closed.load(std::memory_order_acquire))
		{
			return;  // 이미 닫힘(또는 닫는 중)
		}
		// 전송에 연결 종료를 요청 → ITransportSink::OnClosed 로 되돌아와
		// ServerCore가 MarkClosed/OnDisconnect/정리를 1회 수행한다.
		_t.Close(_conn, reason);
	}
}
