#include "Strada/Math/Math.h"
#include "Strada/Scene/ComponentTraits.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <limits>

using namespace Strada;

namespace
{
	template<typename T>
	Result<T> Read(Json const& json, DeserializationContext const& context = {})
	{
		T value{};
		if (Result<void> result = JsonTraits<T>::FromJson(json, value, context); !result)
		{
			return Error{result.GetError()};
		}
		return value;
	}

	template<typename T>
	T RoundTrip(T const& value)
	{
		Result<T> result = Read<T>(JsonTraits<T>::ToJson(value));
		REQUIRE(result.IsOk());
		return result.GetValue();
	}
}

TEST_CASE("Serialization: scalar traits accept valid values and reject wrong types")
{
	CHECK(Read<bool>(Json(true)).GetValue());
	CHECK(Read<bool>(Json(1)).IsError());

	CHECK(Read<int32_t>(Json(-12)).GetValue() == -12);
	CHECK(Read<int32_t>(Json(1.5)).IsError());
	CHECK(Read<int32_t>(Json(int64_t(1) << 40)).IsError());
	CHECK(Read<int32_t>(Json("12")).IsError());

	CHECK(Read<uint32_t>(Json(7)).GetValue() == 7u);
	CHECK(Read<uint32_t>(Json(-1)).IsError());

	CHECK(Read<float>(Json(2)).GetValue() == doctest::Approx(2.0f));
	CHECK(Read<float>(Json(0.25)).GetValue() == doctest::Approx(0.25f));
	CHECK(Read<float>(Json("1.0")).IsError());
	CHECK(Read<float>(Json(1e300)).IsError());

	CHECK(Read<double>(Json(1e300)).GetValue() == doctest::Approx(1e300));
	CHECK(Read<std::string>(Json("text")).GetValue() == "text");
	CHECK(Read<std::string>(Json(3)).IsError());
}

TEST_CASE("Serialization: UUIDs are written as decimal strings and read losslessly")
{
	UUID const maximum(std::numeric_limits<uint64_t>::max());
	Json const json = JsonTraits<UUID>::ToJson(maximum);
	REQUIRE(json.is_string());
	CHECK(json.get<std::string>() == "18446744073709551615");
	CHECK(Read<UUID>(json).GetValue() == maximum);

	// Plain unsigned numbers and empty strings are accepted; malformed text is not.
	CHECK(Read<UUID>(Json(uint64_t(42))).GetValue() == UUID(42));
	CHECK_FALSE(Read<UUID>(Json("")).GetValue().IsValid());
	CHECK(Read<UUID>(Json("12ab")).IsError());
	CHECK(Read<UUID>(Json(-3)).IsError());
}

TEST_CASE("Serialization: asset handles resolve references through the context")
{
	AssetHandle const handle(UUID(99));
	CHECK(RoundTrip(handle) == handle);
	CHECK_FALSE(Read<AssetHandle>(Json("0")).GetValue().IsValid());

	// Without a resolver, symbolic references are errors.
	Result<AssetHandle> const unresolved = Read<AssetHandle>(Json("asset://Meshes/Cube.glb"));
	REQUIRE(unresolved.IsError());
	CHECK(unresolved.GetError().find("asset://Meshes/Cube.glb") != std::string::npos);

	DeserializationContext context;
	context.ResolveAssetReference = [](std::string_view reference) -> Result<AssetHandle>
	{
		if (reference == "builtin://Cube")
		{
			return AssetHandle(UUID(1));
		}
		return MakeError("unknown asset '{}'", reference);
	};
	CHECK(Read<AssetHandle>(Json("builtin://Cube"), context).GetValue() == AssetHandle(UUID(1)));
	Result<AssetHandle> const missing = Read<AssetHandle>(Json("builtin://Teapot"), context);
	REQUIRE(missing.IsError());
	CHECK(missing.GetError() == "unknown asset 'builtin://Teapot'");
}

