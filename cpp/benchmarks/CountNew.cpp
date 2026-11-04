// Only the allocation probe links these overrides. The timed executable does not.
// Requested operator-new bytes/calls, not peak live memory or direct malloc/system calls.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation
{
	extern thread_local bool enabled;
	extern thread_local std::uint64_t bytes, count;
	void Record(std::size_t size) noexcept { if (enabled) { bytes += size; ++count; } }
	void* Allocate(std::size_t size)
	{
		if (void* p = std::malloc(size ? size : 1)) { Record(size); return p; }
		throw std::bad_alloc();
	}
	void* Aligned(std::size_t size, std::size_t alignment)
	{
#ifdef _WIN32
		void* p = _aligned_malloc(size ? size : 1, alignment);
#else
		void* p = nullptr; if (posix_memalign(&p, alignment, size ? size : 1) != 0) { p = nullptr; }
#endif
		if (!p) { throw std::bad_alloc(); } Record(size); return p;
	}
	void FreeAligned(void* p) noexcept
	{
#ifdef _WIN32
		_aligned_free(p);
#else
		std::free(p);
#endif
	}
}
void* operator new(std::size_t n) { return allocation::Allocate(n); }
void* operator new[](std::size_t n) { return allocation::Allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { return allocation::Aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocation::Aligned(n, static_cast<std::size_t>(a)); }
void operator delete(void* p, std::align_val_t) noexcept { allocation::FreeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { allocation::FreeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { allocation::FreeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { allocation::FreeAligned(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new[](n); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new[](n, a); } catch (...) { return nullptr; } }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation::FreeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation::FreeAligned(p); }
