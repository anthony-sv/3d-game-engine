#pragma once

#include "Strada/Core/Base.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Strada
{
	// Random 64-bit identifier. Zero is reserved as the invalid ID.
	class UUID
	{
	public:
		// Generates a new random, non-zero ID (thread-safe).
		UUID();
		constexpr UUID(uint64_t value)
			: m_Value(value)
		{
		}

		static constexpr UUID Invalid() { return UUID(0); }

		constexpr bool IsValid() const { return m_Value != 0; }
		constexpr uint64_t GetValue() const { return m_Value; }
		constexpr operator uint64_t() const { return m_Value; }

		// Decimal string form used in all JSON files and the automation API.
		std::string ToString() const;
		static std::optional<UUID> FromString(std::string_view text);

	private:
		uint64_t m_Value;
	};

	// Lets fmt/spdlog format UUIDs as their decimal value.
	inline uint64_t format_as(UUID id)
	{
		return id.GetValue();
	}
}

template<>
struct std::hash<Strada::UUID>
{
	size_t operator()(Strada::UUID const& id) const noexcept { return std::hash<uint64_t>()(id.GetValue()); }
};
