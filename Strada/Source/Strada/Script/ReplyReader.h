#pragma once

// Internal to the Script module: reads the JSON the scripting runtime answers with (the debugger's replies, the live events),
// naming the JSON path of the first member that is not of its form. Pure; any thread.

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Strada::ScriptEngineInternal
{
	// Reads the members of a reply, keeping the first error, which names the member's JSON path: reads after an error
	// answer defaults, so a reader reads on and checks once at the end.
	class ReplyReader
	{
	public:
		// root names the whole text in errors about it (an empty path).
		explicit ReplyReader(std::string_view root = "the reply")
			: m_Root(root)
		{
		}

		bool IsOk() const { return m_Error.empty(); }
		std::string const& GetError() const { return m_Error; }

		void Fail(std::string_view path, std::string_view problem)
		{
			if (m_Error.empty())
			{
				m_Error = fmt::format("{}: {}", path.empty() ? std::string_view(m_Root) : path, problem);
			}
		}

		static std::string Member(std::string_view path, std::string_view name)
		{
			return path.empty() ? std::string(name) : fmt::format("{}.{}", path, name);
		}

		static std::string Element(std::string_view path, size_t index) { return fmt::format("{}[{}]", path, index); }

		// Whether the value is an object, failing at the path when it is not.
		bool ExpectObject(Json const& value, std::string_view path)
		{
			if (!value.is_object())
			{
				Fail(path, "not an object");
				return false;
			}
			return true;
		}

		// The member that must be there; null (failing) when it is missing or the value is no object.
		Json const* Require(Json const& object, std::string_view name, std::string_view path)
		{
			if (!object.is_object())
			{
				Fail(path, "not an object");
				return nullptr;
			}
			auto const member = object.find(name);
			if (member == object.end())
			{
				Fail(Member(path, name), "missing");
				return nullptr;
			}
			return &*member;
		}

		// The member when it is there and not null.
		static Json const* Find(Json const& object, std::string_view name)
		{
			if (!object.is_object())
			{
				return nullptr;
			}
			auto const member = object.find(name);
			return member != object.end() && !member->is_null() ? &*member : nullptr;
		}

		bool Bool(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			if (value == nullptr)
			{
				return false;
			}
			if (!value->is_boolean())
			{
				Fail(Member(path, name), "not a boolean");
				return false;
			}
			return value->get<bool>();
		}

		// An optional boolean: false when absent.
		bool OptionalBool(Json const& object, std::string_view name, std::string_view path)
		{
			return object.is_object() && object.contains(name) ? Bool(object, name, path) : false;
		}

		uint64_t UInt(Json const& value, std::string_view path, uint64_t maximum = std::numeric_limits<uint64_t>::max())
		{
			if (value.is_number_unsigned())
			{
				uint64_t const number = value.get<uint64_t>();
				if (number <= maximum)
				{
					return number;
				}
			}
			else if (value.is_number_integer() && value.get<int64_t>() >= 0 && static_cast<uint64_t>(value.get<int64_t>()) <= maximum)
			{
				return static_cast<uint64_t>(value.get<int64_t>());
			}
			Fail(path, maximum == std::numeric_limits<uint32_t>::max() ? "not an unsigned 32-bit integer" : "not an unsigned integer");
			return 0;
		}

		uint64_t UInt(Json const& object, std::string_view name, std::string_view path,
		              uint64_t maximum = std::numeric_limits<uint64_t>::max())
		{
			Json const* const value = Require(object, name, path);
			return value != nullptr ? UInt(*value, Member(path, name), maximum) : 0;
		}

		uint32_t UInt32(Json const& object, std::string_view name, std::string_view path)
		{
			return static_cast<uint32_t>(UInt(object, name, path, std::numeric_limits<uint32_t>::max()));
		}

		// An unsigned 32-bit member that may be absent (0 then).
		uint32_t OptionalUInt32(Json const& object, std::string_view name, std::string_view path)
		{
			return object.is_object() && object.contains(name) ? UInt32(object, name, path) : 0;
		}

		// An unsigned integer or null.
		std::optional<uint64_t> NullableUInt(Json const& object, std::string_view name, std::string_view path,
		                                     uint64_t maximum = std::numeric_limits<uint64_t>::max())
		{
			Json const* const value = Require(object, name, path);
			if (value == nullptr || value->is_null())
			{
				return std::nullopt;
			}
			return UInt(*value, Member(path, name), maximum);
		}

		int64_t Int(Json const& value, std::string_view path)
		{
			if (value.is_number_integer() && !value.is_number_unsigned())
			{
				return value.get<int64_t>();
			}
			if (value.is_number_unsigned() && value.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			{
				return static_cast<int64_t>(value.get<uint64_t>());
			}
			Fail(path, "not a 64-bit integer");
			return 0;
		}

		int64_t Int(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			return value != nullptr ? Int(*value, Member(path, name)) : 0;
		}

		double Number(Json const& value, std::string_view path)
		{
			if (!value.is_number())
			{
				Fail(path, "not a number");
				return 0.0;
			}
			double const number = value.get<double>();
			if (!std::isfinite(number))
			{
				Fail(path, "not a finite number");
				return 0.0;
			}
			return number;
		}

		double Number(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			return value != nullptr ? Number(*value, Member(path, name)) : 0.0;
		}

		// A finite number or null.
		std::optional<double> NullableNumber(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			if (value == nullptr || value->is_null())
			{
				return std::nullopt;
			}
			return Number(*value, Member(path, name));
		}

		std::string String(Json const& value, std::string_view path)
		{
			if (!value.is_string())
			{
				Fail(path, "not a string");
				return {};
			}
			return value.get<std::string>();
		}

		std::string String(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			return value != nullptr ? String(*value, Member(path, name)) : std::string();
		}

		// A string or null.
		std::optional<std::string> NullableString(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			if (value == nullptr || value->is_null())
			{
				return std::nullopt;
			}
			return String(*value, Member(path, name));
		}

		// A handle or an entity: a decimal string of an unsigned 64-bit integer.
		uint64_t Id(Json const& value, std::string_view path)
		{
			if (value.is_string())
			{
				if (std::optional<UUID> const id = UUID::FromString(value.get_ref<std::string const&>()))
				{
					return id->GetValue();
				}
			}
			Fail(path, "not a decimal string of an unsigned 64-bit integer");
			return 0;
		}

		uint64_t Id(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			return value != nullptr ? Id(*value, Member(path, name)) : 0;
		}

		AssetHandle Handle(Json const& object, std::string_view name, std::string_view path)
		{
			return AssetHandle(UUID(Id(object, name, path)));
		}

		UUID Entity(Json const& object, std::string_view name, std::string_view path) { return UUID(Id(object, name, path)); }

		// The array member; null (failing) when it is missing or no array.
		Json const* Array(Json const& object, std::string_view name, std::string_view path)
		{
			Json const* const value = Require(object, name, path);
			if (value != nullptr && !value->is_array())
			{
				Fail(Member(path, name), "not an array");
				return nullptr;
			}
			return value;
		}

		// An enumerator by its name among the names, in the order of the enum's values.
		template<typename TEnum, size_t Count>
		TEnum Enum(Json const& object, std::string_view name, std::string_view path, std::array<std::string_view, Count> const& names)
		{
			std::string const text = String(object, name, path);
			for (size_t index = 0; index < Count; index++)
			{
				if (names[index] == text)
				{
					return static_cast<TEnum>(index);
				}
			}
			if (IsOk())
			{
				Fail(Member(path, name), fmt::format("unknown value \"{}\"", text));
			}
			return static_cast<TEnum>(0);
		}

	private:
		std::string m_Root;
		std::string m_Error;
	};
}
