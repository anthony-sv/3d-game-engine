#pragma once

#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"

#include <nvrhi/nvrhi.h>

namespace Strada
{
	// Copies the first mip/slice of a texture to the CPU as RGBA8. Supports 8-bit RGBA/BGRA formats (UNORM and sRGB).
	// Synchronous: waits for the GPU to finish all submitted work. Main thread only.
	[[nodiscard]] Result<Image> ReadbackTexture(nvrhi::ITexture* texture);
}
