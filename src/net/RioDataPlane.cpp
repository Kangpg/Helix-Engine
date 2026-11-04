#include "RioDataPlane.h"
#include "RioBuffer.h"
#include "../log/Log.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <stdexcept>

namespace helix::net
{
	namespace
	{
		struct RioContext final : IoContext { RioBuffer storage; };
	}
	RioDataPlane::~RioDataPlane() { Shutdown(); }
	bool RioDataPlane::Init(HANDLE iocp, const BackendConfig& config)
	{
		if (!config.rioBufferSize || config.rioCompletionQueueSize < 2ull * config.maxConnections) { return false; }
		_bufferSize = config.rioBufferSize;
		const auto socket = CreateAcceptSocket();
		if (socket == kInvalidHandle) { return false; }
		GUID guid = WSAID_MULTIPLE_RIO;
		DWORD bytes = 0;
		const int result = ::WSAIoctl(static_cast<SOCKET>(socket), SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
			&guid, sizeof(guid), &_functions, sizeof(_functions), &bytes, nullptr, nullptr);
		::closesocket(static_cast<SOCKET>(socket));
		if (result == SOCKET_ERROR) { return false; }
		RIO_NOTIFICATION_COMPLETION notification{};
		notification.Type = RIO_IOCP_COMPLETION;
		notification.Iocp.IocpHandle = iocp;
		notification.Iocp.CompletionKey = reinterpret_cast<void*>(CompletionKey);
		notification.Iocp.Overlapped = &_notification;
		_cq = _functions.RIOCreateCompletionQueue(config.rioCompletionQueueSize, &notification);
		if (_cq == RIO_INVALID_CQ) { return false; }
		if (_functions.RIONotify(_cq) != ERROR_SUCCESS) { Shutdown(); return false; }
		return true;
	}
	void RioDataPlane::Shutdown()
	{
		if (_cq != RIO_INVALID_CQ) { _functions.RIOCloseCompletionQueue(_cq); _cq = RIO_INVALID_CQ; }
		_requests.clear();
	}
	NativeHandle RioDataPlane::CreateAcceptSocket()
	{
		const auto socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
			WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO);
		return socket == INVALID_SOCKET ? kInvalidHandle : static_cast<NativeHandle>(socket);
	}
	bool RioDataPlane::Register(NativeHandle handle)
	{
		std::lock_guard lock(_requestLock);
		const auto queue = _functions.RIOCreateRequestQueue(static_cast<SOCKET>(handle), 1, 1, 1, 1, _cq, _cq, nullptr);
		if (queue == RIO_INVALID_RQ) { return false; }
		_requests.emplace(handle, queue);
		return true;
	}
	void RioDataPlane::CloseSocket(NativeHandle handle)
	{
		std::lock_guard lock(_requestLock);
		_requests.erase(handle);
		if (handle != kInvalidHandle) { ::closesocket(static_cast<SOCKET>(handle)); }
	}
	IoContext* RioDataPlane::CreateContext(IoOp op)
	{
		auto* context = new (std::nothrow) RioContext();
		if (context) { context->op = op; }
		return context;
	}
	void RioDataPlane::DestroyContext(IoContext* context) { delete static_cast<RioContext*>(context); }
	bool RioDataPlane::Post(IoContext* context)
	{
		auto& rio = *static_cast<RioContext*>(context);
		if (!rio.buffer || !rio.length || !rio.storage.Init(_functions, _bufferSize)) { return false; }
		RIO_BUF buffer{ rio.storage.Id(), 0, std::min<ULONG>(rio.length, _bufferSize) };
		if (rio.op == IoOp::Send) { std::memcpy(rio.storage.Data(), rio.buffer, buffer.Length); }
		std::lock_guard lock(_requestLock);
		const auto it = _requests.find(rio.handle);
		if (it == _requests.end()) { return false; }
		const bool posted = rio.op == IoOp::Send
			? _functions.RIOSend(it->second, &buffer, 1, 0, &rio) != FALSE
			: _functions.RIOReceive(it->second, &buffer, 1, 0, &rio) != FALSE;
		if (!posted) { HLOG_ERROR("RIO post failed op=" + std::string(rio.op == IoOp::Send ? "send" : "recv") + " error=" + std::to_string(::WSAGetLastError())); }
		return posted;
	}
	int RioDataPlane::Drain(IoCompletion* out, int capacity)
	{
		std::lock_guard lock(_completionLock);
		std::array<RIORESULT, 128> results{};
		const auto count = _functions.RIODequeueCompletion(_cq, results.data(), static_cast<ULONG>(std::min(capacity, 128)));
		if (count == RIO_CORRUPT_CQ) { throw std::runtime_error("RIO completion queue corrupted"); }
		for (ULONG i = 0; i < count; ++i)
		{
			auto* context = reinterpret_cast<RioContext*>(static_cast<std::uintptr_t>(results[i].RequestContext));
			auto error = std::error_code(results[i].Status, std::system_category());
			if (results[i].BytesTransferred > std::min<ULONG>(context->length, _bufferSize))
				{ error = std::make_error_code(std::errc::message_size); }
			if (!error && context->op == IoOp::Recv)
				{ std::memcpy(context->buffer, context->storage.Data(), results[i].BytesTransferred); }
			out[i] = { context, results[i].BytesTransferred, error };
		}
		// This notification was removed from IOCP. Arm once, including when CQ remains nonempty.
		if (_functions.RIONotify(_cq) != ERROR_SUCCESS) { throw std::runtime_error("RIO notification rearm failed"); }
		return static_cast<int>(count);
	}
}
