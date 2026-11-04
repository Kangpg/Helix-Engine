#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include <Windows.h>

namespace helix::net
{
	// Registered storage outlives every request using it, including cancellation.
	class RioBuffer final
	{
	public:
		RioBuffer() = default;
		~RioBuffer();
		RioBuffer(const RioBuffer&) = delete;
		RioBuffer& operator=(const RioBuffer&) = delete;
		bool Init(RIO_EXTENSION_FUNCTION_TABLE& functions, ULONG size);
		char* Data() const noexcept { return _data; }
		RIO_BUFFERID Id() const noexcept { return _id; }
	private:
		RIO_EXTENSION_FUNCTION_TABLE* _functions = nullptr;
		char* _data = nullptr;
		RIO_BUFFERID _id = RIO_INVALID_BUFFERID;
	};
}
