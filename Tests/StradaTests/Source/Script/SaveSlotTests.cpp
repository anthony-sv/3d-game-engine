#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// Save slots (Docs/Architecture.md §13.1, "Saves"; §7.3, "Save slot"): the .ssave format, its newer and damaged rules,
// the names and values it takes, and the store below the game data folder.
namespace
{
	ScriptFieldValue MakeList(ScriptFieldType elementType, std::vector<ScriptFieldValue> elements)
	{
		return ScriptFieldValue::FromList(elementType, std::move(elements));
	}

	// One value of every element type and a list of each, the values a lap's ghost and a race's splits are among them.
	SaveSlotData MakeEveryTypeSlot()
	{
		SaveSlotData slot;
		ScriptFieldMap& values = slot.Values;
		values["Linked"] = ScriptFieldValue::FromBool(true);
		values["Laps"] = ScriptFieldValue::FromInt32(-3);
		values["Gear"] = ScriptFieldValue::FromUInt32(6);
		values["Distance"] = ScriptFieldValue::FromInt64(-9000000000);
		values["Seed"] = ScriptFieldValue::FromUInt64(std::numeric_limits<uint64_t>::max());
		values["BestLap"] = ScriptFieldValue::FromFloat(92.418f);
		values["Total"] = ScriptFieldValue::FromDouble(277.25125);
		values["Rider"] = ScriptFieldValue::FromString("Rider \"Ace\"\n\xC3\xA9");
		values["Offset"] = ScriptFieldValue::FromVector2({0.5f, -1.25f});
		values["Start"] = ScriptFieldValue::FromVector3({1.0f, 0.5f, -2.0f});
		values["Plane"] = ScriptFieldValue::FromVector4({0.0f, 1.0f, 0.0f, -3.5f});
		values["Heading"] = ScriptFieldValue::FromQuaternion(glm::quat::wxyz(0.0f, 0.0f, 1.0f, 0.0f));
		values["Tint"] = ScriptFieldValue::FromColor({1.0f, 0.5f, 0.25f, 1.0f});
		values["Checkpoint"] = ScriptFieldValue::FromEntity(UUID(12345));
		values["Bike"] = ScriptFieldValue::FromPrefab(AssetHandle(UUID(678)));
		values["Track"] = ScriptFieldValue::FromAsset(AssetHandle(UUID(910)));

		values["Ghost.Positions"] = MakeList(ScriptFieldType::Vector3, {ScriptFieldValue::FromVector3({0.0f, 0.5f, 0.0f}),
		                                                                ScriptFieldValue::FromVector3({3.1f, 0.5f, 0.2f})});
		values["Ghost.Rotations"] =
			MakeList(ScriptFieldType::Quaternion, {ScriptFieldValue::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f))});
		values["Ghost.Speeds"] = MakeList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(0.0f), ScriptFieldValue::FromFloat(61.5f)});
		values["Splits"] = MakeList(ScriptFieldType::Float, {});
		values["Flags"] = MakeList(ScriptFieldType::Bool, {ScriptFieldValue::FromBool(false)});
		values["Scores"] = MakeList(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(1), ScriptFieldValue::FromInt32(-2)});
		values["Counts"] = MakeList(ScriptFieldType::UInt32, {ScriptFieldValue::FromUInt32(4000000000u)});
		values["Times"] = MakeList(ScriptFieldType::Int64, {ScriptFieldValue::FromInt64(std::numeric_limits<int64_t>::min())});
		values["Ids"] = MakeList(ScriptFieldType::UInt64, {ScriptFieldValue::FromUInt64(42)});
		values["Precise"] = MakeList(ScriptFieldType::Double, {ScriptFieldValue::FromDouble(0.1)});
		values["Names"] = MakeList(ScriptFieldType::String, {ScriptFieldValue::FromString(""), ScriptFieldValue::FromString("b")});
		values["Path"] = MakeList(ScriptFieldType::Vector2, {ScriptFieldValue::FromVector2({1.0f, 2.0f})});
		values["Planes"] = MakeList(ScriptFieldType::Vector4, {ScriptFieldValue::FromVector4({1.0f, 2.0f, 3.0f, 4.0f})});
		values["Palette"] = MakeList(ScriptFieldType::Color, {ScriptFieldValue::FromColor({0.0f, 0.0f, 0.0f, 1.0f})});
		values["Marshals"] =
			MakeList(ScriptFieldType::Entity, {ScriptFieldValue::FromEntity(UUID(1)), ScriptFieldValue::FromEntity(UUID(2))});
		values["Props"] = MakeList(ScriptFieldType::Prefab, {ScriptFieldValue::FromPrefab(AssetHandle(UUID(3)))});
		values["Assets"] = MakeList(ScriptFieldType::Asset, {ScriptFieldValue::FromAsset(AssetHandle(UUID(4)))});
		return slot;
	}

	std::string WriteText(SaveSlotData const& slot)
	{
		Result<std::string> text = WriteSaveSlotText(slot);
		REQUIRE_MESSAGE(text.IsOk(), (text.IsOk() ? std::string() : text.GetError()));
		return text.TakeValue();
	}

	// The text of a document with a header of the version over the content (which may be anything).
	std::string MakeHeaderText(std::string_view version, std::string_view rest)
	{
		return "{\n\t\"Strada\": { \"Version\": " + std::string(version) + ", \"Type\": \"SaveSlot\" }" + std::string(rest);
	}

	// Pads a slot's text past MaxSaveSlotBytes with a string value, keeping it a valid document.
	std::string MakeOversizeText(std::string_view version)
	{
		std::string const padding(static_cast<size_t>(MaxSaveSlotBytes), 'x');
		return MakeHeaderText(version, ",\n\t\"SaveSlot\": { \"Values\": { \"Padding\": { \"Type\": \"String\", \"Value\": \"" + padding +
		                                   "\" } } }\n}\n");
	}

	std::string ReadWholeFile(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE(text.IsOk());
		return text.TakeValue();
	}

	// Whether the document's header names a slot of a version above 1 (the fuzz case's rule for Newer).
	bool NamesNewerSlot(Json const& document)
	{
		if (!document.is_object() || !document.contains("Strada") || !document["Strada"].is_object())
		{
			return false;
		}
		Json const& header = document["Strada"];
		if (!header.contains("Type") || header["Type"] != "SaveSlot" || !header.contains("Version") ||
		    !header["Version"].is_number_integer())
		{
			return false;
		}
		return header["Version"].is_number_unsigned() ? header["Version"].get<uint64_t>() > 1 : header["Version"].get<int64_t>() > 1;
	}
}

