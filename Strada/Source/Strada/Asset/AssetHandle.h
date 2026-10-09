#pragma once

#include "Strada/Core/UUID.h"

#include <functional>
#include <string>

namespace Strada
{
	// Identifies an asset (mesh, material, texture, ...). Default-constructed handles are invalid ("no asset"); unlike
	// UUID, construction never generates a new random value implicitly.
	class AssetHandle
	{
	public:
		constexpr AssetHandle() = default;
		constexpr explicit AssetHandle(UUID id)
			: m_ID(id)
		{
		}

		// Values below this are reserved for built-in assets and never generated.
		static constexpr uint64_t ReservedCount = 1024;

		// A new random handle outside the reserved range.
		[[nodiscard]] static AssetHandle Generate()
		{
			UUID id;
			while (id.GetValue() < ReservedCount)
			{
				id = UUID();
			}
			return AssetHandle(id);
		}

		constexpr bool IsValid() const { return m_ID.IsValid(); }
		constexpr bool IsReserved() const { return m_ID.IsValid() && m_ID.GetValue() < ReservedCount; }
		constexpr UUID GetUUID() const { return m_ID; }
		std::string ToString() const { return m_ID.ToString(); }

		constexpr bool operator==(AssetHandle const& other) const { return m_ID.GetValue() == other.m_ID.GetValue(); }
		constexpr bool operator!=(AssetHandle const& other) const { return !(*this == other); }

	private:
		UUID m_ID = UUID::Invalid();
	};

	inline uint64_t format_as(AssetHandle handle)
	{
		return handle.GetUUID().GetValue();
	}
}

template<>
struct std::hash<Strada::AssetHandle>
{
	size_t operator()(Strada::AssetHandle const& handle) const noexcept { return std::hash<uint64_t>()(handle.GetUUID().GetValue()); }
};
