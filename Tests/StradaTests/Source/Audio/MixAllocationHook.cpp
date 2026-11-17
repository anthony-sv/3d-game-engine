#include "Audio/MixAllocationHook.h"

#include "Strada/Audio/AudioEngine.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

// A sanitizer's runtime (AddressSanitizer and ThreadSanitizer: the STRADA_ENABLE_SANITIZERS and
// STRADA_ENABLE_THREAD_SANITIZER builds, which define ST_SANITIZER_BUILD) defines every global allocation function itself,
// to watch the memory they hand out. Clang on Linux links that runtime statically, so a second definition here fails the
// link ("multiple definition of operator new"); the other compilers' runtimes define them as well. In every sanitizer
// build the hook steps aside and leaves them to the runtime: nothing is counted, and the cases that would compare the
// count say they skip it.
#if defined(ST_SANITIZER_BUILD)

bool Strada::Testing::IsMixingThreadAllocationCounted()
{
	return false;
}

uint64_t Strada::Testing::GetMixingThreadAllocationCount()
{
	return 0;
}

#else

// The global allocation functions of the test program, replaced so that a test can count what the mixing thread
// allocates or frees while it mixes (Docs/Architecture.md §9.9). They allocate as the default ones do (malloc and free, the
// aligned forms through the platform's aligned allocation), so memory one form allocates any form of the same family
// frees. IsMixingOnThisThread reads one atomic in static storage, so these may run before main, on any thread.
namespace
{
	constinit std::atomic<uint64_t> s_MixingThreadAllocations{0};

	void Count()
	{
		if (Strada::AudioEngine::IsMixingOnThisThread())
		{
			s_MixingThreadAllocations.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void* Allocate(std::size_t size)
	{
		Count();
		return std::malloc(size != 0 ? size : 1);
	}

	void* AllocateAligned(std::size_t size, std::align_val_t alignment)
	{
		Count();
		std::size_t const bytes = size != 0 ? size : 1;
		std::size_t const align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
		return _aligned_malloc(bytes, align);
#else
		// aligned_alloc wants a size that is a multiple of the alignment.
		return std::aligned_alloc(align, (bytes + align - 1) / align * align);
#endif
	}

	void Free(void* memory)
	{
		Count();
		std::free(memory);
	}

	void FreeAligned(void* memory)
	{
		Count();
#if defined(_MSC_VER)
		_aligned_free(memory);
#else
		std::free(memory);
#endif
	}
}

bool Strada::Testing::IsMixingThreadAllocationCounted()
{
	return true;
}

uint64_t Strada::Testing::GetMixingThreadAllocationCount()
{
	return s_MixingThreadAllocations.load(std::memory_order_relaxed);
}

void* operator new(std::size_t size)
{
	if (void* const memory = Allocate(size))
	{
		return memory;
	}
	throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
	if (void* const memory = Allocate(size))
	{
		return memory;
	}
	throw std::bad_alloc();
}

void* operator new(std::size_t size, std::nothrow_t const&) noexcept
{
	return Allocate(size);
}

void* operator new[](std::size_t size, std::nothrow_t const&) noexcept
{
	return Allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
	if (void* const memory = AllocateAligned(size, alignment))
	{
		return memory;
	}
	throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
	if (void* const memory = AllocateAligned(size, alignment))
	{
		return memory;
	}
	throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment, std::nothrow_t const&) noexcept
{
	return AllocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, std::nothrow_t const&) noexcept
{
	return AllocateAligned(size, alignment);
}

void operator delete(void* memory) noexcept
{
	Free(memory);
}

void operator delete[](void* memory) noexcept
{
	Free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
	Free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
	Free(memory);
}

void operator delete(void* memory, std::nothrow_t const&) noexcept
{
	Free(memory);
}

void operator delete[](void* memory, std::nothrow_t const&) noexcept
{
	Free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
	FreeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
	FreeAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
	FreeAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
	FreeAligned(memory);
}

void operator delete(void* memory, std::align_val_t, std::nothrow_t const&) noexcept
{
	FreeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t, std::nothrow_t const&) noexcept
{
	FreeAligned(memory);
}

#endif
