#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Math/Math.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Strada
{
	// Ordered JSON keeps file output deterministic and preserves the order of loaded documents.
	using Json = nlohmann::ordered_json;

	enum class UnknownFieldPolicy : uint8_t
	{
		// Reject the document (automation input: catches typos).
		Error = 0,
		// Record a warning and continue (files written by newer versions).
		Warn,
		Ignore
	};

	struct DeserializationContext
	{
		UnknownFieldPolicy UnknownFields = UnknownFieldPolicy::Error;
		// Resolves "asset://<path>" and "builtin://<name>" references. When unset, such references are errors.
		std::function<Result<AssetHandle>(std::string_view reference)> ResolveAssetReference;
		// Receives non-fatal problems; may be null.
		std::vector<std::string>* Warnings = nullptr;

		void Warn(std::string message) const
		{
			if (Warnings != nullptr)
			{
				Warnings->push_back(std::move(message));
			}
		}
	};

	// Name table for enums serialized as strings: specialize with
	//   static constexpr std::array<std::pair<T, std::string_view>, N> Values = {...};
	template<typename T>
	struct EnumTraits;

	template<typename T>
	concept SerializableEnum = std::is_enum_v<T> && requires { EnumTraits<T>::Values; };

	template<SerializableEnum T>
	constexpr std::string_view EnumToString(T value)
	{
		for (auto const& [enumerator, name] : EnumTraits<T>::Values)
		{
			if (enumerator == value)
			{
				return name;
			}
		}
		return {};
	}

	template<SerializableEnum T>
	constexpr std::optional<T> EnumFromString(std::string_view name)
	{
		for (auto const& [enumerator, enumeratorName] : EnumTraits<T>::Values)
		{
			if (enumeratorName == name)
			{
				return enumerator;
			}
		}
		return std::nullopt;
	}

	// Conversion between C++ values and JSON. Every specialization provides:
	//   static Json ToJson(T const&);
	//   static Result<void> FromJson(Json const&, T&, DeserializationContext const&);  (error text describes the expectation)
	//   static std::string TypeName();  (used in schemas)
	template<typename T>
	struct JsonTraits;

	namespace Detail
	{
		inline Result<void> ReadFloat(Json const& json, float& out)
		{
			if (!json.is_number())
			{
				return Error{"expected a number"};
			}
			double const value = json.get<double>();
			if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
			{
				return Error{"expected a finite number"};
			}
			out = static_cast<float>(value);
			return {};
		}

		template<size_t N>
		Result<void> ReadFloatArray(Json const& json, std::array<float, N>& out)
		{
			if (!json.is_array() || json.size() != N)
			{
				return MakeError("expected an array of {} numbers", N);
			}
			for (size_t i = 0; i < N; i++)
			{
				if (Result<void> result = ReadFloat(json[i], out[i]); !result)
				{
					return MakeError("expected an array of {} finite numbers", N);
				}
			}
			return {};
		}
	}

	template<>
	struct JsonTraits<bool>
	{
		static Json ToJson(bool value) { return value; }
		static Result<void> FromJson(Json const& json, bool& out, DeserializationContext const&)
		{
			if (!json.is_boolean())
			{
				return Error{"expected true or false"};
			}
			out = json.get<bool>();
			return {};
		}
		static std::string TypeName() { return "bool"; }
	};

	template<typename T>
		requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
	struct JsonTraits<T>
	{
		static Json ToJson(T value) { return value; }
		static Result<void> FromJson(Json const& json, T& out, DeserializationContext const&)
		{
			if (!json.is_number_integer())
			{
				return Error{std::is_signed_v<T> ? "expected an integer" : "expected a non-negative integer"};
			}
			if constexpr (std::is_signed_v<T>)
			{
				if (json.is_number_unsigned())
				{
					uint64_t const value = json.get<uint64_t>();
					if (value > static_cast<uint64_t>(std::numeric_limits<T>::max()))
					{
						return Error{"integer out of range"};
					}
					out = static_cast<T>(value);
					return {};
				}
				int64_t const value = json.get<int64_t>();
				if (value < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
				    value > static_cast<int64_t>(std::numeric_limits<T>::max()))
				{
					return Error{"integer out of range"};
				}
				out = static_cast<T>(value);
			}
			else
			{
				// Non-negative values may be stored as either signed or unsigned JSON integers.
				uint64_t value = 0;
				if (json.is_number_unsigned())
				{
					value = json.get<uint64_t>();
				}
				else
				{
					int64_t const signedValue = json.get<int64_t>();
					if (signedValue < 0)
					{
						return Error{"expected a non-negative integer"};
					}
					value = static_cast<uint64_t>(signedValue);
				}
				if (value > static_cast<uint64_t>(std::numeric_limits<T>::max()))
				{
					return Error{"integer out of range"};
				}
				out = static_cast<T>(value);
			}
			return {};
		}
		static std::string TypeName() { return std::is_signed_v<T> ? "int" : "uint"; }
	};

	template<>
	struct JsonTraits<float>
	{
		static Json ToJson(float value) { return value; }
		static Result<void> FromJson(Json const& json, float& out, DeserializationContext const&) { return Detail::ReadFloat(json, out); }
		static std::string TypeName() { return "float"; }
	};

	template<>
	struct JsonTraits<double>
	{
		static Json ToJson(double value) { return value; }
		static Result<void> FromJson(Json const& json, double& out, DeserializationContext const&)
		{
			if (!json.is_number() || !std::isfinite(json.get<double>()))
			{
				return Error{"expected a finite number"};
			}
			out = json.get<double>();
			return {};
		}
		static std::string TypeName() { return "double"; }
	};

	template<>
	struct JsonTraits<std::string>
	{
		static Json ToJson(std::string const& value) { return value; }
		static Result<void> FromJson(Json const& json, std::string& out, DeserializationContext const&)
		{
			if (!json.is_string())
			{
				return Error{"expected a string"};
			}
			out = json.get<std::string>();
			return {};
		}
		static std::string TypeName() { return "string"; }
	};

	// UUIDs are decimal strings so every JSON consumer reads them losslessly. "0" (or "") means none.
	template<>
	struct JsonTraits<UUID>
	{
		static Json ToJson(UUID value) { return value.ToString(); }
		static Result<void> FromJson(Json const& json, UUID& out, DeserializationContext const&)
		{
			if (json.is_string())
			{
				std::string const& text = json.get_ref<std::string const&>();
				if (text.empty())
				{
					out = UUID::Invalid();
					return {};
				}
				if (std::optional<UUID> const parsed = UUID::FromString(text))
				{
					out = *parsed;
					return {};
				}
			}
			else if (json.is_number_unsigned())
			{
				out = UUID(json.get<uint64_t>());
				return {};
			}
			else if (json.is_number_integer() && json.get<int64_t>() >= 0)
			{
				out = UUID(static_cast<uint64_t>(json.get<int64_t>()));
				return {};
			}
			return Error{"expected an ID as a decimal string"};
		}
		static std::string TypeName() { return "uuid"; }
	};

	// Asset handles accept a decimal ID string, "asset://<path relative to Assets/>" or "builtin://<Name>".
	template<>
	struct JsonTraits<AssetHandle>
	{
		static Json ToJson(AssetHandle value) { return value.ToString(); }
		static Result<void> FromJson(Json const& json, AssetHandle& out, DeserializationContext const& context)
		{
			if (json.is_string())
			{
				std::string const& text = json.get_ref<std::string const&>();
				if (text.starts_with("asset://") || text.starts_with("builtin://"))
				{
					if (!context.ResolveAssetReference)
					{
						return MakeError("asset reference '{}' cannot be resolved here", text);
					}
					Result<AssetHandle> resolved = context.ResolveAssetReference(text);
					if (!resolved)
					{
						return Error{resolved.GetError()};
					}
					out = resolved.GetValue();
					return {};
				}
			}
			UUID id = UUID::Invalid();
			if (Result<void> result = JsonTraits<UUID>::FromJson(json, id, context); !result)
			{
				return Error{"expected an asset handle (decimal string), \"asset://<path>\" or \"builtin://<name>\""};
			}
			out = AssetHandle(id);
			return {};
		}
		static std::string TypeName() { return "asset"; }
	};

	template<>
	struct JsonTraits<glm::vec2>
	{
		static Json ToJson(glm::vec2 const& value) { return Json::array({value.x, value.y}); }
		static Result<void> FromJson(Json const& json, glm::vec2& out, DeserializationContext const&)
		{
			std::array<float, 2> values{};
			if (Result<void> result = Detail::ReadFloatArray(json, values); !result)
			{
				return result;
			}
			out = {values[0], values[1]};
			return {};
		}
		static std::string TypeName() { return "vec2"; }
	};

	template<>
	struct JsonTraits<glm::vec3>
	{
		static Json ToJson(glm::vec3 const& value) { return Json::array({value.x, value.y, value.z}); }
		static Result<void> FromJson(Json const& json, glm::vec3& out, DeserializationContext const&)
		{
			std::array<float, 3> values{};
			if (Result<void> result = Detail::ReadFloatArray(json, values); !result)
			{
				return result;
			}
			out = {values[0], values[1], values[2]};
			return {};
		}
		static std::string TypeName() { return "vec3"; }
	};

	template<>
	struct JsonTraits<glm::vec4>
	{
		static Json ToJson(glm::vec4 const& value) { return Json::array({value.x, value.y, value.z, value.w}); }
		static Result<void> FromJson(Json const& json, glm::vec4& out, DeserializationContext const&)
		{
			std::array<float, 4> values{};
			if (Result<void> result = Detail::ReadFloatArray(json, values); !result)
			{
				return result;
			}
			out = {values[0], values[1], values[2], values[3]};
			return {};
		}
		static std::string TypeName() { return "vec4"; }
	};

	// Quaternions are [x, y, z, w]; the result is normalized.
	template<>
	struct JsonTraits<glm::quat>
	{
		static Json ToJson(glm::quat const& value) { return Json::array({value.x, value.y, value.z, value.w}); }
		static Result<void> FromJson(Json const& json, glm::quat& out, DeserializationContext const&)
		{
			std::array<float, 4> values{};
			if (Result<void> result = Detail::ReadFloatArray(json, values); !result)
			{
				return Error{"expected a quaternion [x, y, z, w]"};
			}
			glm::quat const quaternion = glm::quat::wxyz(values[3], values[0], values[1], values[2]);
			float const length = Math::QuaternionLength(quaternion);
			if (length < 1e-6f)
			{
				return Error{"expected a non-zero quaternion [x, y, z, w]"};
			}
			// Unit quaternions are kept bit-exact: renormalizing them would drift by an ulp on every save/load or
			// undo/redo round trip.
			out = std::abs(length - 1.0f) <= UnitLengthTolerance ? quaternion : Math::NormalizeRotation(quaternion);
			return {};
		}
		static std::string TypeName() { return "quat"; }

		static constexpr float UnitLengthTolerance = 1e-5f;
	};

	template<>
	struct JsonTraits<glm::bvec3>
	{
		static Json ToJson(glm::bvec3 const& value) { return Json::array({value.x, value.y, value.z}); }
		static Result<void> FromJson(Json const& json, glm::bvec3& out, DeserializationContext const&)
		{
			if (!json.is_array() || json.size() != 3 || !json[0].is_boolean() || !json[1].is_boolean() || !json[2].is_boolean())
			{
				return Error{"expected an array of 3 booleans"};
			}
			out = {json[0].get<bool>(), json[1].get<bool>(), json[2].get<bool>()};
			return {};
		}
		static std::string TypeName() { return "bvec3"; }
	};

	template<SerializableEnum T>
	struct JsonTraits<T>
	{
		static Json ToJson(T value) { return std::string(EnumToString(value)); }
		static Result<void> FromJson(Json const& json, T& out, DeserializationContext const&)
		{
			if (json.is_string())
			{
				if (std::optional<T> const value = EnumFromString<T>(json.get_ref<std::string const&>()))
				{
					out = *value;
					return {};
				}
			}
			return MakeError("expected one of {}", TypeName());
		}
		static std::string TypeName()
		{
			std::string names;
			for (auto const& [enumerator, name] : EnumTraits<T>::Values)
			{
				names += names.empty() ? "" : "|";
				names += name;
			}
			return names;
		}
	};

	template<typename T>
	struct JsonTraits<std::vector<T>>
	{
		static Json ToJson(std::vector<T> const& values)
		{
			Json result = Json::array();
			for (T const& value : values)
			{
				result.push_back(JsonTraits<T>::ToJson(value));
			}
			return result;
		}
		static Result<void> FromJson(Json const& json, std::vector<T>& out, DeserializationContext const& context)
		{
			if (!json.is_array())
			{
				return MakeError("expected an array of {}", JsonTraits<T>::TypeName());
			}
			std::vector<T> values;
			values.reserve(json.size());
			for (size_t i = 0; i < json.size(); i++)
			{
				T value{};
				if (Result<void> result = JsonTraits<T>::FromJson(json[i], value, context); !result)
				{
					return MakeError("element {}: {}", i, result.GetError());
				}
				values.push_back(std::move(value));
			}
			out = std::move(values);
			return {};
		}
		static std::string TypeName() { return JsonTraits<T>::TypeName() + "[]"; }
	};

	// Versioned file header: "Strada": { "Version": <int>, "Type": "<type>" }.
	Json MakeFileHeader(std::string_view type, int version);
	// Validates the header of a loaded document; returns the file's version.
	[[nodiscard]] Result<int> ReadFileHeader(Json const& document, std::string_view expectedType, int maximumVersion);

	// Parses JSON text (comments are not allowed). Errors include the position of the problem.
	[[nodiscard]] Result<Json> ParseJson(std::string_view text);
	// Formats with tab indentation and a trailing newline (the format of every Strada file).
	std::string DumpJson(Json const& json);
}
