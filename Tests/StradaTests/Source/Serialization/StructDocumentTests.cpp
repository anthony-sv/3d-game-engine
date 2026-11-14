#include "Strada/Serialization/StructDocument.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// A settings document in its third version: version 1 called Volume "Loudness", version 2 stored it in percent.
	struct PlayerSettings
	{
		float Volume = 1.0f;
		bool Subtitles = false;
		std::vector<std::string> Profiles;
	};
}

template<>
struct Strada::StructTraits<PlayerSettings>
{
	static constexpr std::string_view Name = "PlayerSettings";
	static constexpr auto Fields =
		std::make_tuple(Field("Volume", &PlayerSettings::Volume).Range(0.0, 1.0), Field("Subtitles", &PlayerSettings::Subtitles),
	                    Field("Profiles", &PlayerSettings::Profiles).MaxCount(4));
};

namespace
{
	Result<void> MigrateFrom1(Json& content)
	{
		if (auto const loudness = content.find("Loudness"); loudness != content.end())
		{
			Json volume = *loudness;
			content.erase(loudness);
			content["Volume"] = std::move(volume);
		}
		return {};
	}

	// Reads the volume without checking it first, as a careless migration would: a value of another kind throws.
	Result<void> MigrateFrom2(Json& content)
	{
		if (auto const volume = content.find("Volume"); volume != content.end())
		{
			if (volume->is_string() && volume->get_ref<std::string const&>() == "refuse")
			{
				return Error{"the volume cannot be converted"};
			}
			if (volume->is_string() && volume->get_ref<std::string const&>() == "replace")
			{
				content = Json::array();
				return {};
			}
			*volume = volume->get<double>() / 100.0;
		}
		return {};
	}

	constexpr std::array<DocumentMigration, 2> Migrations = {&MigrateFrom1, &MigrateFrom2};
	constexpr StructDocumentFormat Format = {"PlayerSettings", 3, Migrations};

	Result<void> ValidateSettings(PlayerSettings const& settings)
	{
		if (settings.Subtitles && settings.Volume == 0.0f)
		{
			return Error{"subtitles need a volume"};
		}
		return {};
	}

	Json MakeDocument(int version, Json content)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("PlayerSettings", version);
		document["PlayerSettings"] = std::move(content);
		return document;
	}

	std::string ReadError(Json const& document, StructDocumentFormat const& format = Format)
	{
		Result<PlayerSettings> settings = ReadStructDocument<PlayerSettings>(document, format, &ValidateSettings, DeserializationContext{});
		REQUIRE(settings.IsError());
		return settings.GetError();
	}
}

TEST_CASE("StructDocument: documents of one struct round trip and migrate from every older version")
{
	PlayerSettings settings;
	settings.Volume = 0.5f;
	settings.Subtitles = true;
	settings.Profiles = {"Keyboard", "Gamepad"};
	Json const document = WriteStructDocument(settings, Format);
	CHECK(document == MakeDocument(3, Json::parse(R"({ "Volume": 0.5, "Subtitles": true, "Profiles": [ "Keyboard", "Gamepad" ] })")));
	CHECK(MakeStructDocument(Format, Json::object()) == MakeDocument(3, Json::object()));

	Result<PlayerSettings> read = ReadStructDocument<PlayerSettings>(document, Format, &ValidateSettings, DeserializationContext{});
	REQUIRE(read.IsOk());
	CHECK(read.GetValue().Volume == 0.5f);
	CHECK(read.GetValue().Subtitles);
	CHECK(read.GetValue().Profiles == settings.Profiles);

	// Each older version runs the migrations from its own on; missing fields keep their defaults.
	Result<PlayerSettings> fromVersion1 = ReadStructDocument<PlayerSettings>(MakeDocument(1, Json::object({{"Loudness", 25}})), Format,
	                                                                         &ValidateSettings, DeserializationContext{});
	REQUIRE(fromVersion1.IsOk());
	CHECK(fromVersion1.GetValue().Volume == 0.25f);
	CHECK_FALSE(fromVersion1.GetValue().Subtitles);
	Result<Json> content = ReadDocumentContent(MakeDocument(2, Json::object({{"Volume", 50}})), Format);
	REQUIRE(content.IsOk());
	CHECK(content.GetValue() == Json::object({{"Volume", 0.5}}));
	content = ReadDocumentContent(MakeDocument(3, Json::object({{"Volume", 50}})), Format);
	REQUIRE(content.IsOk());
	CHECK(content.GetValue() == Json::object({{"Volume", 50}}));

	// No validation when there is none to run.
	Result<PlayerSettings> unchecked = ReadStructDocument<PlayerSettings>(
		MakeDocument(3, Json::object({{"Volume", 0.0}, {"Subtitles", true}})), Format, nullptr, DeserializationContext{});
	CHECK(unchecked.IsOk());
}

TEST_CASE("StructDocument: damaged documents and failing migrations are refused with a reason")
{
	std::array<DocumentMigration, 1> const tooFew = {&MigrateFrom1};
	StructDocumentFormat const missingMigration = {"PlayerSettings", 3, tooFew};
	Json anotherType = MakeDocument(3, Json::object());
	anotherType["Strada"]["Type"] = "Scene";

	CHECK(ReadError(Json::array()) == "document must be a JSON object");
	CHECK(ReadError(anotherType) == "expected a PlayerSettings file but found 'Scene'");
	CHECK(ReadError(MakeDocument(0, Json::object())) == "invalid file version 0");
	CHECK(ReadError(MakeDocument(4, Json::object())) == "file version 4 is newer than the supported version 3");
	CHECK(ReadError(Json::object({{"Strada", MakeFileHeader("PlayerSettings", 3)}})) == "the document has no \"PlayerSettings\" object");
	CHECK(ReadError(MakeDocument(3, Json(3))) == "\"PlayerSettings\" must be a JSON object");
	CHECK(ReadError(MakeDocument(2, Json::object()), missingMigration) == "no migration from version 2 of PlayerSettings documents");
	// A migration that refuses, one that throws, and one that leaves no object.
	CHECK(ReadError(MakeDocument(2, Json::object({{"Volume", "refuse"}}))) ==
	      "PlayerSettings version 2 cannot be read: the volume cannot be converted");
	CHECK(ReadError(MakeDocument(2, Json::object({{"Volume", "loud"}})))
	          .starts_with("PlayerSettings version 2 cannot be read: unexpected content ("));
	CHECK(ReadError(MakeDocument(2, Json::object({{"Volume", "replace"}}))) ==
	      "PlayerSettings version 2 cannot be read: its migration left no object");
	// The fields, then the validation.
	CHECK(ReadError(MakeDocument(3, Json::object({{"Volume", 2.0}}))) == "PlayerSettings.Volume: must be between 0 and 1");
	CHECK(ReadError(MakeDocument(3, Json::object({{"Profiles", {"A", "B", "C", "D", "E"}}}))) ==
	      "PlayerSettings.Profiles: must hold at most 4 elements");
	CHECK(ReadError(MakeDocument(3, Json::object({{"Colour", "Red"}}))) ==
	      "document 'PlayerSettings' has no field 'Colour' (fields: Volume, Subtitles, Profiles)");
	CHECK(ReadError(MakeDocument(3, Json::object({{"Volume", 0.0}, {"Subtitles", true}}))) == "PlayerSettings: subtitles need a volume");
}
