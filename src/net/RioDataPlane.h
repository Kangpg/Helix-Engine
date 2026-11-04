#pragma once
#include "WindowsIocpBackend.h"
#include <mutex>
#include <unordered_map>

namespace helix::net
{
	class RioDataPlane final : public IIocpCompletionSource
	{
	public:
		static constexpr ULONG_PTR CompletionKey = 0xFFFFFFFEull;
		~RioDataPlane();
		bool Init(HANDLE iocp, const BackendConfig& config);
		void Shutdown(); // only after workers, contexts and pending operations have drained
		NativeHandle CreateAcceptSocket();
		bool Register(NativeHandle handle);
		void CloseSocket(NativeHandle handle);
		IoContext* CreateContext(IoOp op);
		void DestroyContext(IoContext* context);
		bool Post(IoContext* context);
		int Drain(IoCompletion* out, int capacity) override;
	private:
		RIO_EXTENSION_FUNCTION_TABLE _functions{};
		RIO_CQ _cq = RIO_INVALID_CQ;
		OVERLAPPED _notification{};
		ULONG _bufferSize = 0;
		std::mutex _requestLock; // RIO receive/send on one RQ require synchronization
		std::mutex _completionLock;
		std::unordered_map<NativeHandle, RIO_RQ> _requests;
	};
}
