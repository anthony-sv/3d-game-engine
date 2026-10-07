#pragma once

#include <cstdint>
#include <string_view>

namespace Strada
{
	class Hash
	{
	public:
		// 64-bit FNV-1a. Stable across platforms and runs, usable at compile time.
		static constexpr uint64_t FNV1a(std::string_view text)
		{
			uint64_t hash = OffsetBasis;
			for (char const character : text)
			{
				hash ^= static_cast<uint64_t>(static_cast<uint8_t>(character));
				hash *= Prime;
			}
			return hash;
		}

		// Combines a value into a running hash (boost::hash_combine scheme for 64-bit).
		static constexpr uint64_t Combine(uint64_t seed, uint64_t value)
		{
			return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
		}

	private:
		static constexpr uint64_t OffsetBasis = 14695981039346656037ull;
		static constexpr uint64_t Prime = 1099511628211ull;
	};
}
