#pragma once

#include <bit>
#include <cstdint>
#include <span>
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
				hash = FoldByte(hash, static_cast<uint8_t>(character));
			}
			return hash;
		}

		// Combines a value into a running hash (boost::hash_combine scheme for 64-bit).
		static constexpr uint64_t Combine(uint64_t seed, uint64_t value)
		{
			return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
		}

	private:
		friend class HashBuilder;

		static constexpr uint64_t OffsetBasis = 14695981039346656037ull;
		static constexpr uint64_t Prime = 1099511628211ull;

		static constexpr uint64_t FoldByte(uint64_t hash, uint8_t byte) { return (hash ^ static_cast<uint64_t>(byte)) * Prime; }
	};

	// Builds a 64-bit FNV-1a hash of a sequence of values, each folded in byte by byte in little-endian order (floats by
	// their bit patterns), so equal sequences give equal hashes on every platform and run. Used for state hashes that
	// tell whether two simulations are the same.
	class HashBuilder
	{
	public:
		constexpr void AddUInt64(uint64_t value)
		{
			for (int shift = 0; shift < 64; shift += 8)
			{
				m_Hash = Hash::FoldByte(m_Hash, static_cast<uint8_t>(value >> shift));
			}
		}

		constexpr void AddUInt32(uint32_t value)
		{
			for (int shift = 0; shift < 32; shift += 8)
			{
				m_Hash = Hash::FoldByte(m_Hash, static_cast<uint8_t>(value >> shift));
			}
		}

		// By its two's complement bit pattern.
		constexpr void AddInt32(int32_t value) { AddUInt32(static_cast<uint32_t>(value)); }
		constexpr void AddFloat(float value) { AddUInt32(std::bit_cast<uint32_t>(value)); }
		constexpr void AddBool(bool value) { m_Hash = Hash::FoldByte(m_Hash, value ? 1 : 0); }

		// Each byte in order, as Hash::FNV1a folds a text's characters.
		constexpr void AddBytes(std::span<uint8_t const> bytes)
		{
			for (uint8_t const byte : bytes)
			{
				m_Hash = Hash::FoldByte(m_Hash, byte);
			}
		}

		constexpr uint64_t GetHash() const { return m_Hash; }

	private:
		uint64_t m_Hash = Hash::OffsetBasis;
	};
}
