#include "WindowsRioBackend.h"
#include "../log/Log.h"
namespace helix::net
{
	bool WindowsRioBackend::Init(const BackendConfig& config)
	{
		if (!_iocp.Init(config)) { return false; }
		if (!_rio.Init(_iocp.CompletionPort(), config))
		{
			HLOG_ERROR("iocp_rio unavailable or resources insufficient; choose iocp explicitly for compatibility");
			Shutdown(); return false;
		}
		_iocp.SetCompletionSource(RioDataPlane::CompletionKey, &_rio);
		return true;
	}
	void WindowsRioBackend::Shutdown() { _iocp.SetCompletionSource(0, nullptr); _rio.Shutdown(); _iocp.Shutdown(); }
	bool WindowsRioBackend::Register(NativeHandle handle)
	{
		int listening = 0, size = sizeof(listening);
		if (::getsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&listening), &size) == SOCKET_ERROR) { return false; }
		return listening ? _iocp.Register(handle) : _rio.Register(handle);
	}
	void WindowsRioBackend::DestroyContext(IoContext* context)
	{
		if (context->op == IoOp::Accept) { _iocp.DestroyContext(context); }
		else { _rio.DestroyContext(context); }
	}
}
