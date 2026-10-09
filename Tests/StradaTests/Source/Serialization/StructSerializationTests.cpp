#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Renderer/SceneRendererSettings.h"
#include "Strada/Serialization/StructSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
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

namespace
{
	struct Tuning
	{
		float Gain = 1.0f;
		glm::vec3 Tint = {1.0f, 1.0f, 1.0f};
		std::vector<float> Weights = {0.5f};
		uint32_t Count = 2;
		std::string Notes;
		AssetHandle Texture;
	};
}

template<>
struct Strada::StructTraits<Tuning>
{
	static constexpr std::string_view Name = "Tuning";
	static constexpr auto Fields = std::make_tuple(
		Field("Gain", &Tuning::Gain).AtLeast(0.0).Doc("Linear gain."), Field("Tint", &Tuning::Tint).Range(0.0, 1.0).AsColor(),
		Field("Weights", &Tuning::Weights).Range(0.0, 1.0), Field("Count", &Tuning::Count).Range(1.0, 8.0),
		Field("Notes", &Tuning::Notes).AsMultilineText(), Field("Texture", &Tuning::Texture).References("Texture"));
};

TEST_CASE("StructSerialization: field hints are checked against their types at compile time")
{
	struct Probe
	{
		float Number = 0.0f;
		int32_t Count = 0;
		bool Flag = false;
		std::string Text;
		glm::vec2 Pair{};
		glm::vec4 Tint{};
		std::vector<glm::vec2> Points;
		std::vector<AssetHandle> Meshes;
	};

	static_assert(Detail::AreHintsValid<float>(Field("Number", &Probe::Number).AtLeast(0.0).Hints));
	static_assert(!Detail::AreHintsValid<std::string>(Field("Text", &Probe::Text).AtLeast(0.0).Hints));
	static_assert(!Detail::AreHintsValid<bool>(Field("Flag", &Probe::Flag).Range(0.0, 1.0).Hints));
	static_assert(!Detail::AreHintsValid<float>(Field("Number", &Probe::Number).Range(2.0, 1.0).Hints));
	static_assert(Detail::AreHintsValid<std::vector<glm::vec2>>(Field("Points", &Probe::Points).AtLeast(0.0).Hints));
	static_assert(Detail::AreHintsValid<glm::vec4>(Field("Tint", &Probe::Tint).AsColor().Hints));
	static_assert(!Detail::AreHintsValid<glm::vec2>(Field("Pair", &Probe::Pair).AsColor().Hints));
	static_assert(!Detail::AreHintsValid<int32_t>(Field("Count", &Probe::Count).AsAngle().Hints));
	static_assert(!Detail::AreHintsValid<float>(Field("Number", &Probe::Number).AsMultilineText().Hints));
	static_assert(!Detail::AreHintsValid<float>(Field("Number", &Probe::Number).References("Mesh").Hints));
	static_assert(Detail::AreHintsValid<std::vector<AssetHandle>>(Field("Meshes", &Probe::Meshes).References("Mesh").Hints));
	static_assert(Detail::IsFieldTableValid<StructTraits<Tuning>, Tuning>());
	static_assert(Detail::IsFieldTableValid<StructTraits<RenderSettings>, RenderSettings>());
}

TEST_CASE("StructSerialization: values outside a field's range are rejected with readable messages")
{
	Tuning tuning;
	auto const apply = [&tuning](Json const& patch)
	{
		Result<void> const result = DeserializeFields<StructTraits<Tuning>>(patch, tuning, DeserializationContext{}, "settings");
		return result ? std::string() : result.GetError();
	};

	CHECK(apply(Json::object({{"Gain", -0.5}})) == "Tuning.Gain: must be at least 0");
	CHECK(apply(Json::object({{"Tint", {0, 2, 0}}})) == "Tuning.Tint: every component must be between 0 and 1");
	CHECK(apply(Json::object({{"Weights", {0.5, 1.5}}})) == "Tuning.Weights: every element must be between 0 and 1");
	CHECK(apply(Json::object({{"Count", 0}})) == "Tuning.Count: must be between 1 and 8");
	// A failure anywhere leaves every field untouched.
	CHECK(apply(Json::object({{"Gain", 3.0}, {"Count", 9}})) == "Tuning.Count: must be between 1 and 8");
	CHECK(tuning.Gain == doctest::Approx(1.0f));

	CHECK(apply(Json::object({{"Gain", 0.0}, {"Tint", {0, 1, 0.5}}, {"Weights", Json::array()}, {"Count", 8}})).empty());
	CHECK(tuning.Gain == 0.0f);
	CHECK(tuning.Tint == glm::vec3(0.0f, 1.0f, 0.5f));
	CHECK(tuning.Weights.empty());
	CHECK(tuning.Count == 8u);
}

