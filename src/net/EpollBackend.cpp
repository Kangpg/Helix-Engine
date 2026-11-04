#if !defined(_WIN32)
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif
#include "EpollBackend.h"
#include "PlatformNet.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <new>
#include <system_error>

namespace helix::net
{
	namespace
	{
		struct EpollIoContext final : IoContext { std::uint32_t sentOffset = 0; };
		bool SetNonBlocking(int fd) noexcept
		{
			const int flags = ::fcntl(fd, F_GETFL, 0);
			if (flags == -1) { return false; }
			return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
		}
		std::error_code Errno(int e) noexcept { return std::error_code(e, std::generic_category()); }
	}

	EpollBackend::~EpollBackend() { Shutdown(); }
	void EpollBackend::CloseSocket(NativeHandle handle) { if (handle != kInvalidHandle) { ::close(static_cast<int>(handle)); } }

	bool EpollBackend::Init(const BackendConfig& cfg)
	{
		_maxConnections = cfg.maxConnections;
		_epfd = ::epoll_create1(EPOLL_CLOEXEC);
		if (_epfd < 0) { return false; }
		_wakefd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE);
		if (_wakefd < 0) { ::close(_epfd); _epfd = -1; return false; }
		epoll_event ev{};
		ev.events = EPOLLIN;
		ev.data.ptr = nullptr;
		if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, _wakefd, &ev) != 0)
		{
			::close(_wakefd); ::close(_epfd); _wakefd = _epfd = -1; return false;
		}
		return true;
	}

	void EpollBackend::Shutdown()
	{
		std::lock_guard<std::mutex> lk(_mtx);
		_fds.clear();
		if (_wakefd >= 0) { ::close(_wakefd); _wakefd = -1; }
		if (_epfd >= 0) { ::close(_epfd); _epfd = -1; }
	}

	bool EpollBackend::Register(NativeHandle handle)
	{
		if (_epfd < 0 || handle == kInvalidHandle) { return false; }
		const int fd = static_cast<int>(handle);
		if (!SetNonBlocking(fd)) { return false; }
		std::lock_guard<std::mutex> lk(_mtx);
		auto st = std::make_unique<FdState>();
		st->fd = fd;
		FdState* raw = st.get();
		_fds[fd] = std::move(st);
		epoll_event ev{};
		ev.events = EPOLLONESHOT;
		ev.data.ptr = raw;
		if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
		{
			if (errno != EEXIST || ::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev) != 0)
			{
				_fds.erase(fd); return false;
			}
		}
		return true;
	}

	IoContext* EpollBackend::CreateContext(IoOp op)
	{
		auto* ctx = new (std::nothrow) EpollIoContext{};
		if (ctx != nullptr) { ctx->op = op; }
		return ctx;
	}
	void EpollBackend::DestroyContext(IoContext* ctx) { delete static_cast<EpollIoContext*>(ctx); }

	bool EpollBackend::RearmLocked(FdState& st)
	{
		std::uint32_t events = EPOLLONESHOT;
		if (st.pendingRecv != nullptr) { events |= EPOLLIN; }
		if (st.pendingSend != nullptr) { events |= EPOLLOUT; }
		epoll_event ev{};
		ev.events = events;
		ev.data.ptr = &st;
		return ::epoll_ctl(_epfd, EPOLL_CTL_MOD, st.fd, &ev) == 0;
	}

	bool EpollBackend::ArmRecv(IoContext* ctx)
	{
		if (_epfd < 0 || ctx == nullptr) { return false; }
		std::lock_guard<std::mutex> lk(_mtx);
		const auto it = _fds.find(static_cast<int>(ctx->handle));
		if (it == _fds.end()) { return false; }
		it->second->pendingRecv = ctx;
		return RearmLocked(*it->second);
	}
	bool EpollBackend::ArmSend(IoContext* ctx)
	{
		if (_epfd < 0 || ctx == nullptr) { return false; }
		std::lock_guard<std::mutex> lk(_mtx);
		const auto it = _fds.find(static_cast<int>(ctx->handle));
		if (it == _fds.end()) { return false; }
		it->second->pendingSend = ctx;
		return RearmLocked(*it->second);
	}

	bool EpollBackend::PostAccept(IoContext* ctx) { ctx->op = IoOp::Accept; return ArmRecv(ctx); }
	bool EpollBackend::PostRecv(IoContext* ctx) { ctx->op = IoOp::Recv; return ArmRecv(ctx); }
	bool EpollBackend::PostSend(IoContext* ctx)
	{
		ctx->op = IoOp::Send;
		static_cast<EpollIoContext*>(ctx)->sentOffset = 0;
		return ArmSend(ctx);
	}

	int EpollBackend::Poll(IoCompletion* out, int maxOut, int timeoutMs)
	{
		if (_epfd < 0 || out == nullptr || maxOut <= 0) { return 0; }
		constexpr int kCap = 128;
		epoll_event evs[kCap];
		const int want = maxOut < kCap ? maxOut : kCap;
		const int n = ::epoll_wait(_epfd, evs, want, timeoutMs);
		if (n <= 0) { return 0; }

		int filled = 0;
		for (int i = 0; i < n && filled < maxOut; ++i)
		{
			const epoll_event& ev = evs[i];
			if (ev.data.ptr == nullptr)
			{
				std::uint64_t v = 0;
				const ssize_t r = ::read(_wakefd, &v, sizeof(v)); (void)r;
				out[filled++] = IoCompletion{ nullptr, 0, std::error_code{} };
				continue;
			}
			FdState* st = static_cast<FdState*>(ev.data.ptr);
			const std::uint32_t e = ev.events;
			const bool err = (e & (EPOLLERR | EPOLLHUP)) != 0;

			IoContext* rc = nullptr;
			IoContext* sc = nullptr;
			{
				std::lock_guard<std::mutex> lk(_mtx);
				if ((e & EPOLLIN) || err)  { rc = st->pendingRecv; st->pendingRecv = nullptr; }
				if ((e & EPOLLOUT) || err) { sc = st->pendingSend; st->pendingSend = nullptr; }
			}

			bool reRecv = false;
			bool reSend = false;

			if (rc != nullptr)
			{
				if (filled >= maxOut) { reRecv = true; }
				else if (rc->op == IoOp::Accept)
				{
					sockaddr_in sa{};
					socklen_t sl = sizeof(sa);
					const int c = ::accept4(st->fd, reinterpret_cast<sockaddr*>(&sa), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
					if (c >= 0) { rc->acceptSocket = static_cast<NativeHandle>(c); out[filled++] = IoCompletion{ rc, 0, std::error_code{} }; }
					else if (errno == EAGAIN || errno == EWOULDBLOCK) { reRecv = true; }
					else { rc->acceptSocket = kInvalidHandle; out[filled++] = IoCompletion{ rc, 0, Errno(errno) }; }
				}
				else
				{
					const ssize_t r = ::recv(st->fd, rc->buffer, rc->length, 0);
					if (r > 0) { out[filled++] = IoCompletion{ rc, static_cast<std::uint32_t>(r), std::error_code{} }; }
					else if (r == 0) { out[filled++] = IoCompletion{ rc, 0, std::error_code{} }; }
					else if (errno == EAGAIN || errno == EWOULDBLOCK) { reRecv = true; }
					else { out[filled++] = IoCompletion{ rc, 0, Errno(errno) }; }
				}
			}

			if (sc != nullptr)
			{
				if (filled >= maxOut) { reSend = true; }
				else
				{
					auto* ec = static_cast<EpollIoContext*>(sc);
					std::uint32_t off = ec->sentOffset;
					int sendErr = 0;
					bool again = false;
					while (off < sc->length)
					{
						const ssize_t w = ::send(st->fd, sc->buffer + off, sc->length - off, MSG_NOSIGNAL);
						if (w > 0) { off += static_cast<std::uint32_t>(w); }
						else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { again = true; break; }
						else { sendErr = (w < 0) ? errno : EIO; break; }
					}
					ec->sentOffset = off;
					if (off >= sc->length) { out[filled++] = IoCompletion{ sc, sc->length, std::error_code{} }; }
					else if (again) { reSend = true; }
					else { out[filled++] = IoCompletion{ sc, 0, Errno(sendErr) }; }
				}
			}

			{
				std::lock_guard<std::mutex> lk(_mtx);
				if (reRecv) { st->pendingRecv = rc; }
				if (reSend) { st->pendingSend = sc; }
				RearmLocked(*st);
			}
		}
		return filled;
	}

	void EpollBackend::Wake(int count)
	{
		if (_wakefd < 0) { return; }
		std::uint64_t v = (count > 0) ? static_cast<std::uint64_t>(count) : 1;
		const ssize_t r = ::write(_wakefd, &v, sizeof(v)); (void)r;
	}
}
#endif