TEST_CASE("Serialization: vectors, quaternions and boolean vectors")
{
	CHECK(RoundTrip(glm::vec2(1.0f, -2.0f)) == glm::vec2(1.0f, -2.0f));
	CHECK(RoundTrip(glm::vec3(1.0f, 2.0f, 3.0f)) == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(RoundTrip(glm::vec4(1.0f, 2.0f, 3.0f, 4.0f)) == glm::vec4(1.0f, 2.0f, 3.0f, 4.0f));
	CHECK(RoundTrip(glm::bvec3(true, false, true)) == glm::bvec3(true, false, true));

	Result<glm::vec3> const wrongSize = Read<glm::vec3>(Json::array({1, 2}));
	REQUIRE(wrongSize.IsError());
	CHECK(wrongSize.GetError() == "expected an array of 3 numbers");
	CHECK(Read<glm::vec3>(Json::array({1, "2", 3})).IsError());

	// Quaternions are [x, y, z, w] and normalized on read.
	Result<glm::quat> const quaternion = Read<glm::quat>(Json::array({0, 0, 0, 2}));
	REQUIRE(quaternion.IsOk());
	CHECK(quaternion.GetValue().w == doctest::Approx(1.0f));
	Json const written = JsonTraits<glm::quat>::ToJson(glm::quat::wxyz(0.5f, 0.5f, 0.5f, 0.5f));
	CHECK(written == Json::array({0.5f, 0.5f, 0.5f, 0.5f}));
	CHECK(Read<glm::quat>(Json::array({0, 0, 0, 0})).IsError());
	// Components near the float range normalize too: their squares would overflow.
	Result<glm::quat> const huge = Read<glm::quat>(Json::array({1.0e30, 0.0, 0.0, 1.0e30}));
	REQUIRE(huge.IsOk());
	CHECK(huge.GetValue().x == doctest::Approx(0.70710678f));
	CHECK(huge.GetValue().w == doctest::Approx(0.70710678f));
	Result<glm::quat> const largest = Read<glm::quat>(Json::array({3.0e38, 3.0e38, 3.0e38, 3.0e38}));
	REQUIRE(largest.IsOk());
	CHECK(largest.GetValue().w == doctest::Approx(0.5f));
}

TEST_CASE("Serialization: unit quaternions round trip bit-exactly")
{
	// Undo snapshots and repeated save/load cycles must not drift, so already-normalized rotations are not renormalized.
	for (glm::vec3 const degrees : {glm::vec3(30.0f, 45.0f, 60.0f), glm::vec3(-170.0f, 12.5f, 89.0f), glm::vec3(0.1f, 0.2f, 0.3f)})
	{
		glm::quat const rotation = Math::EulerDegreesToQuaternion(degrees);
		glm::quat value = rotation;
		for (int i = 0; i < 5; i++)
		{
			value = RoundTrip(value);
		}
		CHECK(value.x == rotation.x);
		CHECK(value.y == rotation.y);
		CHECK(value.z == rotation.z);
		CHECK(value.w == rotation.w);
	}
}

TEST_CASE("Serialization: enums use their names")
{
	CHECK(JsonTraits<RigidBodyType>::ToJson(RigidBodyType::Kinematic) == Json("Kinematic"));
	CHECK(Read<RigidBodyType>(Json("Dynamic")).GetValue() == RigidBodyType::Dynamic);
	Result<RigidBodyType> const invalid = Read<RigidBodyType>(Json("Floating"));
	REQUIRE(invalid.IsError());
	CHECK(invalid.GetError() == "expected one of Static|Dynamic|Kinematic");
	CHECK(Read<RigidBodyType>(Json(1)).IsError());
}

TEST_CASE("Serialization: arrays report the failing element")
{
	std::vector<UUID> const ids = {UUID(1), UUID(2)};
	CHECK(RoundTrip(ids) == ids);

	Result<std::vector<UUID>> const invalid = Read<std::vector<UUID>>(Json::array({"1", "x"}));
	REQUIRE(invalid.IsError());
	CHECK(invalid.GetError().find("element 1") != std::string::npos);
	CHECK(Read<std::vector<UUID>>(Json("1")).IsError());
}

TEST_CASE("Serialization: file headers are validated")
{
	Json document = Json::object();
	document["Strada"] = MakeFileHeader("Scene", 1);
	CHECK(ReadFileHeader(document, "Scene", 1).GetValue() == 1);

	CHECK(ReadFileHeader(document, "Prefab", 1).GetError() == "expected a Prefab file but found 'Scene'");
	document["Strada"]["Version"] = 3;
	CHECK(ReadFileHeader(document, "Scene", 2).GetError() == "file version 3 is newer than the supported version 2");
	document["Strada"]["Version"] = 0;
	CHECK(ReadFileHeader(document, "Scene", 2).IsError());
	CHECK(ReadFileHeader(Json::object(), "Scene", 1).GetError() == "missing \"Strada\" file header");
	CHECK(ReadFileHeader(Json::array(), "Scene", 1).IsError());
}

TEST_CASE("Serialization: parsing reports errors and dumping uses tabs")
{
	Result<Json> const parsed = ParseJson(R"({"a": [1, 2], "b": "text"})");
	REQUIRE(parsed.IsOk());
	CHECK(parsed.GetValue()["a"][1] == 2);

	Result<Json> const invalid = ParseJson("{\"a\": ");
	REQUIRE(invalid.IsError());
	CHECK(invalid.GetError().find("invalid JSON") != std::string::npos);

	Json ordered = Json::object();
	ordered["z"] = 1;
	ordered["a"] = 2;
	CHECK(DumpJson(ordered) == "{\n\t\"z\": 1,\n\t\"a\": 2\n}\n");
}