TEST_CASE("StructSerialization: descriptors describe kinds, hints, defaults and nested structs")
{
	std::vector<FieldDescriptor> const settings = GetFieldDescriptors<StructTraits<RenderSettings>, RenderSettings>();
	REQUIRE(settings.size() == 3);
	CHECK(settings[0].Kind == FieldKind::Float);
	CHECK(settings[0].Default == 1.0f);
	CHECK(settings[1].Kind == FieldKind::Struct);
	REQUIRE(settings[1].Fields.size() == 2);
	CHECK(settings[1].Fields[0].Name == "Enabled");
	CHECK(settings[1].Fields[0].Kind == FieldKind::Bool);
	CHECK(settings[2].Kind == FieldKind::Array);
	CHECK(settings[2].ElementKind == FieldKind::Float);
	CHECK(settings[2].TypeName == "float[]");

	std::vector<FieldDescriptor> const tuning = GetFieldDescriptors<StructTraits<Tuning>, Tuning>();
	REQUIRE(tuning.size() == 6);
	CHECK(tuning[0].Hints.Description == "Linear gain.");
	CHECK(tuning[1].Kind == FieldKind::Vec3);
	CHECK(tuning[1].Hints.Display == FieldDisplay::Color);
	CHECK(tuning[3].Kind == FieldKind::UInt);
	CHECK(tuning[3].Hints.Min == 1.0);
	// The range the C++ type represents, for editors.
	CHECK(tuning[3].TypeMin == 0.0);
	CHECK(tuning[3].TypeMax == static_cast<double>(std::numeric_limits<uint32_t>::max()));
	CHECK(tuning[0].TypeMax == static_cast<double>(std::numeric_limits<float>::max()));
	CHECK(tuning[1].TypeMin == static_cast<double>(std::numeric_limits<float>::lowest()));
	CHECK(tuning[4].TypeMin == -std::numeric_limits<double>::infinity());
	CHECK(tuning[4].Hints.Display == FieldDisplay::MultilineText);
	CHECK(tuning[5].Kind == FieldKind::Asset);
	CHECK(tuning[5].Hints.AssetTypeName == "Texture");

	Json const schema = DescribeFields<StructTraits<Tuning>, Tuning>();
	CHECK_FALSE(schema.contains("Description"));
	CHECK(schema["Fields"][0]["Description"] == "Linear gain.");
	CHECK(schema["Fields"][1]["Display"] == "Color");
	CHECK(schema["Fields"][3]["Min"] == 1);
	CHECK(schema["Fields"][3]["Min"].is_number_integer());
	CHECK(schema["Fields"][3]["Max"] == 8);
	CHECK(schema["Fields"][5]["AssetType"] == "Texture");
	CHECK(DescribeFields<StructTraits<RenderSettings>, RenderSettings>()["Fields"][1]["Fields"][1]["Name"] == "Intensity");
}

TEST_CASE("StructSerialization: renderer settings and material defaults satisfy their own ranges")
{
	auto const checkDefaults = []<typename T>(std::type_identity<T>)
	{
		for (FieldDescriptor const& field : GetFieldDescriptors<StructTraits<T>, T>())
		{
			CAPTURE(field.Name);
			T value;
			Json patch = Json::object();
			patch[std::string(field.Name)] = field.Default;
			Result<void> const result = DeserializeFields<StructTraits<T>>(patch, value, DeserializationContext{}, "settings");
			CHECK_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
		}
	};
	checkDefaults(std::type_identity<SceneRendererSettings>{});
	checkDefaults(std::type_identity<MaterialData>{});
}
