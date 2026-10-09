#pragma once

// Internal to the Audio module: miniaudio types stay out of every other header.

#include <miniaudio.h>

namespace Strada::Miniaudio
{
	// The engine of AudioEngine (valid while it is initialized).
	ma_engine& GetEngine();
}