TEST_CASE("SaveSlot: a slot of every value type and their lists round trips, writes the same text again and warns about "
          "unknown fields")
{
	SaveSlotData const slot = MakeEveryTypeSlot();
	std::string const text = WriteText(slot);
	// The text is what DumpJson writes of the document, written without building it; the header comes first, so that a
	// reader finds a later version's without reading the rest.
	CHECK(text == DumpJson(WriteSaveSlot(slot)));
	CHECK(text.starts_with("{\n\t\"Strada\": {"));
	Result<Json> const document = ParseJson(text);
	REQUIRE(document.IsOk());
	CHECK(document.GetValue()["Strada"]["Type"] == "SaveSlot");
	CHECK(document.GetValue()["Strada"]["Version"] == 1);
	CHECK(document.GetValue()["SaveSlot"]["Values"]["Ghost.Positions"]["Type"] == "Vector3[]");
	CHECK(document.GetValue()["SaveSlot"]["Values"]["Seed"]["Value"] == "18446744073709551615");

	SaveSlotRead const read = ReadSaveSlotText(text);
	REQUIRE(read.Status == SaveSlotStatus::Present);
	CHECK(read.Version == 1);
	CHECK(read.Problem.empty());
	CHECK(read.Warnings.empty());
	CHECK(read.Data == slot);
	CHECK(WriteText(read.Data) == text);
	// What scripts are given of a slot read is the same text.
	CHECK(WriteReadSaveSlotText(read.Data) == text);
	CHECK(ReadSaveSlotVersion(text) == 1);

	// An empty slot is a document too.
	CHECK(WriteText({}) == DumpJson(WriteSaveSlot({})));
	SaveSlotRead const empty = ReadSaveSlotText(WriteText({}));
	CHECK(empty.Status == SaveSlotStatus::Present);
	CHECK(empty.Data.Values.empty());

	// A compatible field of a later engine is skipped with a warning; a byte order mark is read past.
	Json extended = document.GetValue();
	extended["SaveSlot"]["Later"] = 1;
	SaveSlotRead const later = ReadSaveSlotText("\xEF\xBB\xBF" + DumpJson(extended));
	REQUIRE(later.Status == SaveSlotStatus::Present);
	CHECK(later.Warnings.size() == 1);
	CHECK(later.Data == slot);

	// A quaternion reads back normalized, as in scenes, and then writes the same text again.
	SaveSlotData scaled;
	scaled.Values["Heading"] = ScriptFieldValue::FromQuaternion(glm::quat::wxyz(2.0f, 0.0f, 0.0f, 0.0f));
	SaveSlotRead const normalized = ReadSaveSlotText(WriteText(scaled));
	REQUIRE(normalized.Status == SaveSlotStatus::Present);
	CHECK(normalized.Data.Values.at("Heading").GetQuaternion() == glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("SaveSlot: the version 1 fixture reads, and writes and reads back alike")
{
	std::filesystem::path const path = Testing::GetTestDataPath("Saves/Version1.ssave");
	SaveSlotRead const read = ReadSaveSlotText(ReadWholeFile(path));
	REQUIRE_MESSAGE(read.Status == SaveSlotStatus::Present, read.Problem);
	ScriptFieldMap const& values = read.Data.Values;
	REQUIRE(values.size() == 13);
	CHECK(values.at("BestLap").GetFloat() == 92.418f);
	CHECK(values.at("Laps").GetInt32() == 3);
	CHECK(values.at("Ghost.Positions").GetType() == MakeScriptFieldListType(ScriptFieldType::Vector3));
	REQUIRE(values.at("Ghost.Positions").GetElements().size() == 3);
	CHECK(values.at("Ghost.Positions").GetElements()[1].GetVector3() == glm::vec3(3.1f, 0.5f, 0.2f));
	CHECK(values.at("Splits").GetElements().size() == 2);
	CHECK(values.at("Total").GetInt64() == 9000000000);
	CHECK(values.at("Rider").GetString() == "Rider");
	CHECK(values.at("Bike").GetType() == ScriptFieldType::Prefab);
	CHECK(values.at("Bike").GetAsset() == AssetHandle(UUID(678)));

	SaveSlotRead const again = ReadSaveSlotText(WriteText(read.Data));
	REQUIRE(again.Status == SaveSlotStatus::Present);
	CHECK(again.Data == read.Data);
}

TEST_CASE("SaveSlot: a slot of a newer version is told apart from a damaged one, whatever follows its header")
{
	std::string const valid = WriteText(MakeEveryTypeSlot());
	auto const checkNewer = [](std::string_view slotText, int expectedVersion)
	{
		SaveSlotRead const found = ReadSaveSlotText(slotText);
		CHECK(found.Status == SaveSlotStatus::Newer);
		CHECK(found.Version == expectedVersion);
		CHECK(found.Data.Values.empty());
		CHECK(found.Problem.find("newer") != std::string::npos);
	};

	// A later version's header over content this engine cannot read, or over text that is no JSON at all.
	checkNewer(MakeHeaderText("2", ",\n\t\"SaveSlot\": { \"Values\": { \"Ghost\": { \"Type\": \"Spline\", \"Value\": [] } } }\n}\n"), 2);
	checkNewer(MakeHeaderText("2", ",\n\t\"SaveSlot\": [garbage"), 2);
	checkNewer(MakeHeaderText("2", ""), 2);
	checkNewer(MakeHeaderText("9223372036854775807", "}"), INT_MAX);
	checkNewer(MakeHeaderText("18446744073709551615", "}"), INT_MAX);
	// A header that does not come first is found by the scanner, the last of repeated ones as the parser keeps it.
	checkNewer("{ \"SaveSlot\": { \"Values\": {} }, \"Strada\": { \"Type\": \"SaveSlot\", \"Version\": 3 } }", 3);
	checkNewer("{ \"Other\": 1, \"Strada\": { \"Type\": \"SaveSlot\", \"Version\": 1 }, \"Strada\": { \"Type\": \"SaveSlot\", "
	           "\"Version\": 4 } }",
	           4);
	// A file larger than a slot may be, of a later version: its header still says so.
	checkNewer(MakeOversizeText("2"), 2);
	checkNewer(MakeOversizeText("2").substr(0, static_cast<size_t>(MaxSaveSlotHeaderBytes)), 2);

	std::vector<std::pair<std::string, std::string>> damaged = {
		{"empty", ""},
		{"not JSON", "not a slot"},
		{"truncated", valid.substr(0, valid.size() / 2)},
		{"an array", "[]"},
		{"no header", "{ \"SaveSlot\": { \"Values\": {} } }"},
		{"another type", "{ \"Strada\": { \"Version\": 1, \"Type\": \"InputBindingProfile\" }, \"SaveSlot\": {} }"},
		{"version 0", MakeHeaderText("0", ", \"SaveSlot\": {} }")},
		{"a version that is no integer", MakeHeaderText("1.5", ", \"SaveSlot\": {} }")},
		{"no content", MakeHeaderText("1", " }")},
		{"values that are no map", MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": [] } }")},
		{"an unknown type", MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"A\": { \"Type\": \"Spline\", \"Value\": 1 } } } }")},
		{"a value of the wrong kind",
	     MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"A\": { \"Type\": \"Float\", \"Value\": \"x\" } } } }")},
		{"null for a number", MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"A\": { \"Type\": \"Float\", \"Value\": null } } } }")},
		{"a zero quaternion",
	     MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"A\": { \"Type\": \"Quaternion\", \"Value\": [0, 0, 0, 0] } } } }")},
		{"an empty name", MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"\": { \"Type\": \"Bool\", \"Value\": true } } } }")},
		{"a control character in a name",
	     MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"A\\u0001\": { \"Type\": \"Bool\", \"Value\": true } } } }")},
		{"a name too long", MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"" + std::string(MaxSavedValueNameLength + 1, 'n') +
	                                                "\": { \"Type\": \"Bool\", \"Value\": true } } } }")},
		{"larger than a slot may be", MakeOversizeText("1")},
	};
	std::string list = "[0";
	std::string values = "{ \"V0\": { \"Type\": \"Bool\", \"Value\": true }";
	for (size_t index = 1; index <= MaxScriptFieldListElements; index++)
	{
		list += ", " + std::to_string(index);
		values += ", \"V" + std::to_string(index) + "\": { \"Type\": \"Bool\", \"Value\": true }";
	}
	damaged.emplace_back(
		"a list of 4,097 elements",
		MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": { \"A\": { \"Type\": \"Int32[]\", \"Value\": " + list + "] } } } }"));
	damaged.emplace_back("4,097 values", MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": " + values + " } } }"));
	for (auto const& [kind, text] : damaged)
	{
		CAPTURE(kind);
		SaveSlotRead const read = ReadSaveSlotText(text);
		CHECK(read.Status == SaveSlotStatus::Damaged);
		CHECK(read.Data.Values.empty());
		CHECK_FALSE(read.Problem.empty());
	}
	CHECK(ReadSaveSlotText(MakeOversizeText("1")).Version == 1);
	CHECK(ReadSaveSlotText("not a slot").Version == 0);
	CHECK_FALSE(ReadSaveSlotVersion("not a slot").has_value());
	CHECK_FALSE(ReadSaveSlotVersion("{ \"Strada\": { \"Version\": 2, \"Type\": \"Scene\" } }").has_value());
}

