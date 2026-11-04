#include "Acceptor.h"
#include "../session/SessionManager.h"
#include "../session/Session.h"

#include "PlatformNet.h"

namespace helix::net
{
	Acceptor::Acceptor(INetBackend& backend, helix::core::SessionManager& mgr)
		: _backend(backend), _mgr(mgr) {}

	Acceptor::~Acceptor()
	{
		Stop();
		// ServerCore drains canceled AcceptEx before destroying this owner.
		for (auto* context : _acceptCtxs) { _backend.DestroyContext(context); }
	}

	NativeHandle Acceptor::CreateAcceptSocket() const
	{
		return _backend.CreateAcceptSocket();
	}

	bool Acceptor::Listen(const helix::Endpoint& ep, std::uint32_t backlog, int concurrentAccepts)
	{
		std::lock_guard lock(_acceptLock);
#if defined(_WIN32)
		// AcceptEx inherits the listener's I/O mode; both sockets need registered-I/O flags.
		const SOCKET listen = static_cast<SOCKET>(_backend.CreateAcceptSocket());
#else
		const SOCKET listen = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
#endif
		if (listen == INVALID_SOCKET) { return false; }
		{ const int one = 1; ::setsockopt(listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one)); }

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = ::htons(ep.port);
		if (ep.ip.empty() || ep.ip == "0.0.0.0") { addr.sin_addr.s_addr = INADDR_ANY; }
		else if (::inet_pton(AF_INET, ep.ip.c_str(), &addr.sin_addr) != 1) { ::closesocket(listen); return false; }

		if (::bind(listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
		    ::listen(listen, static_cast<int>(backlog)) == SOCKET_ERROR) { ::closesocket(listen); return false; }

		_listen = static_cast<NativeHandle>(listen);
		if (!_backend.Register(_listen)) { ::closesocket(listen); _listen = kInvalidHandle; return false; }

#if !defined(_WIN32)
		concurrentAccepts = 1;  // epoll: listen 준비 단위라 동시 accept 무의미
#endif
		for (int i = 0; i < concurrentAccepts; ++i)
		{
			auto* ctx = _backend.CreateContext(IoOp::Accept);
			if (ctx == nullptr) { continue; }
			ctx->user = this;
			_acceptCtxs.push_back(ctx);
			if (!PostOneAccept(ctx)) { _acceptCtxs.pop_back(); _backend.DestroyContext(ctx); }
		}
		return !_acceptCtxs.empty();
	}

	bool Acceptor::PostOneAccept(IoContext* ctx)
	{
		if (_stopped || _listen == kInvalidHandle) { return false; }
		const NativeHandle accSock = CreateAcceptSocket();
#if defined(_WIN32)
		if (accSock == kInvalidHandle) { return false; }
#endif
		ctx->op = IoOp::Accept;
		ctx->handle = _listen;
		ctx->acceptSocket = accSock;
		ctx->user = this;
		_pendingAccepts.fetch_add(1);
		if (!_backend.PostAccept(ctx))
		{
			_pendingAccepts.fetch_sub(1);
			_backend.CloseSocket(accSock);
			ctx->acceptSocket = kInvalidHandle;
			return false;
		}
		return true;
	}

	void Acceptor::OnAcceptComplete(IoContext* ctx, const std::error_code& ec)
	{
		std::unique_lock lock(_acceptLock);
		_pendingAccepts.fetch_sub(1);
		const SOCKET accepted = static_cast<SOCKET>(ctx->acceptSocket);
		if (ec || _stopped)
		{
			_backend.CloseSocket(ctx->acceptSocket);
			ctx->acceptSocket = kInvalidHandle;
			if (!_stopped) { PostOneAccept(ctx); }
			return;
		}
#if defined(_WIN32)
		const SOCKET listen = static_cast<SOCKET>(_listen);
		::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&listen), sizeof(listen));
#endif
		helix::core::Session* session = nullptr;
		if (_backend.Register(ctx->acceptSocket))
		{
			helix::Endpoint ep;
			sockaddr_in peer{};
			socklen_t plen = sizeof(peer);
			if (::getpeername(accepted, reinterpret_cast<sockaddr*>(&peer), &plen) == 0)
			{
				char ipbuf[INET_ADDRSTRLEN] = {};
				if (::inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf)) != nullptr) { ep.ip = ipbuf; }
				ep.port = ::ntohs(peer.sin_port);
			}
			session = _mgr.Create(ctx->acceptSocket, ep);
			if (session) { session->AddRef(); }
			else { _backend.CloseSocket(ctx->acceptSocket); }
		}
		else { _backend.CloseSocket(ctx->acceptSocket); }
		ctx->acceptSocket = kInvalidHandle;
		PostOneAccept(ctx);
		lock.unlock();
		// App callbacks may stop the server; never invoke them under the accept lock.
		if (session)
		{
			if (!session->Start()) { session->Close(helix::SessionCloseReason::IoError); }
			session->Release();
		}
	}

	void Acceptor::Stop()
	{
		std::lock_guard lock(_acceptLock);
		if (_stopped) { return; }
		_stopped = true;
		if (_listen != kInvalidHandle) { _backend.CloseSocket(_listen); _listen = kInvalidHandle; }
		for (auto* ctx : _acceptCtxs)
		{
			if (ctx->acceptSocket != kInvalidHandle) { _backend.CloseSocket(ctx->acceptSocket); ctx->acceptSocket = kInvalidHandle; }
		}
	}
}
