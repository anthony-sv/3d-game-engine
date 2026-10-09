#include "stpch.h"
#include "Strada/Core/Base64.h"

#include <array>

namespace Strada
{
	namespace
	{
		constexpr std::string_view s_Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		constexpr uint8_t InvalidCharacter = 0xFF;

		constexpr std::array<uint8_t, 256> MakeDecodeTable()
		{
			std::array<uint8_t, 256> table{};
			table.fill(InvalidCharacter);
			for (size_t i = 0; i < s_Alphabet.size(); i++)
			{
				table[static_cast<uint8_t>(s_Alphabet[i])] = static_cast<uint8_t>(i);
			}
			return table;
		}

		constexpr std::array<uint8_t, 256> s_DecodeTable = MakeDecodeTable();
	}

	std::string Base64Encode(std::span<uint8_t const> data)
	{
		std::string result;
		result.reserve((data.size() + 2) / 3 * 4);

		size_t i = 0;
		for (; i + 3 <= data.size(); i += 3)
		{
			uint32_t const group = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
			result.push_back(s_Alphabet[(group >> 18) & 0x3F]);
			result.push_back(s_Alphabet[(group >> 12) & 0x3F]);
			result.push_back(s_Alphabet[(group >> 6) & 0x3F]);
			result.push_back(s_Alphabet[group & 0x3F]);
		}

		size_t const remaining = data.size() - i;
		if (remaining == 1)
		{
			uint32_t const group = uint32_t(data[i]) << 16;
			result.push_back(s_Alphabet[(group >> 18) & 0x3F]);
			result.push_back(s_Alphabet[(group >> 12) & 0x3F]);
			result += "==";
		}
		else if (remaining == 2)
		{
			uint32_t const group = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
			result.push_back(s_Alphabet[(group >> 18) & 0x3F]);
			result.push_back(s_Alphabet[(group >> 12) & 0x3F]);
			result.push_back(s_Alphabet[(group >> 6) & 0x3F]);
			result.push_back('=');
		}
		return result;
	}

	Result<std::vector<uint8_t>> Base64Decode(std::string_view text)
	{
		// Strip padding; everything before it must be alphabet characters.
		size_t length = text.size();
		size_t padding = 0;
		while (length > 0 && text[length - 1] == '=' && padding < 2)
		{
			length--;
			padding++;
		}
		if (padding > 0 && text.size() % 4 != 0)
		{
			return Error{"invalid Base64 padding"};
		}
		if (length % 4 == 1)
		{
			return Error{"truncated Base64 data"};
		}

		std::vector<uint8_t> result;
		result.reserve(length / 4 * 3 + 2);

		uint32_t group = 0;
		int bits = 0;
		for (size_t i = 0; i < length; i++)
		{
			uint8_t const value = s_DecodeTable[static_cast<uint8_t>(text[i])];
			if (value == InvalidCharacter)
			{
				return MakeError("invalid Base64 character at offset {}", i);
			}
			group = (group << 6) | value;
			bits += 6;
			if (bits >= 8)
			{
				bits -= 8;
				result.push_back(static_cast<uint8_t>((group >> bits) & 0xFF));
			}
		}

		// Leftover bits of the final group are ignored (RFC 4648 allows decoders to accept non-zero pad bits).
		return result;
	}
}
