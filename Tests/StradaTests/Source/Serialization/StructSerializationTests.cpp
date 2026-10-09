#include "Strada/Serialization/StructSerialization.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	struct BloomSettings
	{
		bool Enabled = true;
		float Intensity = 0.5f;
	};

	struct RenderSettings
	{
		float Exposure = 1.0f;
		BloomSettings Bloom;
		std::vector<float> Cascades = {0.1f, 0.3f};
	};
}

template<>
struct Strada::StructTraits<BloomSettings>
{
	static constexpr std::string_view Name = "Bloom";
	static constexpr auto Fields =
		std::make_tuple(Field("Enabled", &BloomSettings::Enabled), Field("Intensity", &BloomSettings::Intensity));
};

template<>
struct Strada::StructTraits<RenderSettings>
{
	static constexpr std::string_view Name = "RenderSettings";
	static constexpr auto Fields = std::make_tuple(Field("Exposure", &RenderSettings::Exposure), Field("Bloom", &RenderSettings::Bloom),
	                                               Field("Cascades", &RenderSettings::Cascades));
};

TEST_CASE("StructSerialization: reflected structs nest and round trip")
{
	static_assert(ReflectedStruct<BloomSettings>);
	static_assert(!ReflectedStruct<int>);

	RenderSettings settings;
	settings.Exposure = 2.0f;
	settings.Bloom.Intensity = 0.75f;
	settings.Cascades = {0.2f};

	Json const json = JsonTraits<RenderSettings>::ToJson(settings);
	CHECK(json["Bloom"]["Intensity"] == 0.75f);
	CHECK(json["Cascades"] == Json::array({0.2f}));

	RenderSettings loaded;
	REQUIRE(JsonTraits<RenderSettings>::FromJson(json, loaded, DeserializationContext{}).IsOk());
	CHECK(loaded.Exposure == doctest::Approx(2.0f));
	CHECK(loaded.Bloom.Intensity == doctest::Approx(0.75f));
	CHECK(loaded.Cascades == std::vector<float>{0.2f});
}

TEST_CASE("StructSerialization: nested partial updates keep untouched fields")
{
	RenderSettings settings;
	settings.Exposure = 3.0f;
	Json patch = Json::object();
	patch["Bloom"] = Json::object({{"Enabled", false}});
	REQUIRE(DeserializeFields<StructTraits<RenderSettings>>(patch, settings, DeserializationContext{}, "settings").IsOk());
	CHECK(settings.Exposure == doctest::Approx(3.0f));
	CHECK_FALSE(settings.Bloom.Enabled);
	CHECK(settings.Bloom.Intensity == doctest::Approx(0.5f));
}

TEST_CASE("StructSerialization: errors name the struct, the field and the problem")
{
	RenderSettings settings;
	Json patch = Json::object();
	patch["Bloom"] = Json::object({{"Intensity", "bright"}});
	Result<void> const nested = DeserializeFields<StructTraits<RenderSettings>>(patch, settings, DeserializationContext{}, "settings");
	REQUIRE(nested.IsError());
	CHECK(nested.GetError() == "RenderSettings.Bloom: Bloom.Intensity: expected a number");
	CHECK(settings.Bloom.Intensity == doctest::Approx(0.5f));

	Json unknown = Json::object({{"Gamma", 2.2}});
	Result<void> const strict = DeserializeFields<StructTraits<RenderSettings>>(unknown, settings, DeserializationContext{}, "settings");
	REQUIRE(strict.IsError());
	CHECK(strict.GetError() == "settings 'RenderSettings' has no field 'Gamma' (fields: Exposure, Bloom, Cascades)");

	CHECK(DeserializeFields<StructTraits<RenderSettings>>(Json(3), settings, DeserializationContext{}, "settings").GetError() ==
	      "settings 'RenderSettings' must be a JSON object");
}

TEST_CASE("StructSerialization: schemas include nested defaults")
{
	Json const schema = DescribeFields<StructTraits<RenderSettings>, RenderSettings>();
	CHECK(schema["Name"] == "RenderSettings");
	REQUIRE(schema["Fields"].size() == 3);
	CHECK(schema["Fields"][1]["Name"] == "Bloom");
	CHECK(schema["Fields"][1]["Type"] == "Bloom");
	CHECK(schema["Fields"][1]["Default"]["Enabled"] == true);
	CHECK(schema["Fields"][2]["Type"] == "float[]");
}