TEST_CASE("SaveSlot: a value's name is UTF-8 of 1 to 128 bytes without control characters, and a value is finite and of a "
          "type")
{
	CHECK(ValidateSavedValueName("Ghost.Positions").IsOk());
	CHECK(ValidateSavedValueName("Caf\xC3\xA9 Lap 3").IsOk());
	CHECK(ValidateSavedValueName(std::string(MaxSavedValueNameLength, 'n')).IsOk());
	CHECK_FALSE(ValidateSavedValueName("").IsOk());
	CHECK_FALSE(ValidateSavedValueName(std::string(MaxSavedValueNameLength + 1, 'n')).IsOk());
	CHECK_FALSE(ValidateSavedValueName("bad\xFF").IsOk());
	CHECK_FALSE(ValidateSavedValueName("line\nbreak").IsOk());
	CHECK_FALSE(ValidateSavedValueName("delete\x7F").IsOk());
	CHECK_FALSE(ValidateSavedValueName("next\xC2\x85line").IsOk());

	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const infinity = std::numeric_limits<float>::infinity();
	CHECK(ValidateSavedValue(ScriptFieldValue::FromFloat(std::numeric_limits<float>::max())).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue()).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue::FromFloat(nan)).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue::FromDouble(std::numeric_limits<double>::infinity())).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue::FromVector2({0.0f, infinity})).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue::FromVector3({nan, 0.0f, 0.0f})).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue::FromColor({0.0f, 0.0f, 0.0f, nan})).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue::FromQuaternion(glm::quat::wxyz(0.0f, 0.0f, 0.0f, 0.0f))).IsOk());
	CHECK_FALSE(ValidateSavedValue(ScriptFieldValue::FromQuaternion(glm::quat::wxyz(nan, 0.0f, 0.0f, 0.0f))).IsOk());
	std::vector<ScriptFieldValue> elements(MaxScriptFieldListElements, ScriptFieldValue::FromFloat(1.0f));
	CHECK(ValidateSavedValue(ScriptFieldValue::FromList(ScriptFieldType::Float, elements)).IsOk());
	elements.back() = ScriptFieldValue::FromFloat(infinity);
	Result<void> const list = ValidateSavedValue(ScriptFieldValue::FromList(ScriptFieldType::Float, elements));
	REQUIRE_FALSE(list.IsOk());
	CHECK(list.GetError() == "element 4095: the number is not finite");

	SaveSlotData slot;
	for (size_t index = 0; index < MaxSaveSlotValues; index++)
	{
		slot.Values["V" + std::to_string(index)] = ScriptFieldValue::FromBool(true);
	}
	CHECK(ValidateSaveSlot(slot).IsOk());
	slot.Values["One more"] = ScriptFieldValue::FromBool(true);
	CHECK_FALSE(ValidateSaveSlot(slot).IsOk());
	CHECK_FALSE(WriteSaveSlotText(slot).IsOk());

	SaveSlotData badName;
	badName.Values[""] = ScriptFieldValue::FromBool(true);
	CHECK_FALSE(WriteSaveSlotText(badName).IsOk());

	// A slot whose text would pass MaxSaveSlotBytes is refused whole.
	SaveSlotData large;
	std::string const megabyte(1024 * 1024, 'x');
	for (int index = 0; index <= 16; index++)
	{
		large.Values["Part" + std::to_string(index)] = ScriptFieldValue::FromString(megabyte);
	}
	Result<std::string> const tooLarge = WriteSaveSlotText(large);
	REQUIRE_FALSE(tooLarge.IsOk());
	CHECK(tooLarge.GetError().find("more than 16777216") != std::string::npos);
}

