#include "WindowsIocpBackend.h"

#include <WS2tcpip.h>
#include <array>
#include <new>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace helix::net
{
	namespace
	{
		// Wake() 신호용 완료 키. 실제 I/O 완료의 키(0)와 구분된다.
		constexpr ULONG_PTR kWakeKey = 0xFFFFFFFFull;

		// AcceptEx 주소 버퍼: 로컬/원격 각각 (sockaddr + 16). IPv6 여유 포함.
		constexpr DWORD kAddrLen = sizeof(::sockaddr_in6) + 16;

		// 플랫폼 세부를 품은 IoContext 파생 타입.
		struct WinIoContext final : IoContext
		{
			OVERLAPPED ov{};
			WSABUF wsabuf{};
			std::array<char, kAddrLen * 2> acceptAddr{}; // AcceptEx 출력 버퍼
		};

		[[nodiscard]] WinIoContext* FromOverlapped(OVERLAPPED* ov) noexcept
		{
			return reinterpret_cast<WinIoContext*>(
				reinterpret_cast<char*>(ov) - offsetof(WinIoContext, ov));
		}
	}

	WindowsIocpBackend::~WindowsIocpBackend()
	{
		Shutdown();
	}

	bool WindowsIocpBackend::Init(const BackendConfig& cfg)
	{
		if (_initialized.load(std::memory_order_acquire))
		{
			return true;
		}

		_iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
		if (_iocp == nullptr)
		{
			return false;
		}

		if (!LoadAcceptEx())
		{
			::CloseHandle(_iocp);
			_iocp = nullptr;
			return false;
		}

		_name = "iocp";

		_initialized.store(true, std::memory_order_release);
		return true;
	}

	void WindowsIocpBackend::Shutdown()
	{
		if (!_initialized.exchange(false, std::memory_order_acq_rel))
		{
			return;
		}
		if (_iocp != nullptr)
		{
			::CloseHandle(_iocp);
			_iocp = nullptr;
		}
		_acceptEx = nullptr;
		_source = nullptr;
	}

	bool WindowsIocpBackend::LoadAcceptEx()
	{
		// 임시 소켓으로 AcceptEx 함수 포인터를 WSAIoctl로 로드.
		SOCKET tmp = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (tmp == INVALID_SOCKET)
		{
			return false;
		}

		GUID guid = WSAID_ACCEPTEX;
		DWORD bytes = 0;
		const int rc = ::WSAIoctl(
			tmp, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&guid, sizeof(guid),
			&_acceptEx, sizeof(_acceptEx),
			&bytes, nullptr, nullptr);

		::closesocket(tmp);
		return rc != SOCKET_ERROR && _acceptEx != nullptr;
	}

	bool WindowsIocpBackend::Register(NativeHandle handle)
	{
		if (_iocp == nullptr || handle == kInvalidHandle)
		{
			return false;
		}
		// 완료 키 0 = 일반 I/O. 핸들별 컨텍스트는 OVERLAPPED 쪽에서 복원하므로 키는 0 고정.
		const HANDLE h = reinterpret_cast<HANDLE>(handle);
		return ::CreateIoCompletionPort(h, _iocp, 0, 0) == _iocp;
	}
	NativeHandle WindowsIocpBackend::CreateAcceptSocket()
	{
		const auto socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		return socket == INVALID_SOCKET ? kInvalidHandle : static_cast<NativeHandle>(socket);
	}
	void WindowsIocpBackend::CloseSocket(NativeHandle handle)
	{
		if (handle != kInvalidHandle) { ::closesocket(static_cast<SOCKET>(handle)); }
	}

	IoContext* WindowsIocpBackend::CreateContext(IoOp op)
	{
		auto* ctx = new (std::nothrow) WinIoContext{};
		if (ctx != nullptr)
		{
			ctx->op = op;
		}
		return ctx;
	}

	void WindowsIocpBackend::DestroyContext(IoContext* ctx)
	{
		delete static_cast<WinIoContext*>(ctx);
	}

	bool WindowsIocpBackend::PostAccept(IoContext* ctx)
	{
		auto* w = static_cast<WinIoContext*>(ctx);
		ZeroMemory(&w->ov, sizeof(OVERLAPPED));
		w->op = IoOp::Accept;

		DWORD received = 0;
		// dwReceiveDataLength = 0 → 연결 수립 즉시 완료(데이터 대기 안 함).
		const BOOL ok = _acceptEx(
			static_cast<SOCKET>(w->handle),         // listen 소켓
			static_cast<SOCKET>(w->acceptSocket),   // 미리 만든 수락 소켓
			w->acceptAddr.data(),
			0,
			kAddrLen, kAddrLen,
			&received,
			&w->ov);

		if (ok == FALSE && ::WSAGetLastError() != ERROR_IO_PENDING)
		{
			return false;
		}
		return true;
	}

	bool WindowsIocpBackend::PostRecv(IoContext* ctx)
	{
		auto* w = static_cast<WinIoContext*>(ctx);
		ZeroMemory(&w->ov, sizeof(OVERLAPPED));
		w->op = IoOp::Recv;
		w->wsabuf.buf = reinterpret_cast<CHAR*>(w->buffer);
		w->wsabuf.len = w->length;

		DWORD flags = 0;
		DWORD recvd = 0;
		const int rc = ::WSARecv(
			static_cast<SOCKET>(w->handle),
			&w->wsabuf, 1, &recvd, &flags, &w->ov, nullptr);

		if (rc == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
		{
			return false;
		}
		return true;
	}

	bool WindowsIocpBackend::PostSend(IoContext* ctx)
	{
		auto* w = static_cast<WinIoContext*>(ctx);
		ZeroMemory(&w->ov, sizeof(OVERLAPPED));
		w->op = IoOp::Send;
		w->wsabuf.buf = reinterpret_cast<CHAR*>(w->buffer);
		w->wsabuf.len = w->length;

		DWORD sent = 0;
		const int rc = ::WSASend(
			static_cast<SOCKET>(w->handle),
			&w->wsabuf, 1, &sent, 0, &w->ov, nullptr);

		if (rc == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
		{
			return false;
		}
		return true;
	}

	int WindowsIocpBackend::Poll(IoCompletion* out, int maxOut, int timeoutMs)
	{
		if (_iocp == nullptr || out == nullptr || maxOut <= 0)
		{
			return 0;
		}

		constexpr ULONG kCap = 128;
		std::array<OVERLAPPED_ENTRY, kCap> entries{};
		const ULONG want = static_cast<ULONG>(maxOut < static_cast<int>(kCap) ? maxOut : kCap);

		ULONG removed = 0;
		const DWORD timeout = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
		const BOOL ok = ::GetQueuedCompletionStatusEx(
			_iocp, entries.data(), want, &removed, timeout, FALSE);

		if (ok == FALSE)
		{
			// 타임아웃이면 0. 그 외는 오류이나 MVP에서는 0으로 취급(상위가 재시도).
			return 0;
		}

		int filled = 0;
		for (ULONG i = 0; i < removed && filled < maxOut; ++i)
		{
			const OVERLAPPED_ENTRY& e = entries[i];
			if (_source && e.lpCompletionKey == _sourceKey)
			{
				// Reserve one output for every later IOCP entry; never discard a removed completion.
				const int capacity = maxOut - filled - static_cast<int>(removed - i - 1);
				filled += _source->Drain(out + filled, capacity);
				continue;
			}

			if (e.lpCompletionKey == kWakeKey)
			{
				out[filled].ctx = nullptr;   // 깨우기/종료 신호
				out[filled].bytesTransferred = 0;
				out[filled].error = std::error_code{};
				++filled;
				continue;
			}

			WinIoContext* w = FromOverlapped(e.lpOverlapped);
			out[filled].ctx = w;
			out[filled].bytesTransferred = e.dwNumberOfBytesTransferred;
			out[filled].error = std::error_code{};
			if (e.Internal != 0)
			{
				DWORD transferred = 0, flags = 0;
				if (!::WSAGetOverlappedResult(static_cast<SOCKET>(w->handle), &w->ov, &transferred, FALSE, &flags))
					{ out[filled].error = std::error_code(::WSAGetLastError(), std::system_category()); }
			}
			++filled;
		}
		return filled;
	}

	void WindowsIocpBackend::Wake(int count)
	{
		if (_iocp == nullptr)
		{
			return;
		}
		for (int i = 0; i < count; ++i)
		{
			::PostQueuedCompletionStatus(_iocp, 0, kWakeKey, nullptr);
		}
	}
}
