#include "RioBuffer.h"
namespace helix::net
{
	RioBuffer::~RioBuffer()
	{
		if (_id != RIO_INVALID_BUFFERID) { _functions->RIODeregisterBuffer(_id); }
		if (_data) { ::VirtualFree(_data, 0, MEM_RELEASE); }
	}
	bool RioBuffer::Init(RIO_EXTENSION_FUNCTION_TABLE& functions, ULONG size)
	{
		if (_data) { return true; }
		_data = static_cast<char*>(::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		if (!_data) { return false; }
		_functions = &functions;
		_id = functions.RIORegisterBuffer(_data, size);
		if (_id == RIO_INVALID_BUFFERID) { ::VirtualFree(_data, 0, MEM_RELEASE); _data = nullptr; return false; }
		return true;
	}
}