TEST_CASE("SaveSlotStore: a slot's name is portable, of 1 to 64 characters, and names one file in any letter case")
{
	CHECK(SaveSlotStore::MakeFileStem("Best").GetValue() == "best");
	CHECK(SaveSlotStore::MakeFileStem("Lap 3_Best-Time").GetValue() == "lap 3_best-time");
	CHECK(SaveSlotStore::MakeFileStem(std::string(MaxSaveSlotNameLength, 'a')).IsOk());
	CHECK(SaveSlotStore::MakeFileStem("Console").IsOk());
	for (std::string const& name : {std::string(), std::string(" best"), std::string("best "), std::string("CON"), std::string("nul"),
	                                std::string("com1"), std::string("a/b"), std::string("a\\b"), std::string("a.b"), std::string(".."),
	                                std::string(MaxSaveSlotNameLength + 1, 'a'), std::string("caf\xC3\xA9"), std::string("tab\there")})
	{
		CAPTURE(name);
		CHECK_FALSE(SaveSlotStore::MakeFileStem(name).IsOk());
	}
}

TEST_CASE("SaveSlotStore: slots are read, written, listed and deleted below the game data folder, and never without one")
{
	SaveSlotData const slot = MakeEveryTypeSlot();
	REQUIRE_FALSE(GameData::GetDirectory().has_value());
	CHECK_FALSE(SaveSlotStore::GetDirectory().has_value());
	CHECK(SaveSlotStore::Read("Best").GetValue().Status == SaveSlotStatus::Unavailable);
	CHECK_FALSE(SaveSlotStore::Write("Best", slot).IsOk());
	CHECK_FALSE(SaveSlotStore::Delete("Best").IsOk());
	CHECK(SaveSlotStore::GetNames().empty());

	Testing::TemporaryDirectory directory;
	std::filesystem::path const data = directory.GetPath() / "Data";
	GameDataScope const gameData(data, GameDataLifetime::Kept);
	REQUIRE(SaveSlotStore::GetDirectory() == data / "Saves");
	CHECK_FALSE(SaveSlotStore::Read("a/b").IsOk());
	CHECK_FALSE(SaveSlotStore::Write("a/b", slot).IsOk());
	CHECK_FALSE(SaveSlotStore::Delete("a/b").IsOk());

	CHECK(SaveSlotStore::Read("Best").GetValue().Status == SaveSlotStatus::Missing);
	// Reading creates nothing.
	CHECK_FALSE(FileSystem::Exists(data));
	CHECK(SaveSlotStore::GetNames().empty());
	REQUIRE(SaveSlotStore::Write("Best", slot).IsOk());
	std::filesystem::path const file = data / "Saves" / "best.ssave";
	CHECK(ReadWholeFile(file) == WriteText(slot));
	SaveSlotRead const read = SaveSlotStore::Read("BEST").GetValue();
	CHECK(read.Status == SaveSlotStatus::Present);
	CHECK(read.Data == slot);

	REQUIRE(SaveSlotStore::Write("zeta", {}).IsOk());
	REQUIRE(SaveSlotStore::Write("Alpha 2", {}).IsOk());
	// Left out of the list: an interrupted write's temporary file, a name the store would not write, another
	// extension, and a folder.
	REQUIRE(FileSystem::WriteTextFile(data / "Saves" / ("best.ssave.tmp" + UUID().ToString()), "x").IsOk());
	REQUIRE(FileSystem::WriteTextFile(data / "Saves" / "a.b.ssave", "x").IsOk());
	REQUIRE(FileSystem::WriteTextFile(data / "Saves" / "notes.txt", "x").IsOk());
	REQUIRE(FileSystem::CreateDirectories(data / "Saves" / "folder.ssave").IsOk());
	CHECK(SaveSlotStore::GetNames() == std::vector<std::string>{"alpha 2", "best", "zeta"});

	REQUIRE(SaveSlotStore::Delete("Best").IsOk());
	CHECK_FALSE(FileSystem::Exists(file));
	CHECK(SaveSlotStore::Read("best").GetValue().Status == SaveSlotStatus::Missing);
	// Deleting a slot without a file succeeds.
	CHECK(SaveSlotStore::Delete("Best").IsOk());
	CHECK(SaveSlotStore::GetNames() == std::vector<std::string>{"alpha 2", "zeta"});
}

