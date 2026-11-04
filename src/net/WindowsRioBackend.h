#pragma once
#include "WindowsIocpBackend.h"
#include "RioDataPlane.h"

namespace helix::net
{
	// Composition: IOCP controls AcceptEx/notifications; RIO owns only the data plane.
	class WindowsRioBackend final : public INetBackend
	{
	public:
		~WindowsRioBackend() override { Shutdown(); }
		bool Init(const BackendConfig& config) override;
		void Shutdown() override;
		bool Register(NativeHandle handle) override;
		NativeHandle CreateAcceptSocket() override { return _rio.CreateAcceptSocket(); }
		void CloseSocket(NativeHandle handle) override { _rio.CloseSocket(handle); }
		IoContext* CreateContext(IoOp op) override { return op == IoOp::Accept ? _iocp.CreateContext(op) : _rio.CreateContext(op); }
		void DestroyContext(IoContext* context) override;
		bool PostAccept(IoContext* context) override { return _iocp.PostAccept(context); }
		bool PostRecv(IoContext* context) override { return _rio.Post(context); }
		bool PostSend(IoContext* context) override { return _rio.Post(context); }
		int Poll(IoCompletion* out, int capacity, int timeout) override { return _iocp.Poll(out, capacity, timeout); }
		void Wake(int count = 1) override { _iocp.Wake(count); }
		const char* Name() const noexcept override { return "iocp+rio"; }
	private:
		WindowsIocpBackend _iocp;
		RioDataPlane _rio;
	};
}
