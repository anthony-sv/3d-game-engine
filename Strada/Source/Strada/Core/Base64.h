#pragma once

#include "Strada/Core/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// Standard Base64 (RFC 4648, '+' and '/', '=' padding).
	std::string Base64Encode(std::span<uint8_t const> data);

	// Accepts padded or unpadded input; rejects characters outside the alphabet, misplaced padding and truncated groups.
	// Whitespace is not allowed.
	[[nodiscard]] Result<std::vector<uint8_t>> Base64Decode(std::string_view text);
}