TEST_CASE("SaveSlotStore: a newer slot is never written over or deleted, and a damaged one is replaced only by a write or "
          "a delete")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const data = directory.GetPath() / "Data";
	GameDataScope const gameData(data, GameDataLifetime::Kept);
	std::filesystem::path const file = data / "Saves" / "best.ssave";
	SaveSlotData const slot = MakeEveryTypeSlot();

	for (std::string const& newer : {MakeHeaderText("2", ",\n\t\"SaveSlot\": { \"Ghost\": \"a later form\" }\n}\n"), MakeOversizeText("3"),
	                                 std::string("{ \"SaveSlot\": {}, \"Strada\": { \"Type\": \"SaveSlot\", \"Version\": 5 } }")})
	{
		REQUIRE(FileSystem::WriteTextFile(file, newer).IsOk());
		SaveSlotRead const read = SaveSlotStore::Read("Best").GetValue();
		CHECK(read.Status == SaveSlotStatus::Newer);
		CHECK(read.Version > 1);
		Result<void> const written = SaveSlotStore::Write("Best", slot);
		REQUIRE_FALSE(written.IsOk());
		CHECK(written.GetError().find("newer") != std::string::npos);
		Result<void> const deleted = SaveSlotStore::Delete("best");
		REQUIRE_FALSE(deleted.IsOk());
		CHECK(deleted.GetError().find("newer") != std::string::npos);
		CHECK(ReadWholeFile(file) == newer);
	}

	// A damaged slot stays on disk when it is read, and an explicit write replaces it.
	REQUIRE(FileSystem::WriteTextFile(file, "not a slot").IsOk());
	SaveSlotRead const damaged = SaveSlotStore::Read("Best").GetValue();
	CHECK(damaged.Status == SaveSlotStatus::Damaged);
	CHECK(ReadWholeFile(file) == "not a slot");
	REQUIRE(SaveSlotStore::Write("Best", slot).IsOk());
	CHECK(SaveSlotStore::Read("Best").GetValue().Data == slot);
	// A damaged slot larger than a slot may be is replaced too, and one of version 1 with a bad value is deleted.
	REQUIRE(FileSystem::WriteTextFile(file, MakeOversizeText("1")).IsOk());
	CHECK(SaveSlotStore::Read("Best").GetValue().Status == SaveSlotStatus::Damaged);
	REQUIRE(SaveSlotStore::Write("Best", slot).IsOk());
	REQUIRE(FileSystem::WriteTextFile(file, MakeHeaderText("1", ", \"SaveSlot\": { \"Values\": 3 } }")).IsOk());
	REQUIRE(SaveSlotStore::Delete("Best").IsOk());
	CHECK_FALSE(FileSystem::Exists(file));

	// A folder in the slot's place reads as damaged and is kept: what cannot be read may be a newer slot.
	REQUIRE(FileSystem::WriteTextFile(file / "inside.txt", "x").IsOk());
	CHECK(SaveSlotStore::Read("Best").GetValue().Status == SaveSlotStatus::Damaged);
	CHECK_FALSE(SaveSlotStore::Write("Best", slot).IsOk());
	CHECK_FALSE(SaveSlotStore::Delete("Best").IsOk());
	CHECK(ReadWholeFile(file / "inside.txt") == "x");
}

