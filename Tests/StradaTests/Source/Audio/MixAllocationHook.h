#pragma once

#include <cstdint>

namespace Strada::Testing
{
	// Whether StradaTests replaces the global allocation functions, so that GetMixingThreadAllocationCount counts: not in a
	// build with a sanitizer (ST_SANITIZER_BUILD), whose runtime defines them itself. Without the hook the count stays 0,
	// and a case that compares it says it skips that check instead.
	bool IsMixingThreadAllocationCounted();

	// Calls of the global allocation functions (every form of operator new and operator delete, which StradaTests replaces
	// in MixAllocationHook.cpp) made on the thread that was mixing a block at the time (AudioEngine::IsMixingOnThisThread),
	// since the test program started. The engine's own code allocates only through them; miniaudio's allocations are
	// counted apart (AudioEngine::GetMixAllocationCount).
	uint64_t GetMixingThreadAllocationCount();
}
