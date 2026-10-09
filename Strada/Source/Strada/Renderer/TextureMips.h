#pragma once

#include "Strada/Core/Image.h"

#include <cstdint>
#include <vector>

namespace Strada
{
	// Number of mip levels of a full chain down to 1x1.
	uint32_t GetMipLevelCount(uint32_t width, uint32_t height);

	// Builds the full mip chain of an RGBA8 image (level 0 is the image itself) with a 2x2 box filter. Color channels of
	// sRGB images are averaged in linear space; alpha (and every channel of linear images) is averaged directly. Odd
	// dimensions clamp the filter footprint at the edge.
	std::vector<Image> GenerateMipChain(Image const& image, bool srgb);
}