TEST_CASE("SaveSlot: damaged slots are read, refused or found newer, and what is read writes and reads back alike")
{
	std::vector<Json> seeds;
	seeds.push_back(WriteSaveSlot(MakeEveryTypeSlot()));
	seeds.push_back(Testing::LoadTestDataDocument("Saves/Version1.ssave"));
	// An empty slot, whose header is a large part of the document, so that mutations reach its version often.
	seeds.push_back(WriteSaveSlot({}));

	Testing::DocumentMutator mutator(6200);
	int read = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(400); round < rounds; round++)
	{
		CAPTURE(round);
		Json const damaged = mutator.Damage(seeds[static_cast<size_t>(round) % seeds.size()], round);
		SaveSlotRead const loaded = ReadSaveSlotText(DumpJson(damaged));
		// A header that names a slot of a later version always reads as Newer, whatever else the mutation did.
		CHECK((loaded.Status == SaveSlotStatus::Newer) == NamesNewerSlot(damaged));
		if (loaded.Status == SaveSlotStatus::Newer)
		{
			CHECK(loaded.Version > 1);
			continue;
		}
		if (loaded.Status != SaveSlotStatus::Present)
		{
			REQUIRE(loaded.Status == SaveSlotStatus::Damaged);
			CHECK_FALSE(loaded.Problem.empty());
			refused++;
			continue;
		}
		read++;
		Result<std::string> const text = WriteSaveSlotText(loaded.Data);
		REQUIRE_MESSAGE(text.IsOk(), (text.IsOk() ? std::string() : text.GetError()));
		CHECK(text.GetValue() == DumpJson(WriteSaveSlot(loaded.Data)));
		SaveSlotRead const again = ReadSaveSlotText(text.GetValue());
		REQUIRE(again.Status == SaveSlotStatus::Present);
		CHECK(again.Data == loaded.Data);
		CHECK(WriteText(again.Data) == text.GetValue());
	}
	CHECK(read > 0);
	CHECK(refused > 0);

	// A later version's header over damaged content, of any kind, reads as Newer with its version.
	for (int round = 0, rounds = Testing::GetFuzzRounds(100); round < rounds; round++)
	{
		CAPTURE(round);
		Json document = seeds[static_cast<size_t>(round) % seeds.size()];
		document["SaveSlot"] = mutator.Damage(document["SaveSlot"], round);
		document["Strada"]["Version"] = 2 + round;
		SaveSlotRead const loaded = ReadSaveSlotText(DumpJson(document));
		CHECK(loaded.Status == SaveSlotStatus::Newer);
		CHECK(loaded.Version == 2 + round);
	}
}
