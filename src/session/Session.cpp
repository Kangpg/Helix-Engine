#include "Session.h"
#include "SessionManager.h"

#include "helix/server_app.h"

#include "../net/PlatformNet.h"
#include <cstring>
#include <utility>

namespace helix::core
{
	Session::Session(helix::SessionId id, net::NativeHandle sock,
	                 net::INetBackend& backend, SessionManager& mgr,
	                 std::uint32_t recvBufSize, const proto::FrameCodec& codec,
	                 helix::IServerApp* app, const helix::Endpoint& remote,
	                 std::uint32_t maxSendQueue)
		: _id(id), _sock(sock), _backend(backend), _mgr(mgr),
		  _codec(codec), _reader(codec), _app(app), _remote(remote),
		  _maxSendQueue(maxSendQueue)
	{
		_recvBuf.resize(recvBufSize);
		_recvCtx = _backend.CreateContext(net::IoOp::Recv);
		_sendCtx = _backend.CreateContext(net::IoOp::Send);
		if (_recvCtx != nullptr) { _recvCtx->user = this; _recvCtx->handle = _sock; }
		if (_sendCtx != nullptr) { _sendCtx->user = this; _sendCtx->handle = _sock; }
	}

	Session::~Session()
	{
		if (_recvCtx != nullptr) { _backend.DestroyContext(_recvCtx); }
		if (_sendCtx != nullptr) { _backend.DestroyContext(_sendCtx); }
		if (_sock != net::kInvalidHandle) { _backend.CloseSocket(_sock); _sock = net::kInvalidHandle; }
	}

	void Session::AddRef() noexcept { _ref.fetch_add(1, std::memory_order_relaxed); }

	void Session::Release() noexcept
	{
		if (_ref.fetch_sub(1, std::memory_order_acq_rel) == 1) { _mgr.Remove(_id); delete this; }
	}

	bool Session::Start()
	{
		if (!_recvCtx || !_sendCtx || _closed.load(std::memory_order_acquire)) { return false; }
		if (_app != nullptr) { _app->OnConnect(*this); }
		return PostRecv();
	}

	bool Session::PostRecv()
	{
		bool posted = false;
		{
			std::lock_guard lock(_ioLock);
			if (!_recvCtx || _closed.load(std::memory_order_acquire)) { return false; }
			_recvCtx->buffer = _recvBuf.data();
			_recvCtx->length = static_cast<std::uint32_t>(_recvBuf.size());
			AddRef();
			posted = _backend.PostRecv(_recvCtx);
		}
		if (!posted) { Release(); }
		return posted;
	}

	void Session::OnRecvComplete(std::uint32_t bytes)
	{
		if (bytes == 0 || _closed.load(std::memory_order_acquire))
		{
			Close(helix::SessionCloseReason::PeerClosed);
			Release();
			return;
		}
		OnData(_recvBuf.data(), bytes);
		if (!_closed.load(std::memory_order_acquire))
		{
			if (!PostRecv()) { Close(helix::SessionCloseReason::IoError); }
		}
		Release();
	}

	void Session::OnData(const std::byte* data, std::uint32_t len)
	{
		const bool ok = _reader.Feed(data, len,
			[this](const std::byte* payload, std::uint32_t plen)
			{
				if (_app != nullptr) { _app->OnMessage(*this, helix::ByteSpan(payload, plen)); }
			});
		if (!ok) { Close(helix::SessionCloseReason::ProtocolError); }
	}

	helix::Status Session::Send(helix::ByteSpan message)
	{
		if (_closed.load(std::memory_order_acquire)) { return helix::Status::InvalidSession; }
		if (message.empty()) { return helix::Status::Ok; }
		const std::uint32_t len = static_cast<std::uint32_t>(message.size());
		if (len > _codec.maxMessageSize) { return helix::Status::TransportError; }
		std::vector<std::byte> framed(static_cast<std::size_t>(_codec.headerSize) + len);
		if (!proto::EncodeHeader(_codec, len, framed.data())) { return helix::Status::TransportError; }
		std::memcpy(framed.data() + _codec.headerSize, message.data(), len);
		return EnqueueRaw(std::move(framed)) ? helix::Status::Ok : helix::Status::BackpressureFull;
	}

	bool Session::EnqueueRaw(std::vector<std::byte>&& framed)
	{
		bool failedPost = false;
		{
			std::unique_lock<std::mutex> lk(_sendLock);
			if (_closed.load(std::memory_order_acquire)) { return false; }
			if (_sendQueue.size() >= _maxSendQueue) { return false; }
			_sendQueue.emplace_back(std::move(framed));
			if (!_sendBusy) { if (!StartNextSendLocked()) { failedPost = true; } }
		}
		if (failedPost) { Close(helix::SessionCloseReason::IoError); Release(); return false; }
		return true;
	}

	bool Session::StartNextSendLocked()
	{
		if (_sendQueue.empty() || _sendCtx == nullptr || _closed.load(std::memory_order_acquire))
		{
			_sendBusy = false;
			return true;
		}
		_sendInFlight = std::move(_sendQueue.front());
		_sendQueue.pop_front();
		_sendBusy = true;
		_sendOffset = 0;
		return PostSendLocked();
	}
	bool Session::PostSendLocked()
	{
		AddRef();
		std::lock_guard lock(_ioLock);
		if (_closed.load(std::memory_order_acquire)) { _sendBusy = false; return false; }
		_sendCtx->buffer = _sendInFlight.data() + _sendOffset;
		_sendCtx->length = static_cast<std::uint32_t>(_sendInFlight.size() - _sendOffset);
		if (!_backend.PostSend(_sendCtx)) { _sendBusy = false; return false; }
		return true;
	}

	void Session::OnSendComplete(std::uint32_t bytes)
	{
		if (bytes == 0 || _closed.load(std::memory_order_acquire))
		{
			Close(helix::SessionCloseReason::IoError);
			Release();
			return;
		}
		bool failedPost = false;
		bool invalidCompletion = false;
		{
			std::unique_lock<std::mutex> lk(_sendLock);
			if (bytes > _sendInFlight.size() - _sendOffset) { invalidCompletion = true; }
			else
			{
				_sendOffset += bytes;
				if (_sendOffset < _sendInFlight.size()) { failedPost = !PostSendLocked(); }
				else { _sendInFlight.clear(); failedPost = !StartNextSendLocked(); }
			}
		}
		if (failedPost) { Close(helix::SessionCloseReason::IoError); Release(); }
		if (invalidCompletion) { Close(helix::SessionCloseReason::IoError); }
		Release();
	}

	void Session::Close(helix::SessionCloseReason reason)
	{
		{
			std::lock_guard lock(_ioLock);
			if (_closed.exchange(true, std::memory_order_acq_rel)) { return; }
			if (_sock != net::kInvalidHandle) { _backend.CloseSocket(_sock); _sock = net::kInvalidHandle; }
		}
		if (_app != nullptr) { _app->OnDisconnect(*this, reason); }
		Release();
	}
}
