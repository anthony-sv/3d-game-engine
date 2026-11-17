#pragma once

#include <cstdint>

namespace Strada::Testing
{
	// Calls of the global allocation functions (every form of operator new and operator delete, which StradaTests replaces
	// in MixAllocationHook.cpp) made on the thread that was mixing a block at the time (AudioEngine::IsMixingOnThisThread),
	// since the test program started. The engine's own code allocates only through them; miniaudio's allocations are
	// counted apart (AudioEngine::GetMixAllocationCount).
	uint64_t GetMixingThreadAllocationCount();
}
