#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/VersionedDocument.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <utility>

using namespace Strada;

namespace
{
	// A format of four versions: version 2 only added what version 1 readers refuse (no migration), version 3 renamed
	// "Size" to "Radius", version 4 again only added something.
	Result<void> RenameSize(Json& document)
	{
		auto const size = document.find("Size");
		if (size == document.end())
		{
			return {};
		}
		if (document.contains("Radius"))
		{
			return Error{"both Size and Radius"};
		}
		Json radius = *size;
		document.erase("Size");
		document["Radius"] = std::move(radius);
		return {};
	}

	Result<void> Throw(Json& document)
	{
		// nlohmann throws reading a string as a number.
		document["Number"] = document.at("Text").get<double>();
		return {};
	}

	Result<void> LeaveNoObject(Json& document)
	{
		document = Json::array();
		return {};
	}

	constexpr std::array<DocumentVersion, 3> TestVersions = {{
		{"added lists", nullptr},
		{"renamed Size to Radius", &RenameSize},
		{"added colors", nullptr},
	}};
	constexpr VersionedDocumentFormat TestFormat{"Gadget", TestVersions};

	Json MakeDocument(int version, Json members = Json::object())
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Gadget", version);
		for (auto const& [key, value] : members.items())
		{
			document[key] = value;
		}
		return document;
	}
}

TEST_CASE("VersionedDocument: the current version is one more than the versions after the first")
{
	CHECK(TestFormat.GetVersion() == 4);
	CHECK(VersionedDocumentFormat{"Single", {}}.GetVersion() == 1);
}

TEST_CASE("VersionedDocument: migrating in place runs the migrations after the document's version and names the current one")
{
	Json document = MakeDocument(1, Json::object({{"Size", 2.5}, {"Name", "Ball"}}));
	Result<int> const migrated = MigrateDocument(document, TestFormat);
	REQUIRE_MESSAGE(migrated.IsOk(), migrated.GetError());
	CHECK(migrated.GetValue() == 1);
	CHECK(document == MakeDocument(4, Json::object({{"Name", "Ball"}, {"Radius", 2.5}})));

	// From version 3 on, nothing runs and the document stays as it is.
	Json current = MakeDocument(3, Json::object({{"Size", 1.0}}));
	REQUIRE(MigrateDocument(current, TestFormat).GetValue() == 3);
	CHECK(current == MakeDocument(4, Json::object({{"Size", 1.0}})));
	Json newest = MakeDocument(4, Json::object({{"Size", 1.0}}));
	REQUIRE(MigrateDocument(newest, TestFormat).GetValue() == 4);
	CHECK(newest == MakeDocument(4, Json::object({{"Size", 1.0}})));
}

TEST_CASE("VersionedDocument: reading leaves the caller's document and copies it only when a migration runs")
{
	// Version 3 needs none of the migrations after it: the document itself is the result.
	Json const version3 = MakeDocument(3, Json::object({{"Size", 1.0}}));
	Result<MigratedDocument> const inPlace = ReadMigratedDocument(version3, TestFormat);
	REQUIRE(inPlace.IsOk());
	CHECK_FALSE(inPlace.GetValue().IsCopy());
	CHECK(&inPlace.GetValue().Get() == &version3);
	CHECK(inPlace.GetValue().GetFileVersion() == 3);

	// Version 2 needs the rename: a migrated copy, the caller's document unchanged.
	Json const version2 = MakeDocument(2, Json::object({{"Size", 1.0}}));
	Json const before = version2;
	Result<MigratedDocument> const copied = ReadMigratedDocument(version2, TestFormat);
	REQUIRE(copied.IsOk());
	CHECK(copied.GetValue().IsCopy());
	CHECK(copied.GetValue().GetFileVersion() == 2);
	CHECK(copied.GetValue().Get() == MakeDocument(4, Json::object({{"Radius", 1.0}})));
	CHECK(version2 == before);
}

TEST_CASE("VersionedDocument: formats whose versions need no migration read every version in place")
{
	constexpr std::array<DocumentVersion, 2> Additive = {{{"added lists", nullptr}, {"added colors", nullptr}}};
	VersionedDocumentFormat const format{"Gadget", Additive};
	for (int version = 1; version <= 3; version++)
	{
		CAPTURE(version);
		Json const document = MakeDocument(version, Json::object({{"Size", 1.0}}));
		Result<MigratedDocument> const read = ReadMigratedDocument(document, format);
		REQUIRE(read.IsOk());
		CHECK(&read.GetValue().Get() == &document);
		Json owned = document;
		REQUIRE(MigrateDocument(owned, format).GetValue() == version);
		CHECK(owned == MakeDocument(3, Json::object({{"Size", 1.0}})));
	}
}

TEST_CASE("VersionedDocument: headers of another type or version, and migrations that fail, refuse the document")
{
	Json other = MakeDocument(1);
	other["Strada"]["Type"] = "Scene";
	CHECK(ReadMigratedDocument(other, TestFormat).GetError() == "expected a Gadget file but found 'Scene'");
	CHECK(MigrateDocument(other, TestFormat).GetError() == "expected a Gadget file but found 'Scene'");

	Json newer = MakeDocument(5);
	CHECK(ReadMigratedDocument(newer, TestFormat).GetError() == "file version 5 is newer than the supported version 4");
	CHECK(MigrateDocument(newer, TestFormat).IsError());
	Json zero = MakeDocument(0);
	CHECK(ReadMigratedDocument(zero, TestFormat).GetError() == "invalid file version 0");
	CHECK(ReadMigratedDocument(Json::array(), TestFormat).IsError());

	Json both = MakeDocument(1, Json::object({{"Size", 1.0}, {"Radius", 2.0}}));
	CHECK(ReadMigratedDocument(both, TestFormat).GetError() == "Gadget version 2 cannot be read: both Size and Radius");
	CHECK(MigrateDocument(both, TestFormat).GetError() == "Gadget version 2 cannot be read: both Size and Radius");

	constexpr std::array<DocumentVersion, 1> Throwing = {{{"converted Text", &Throw}}};
	Json text = MakeDocument(1, Json::object({{"Text", "seven"}}));
	Result<MigratedDocument> const thrown = ReadMigratedDocument(text, VersionedDocumentFormat{"Gadget", Throwing});
	REQUIRE(thrown.IsError());
	CHECK(thrown.GetError().starts_with("Gadget version 1 cannot be read: unexpected content ("));

	constexpr std::array<DocumentVersion, 1> Emptying = {{{"emptied", &LeaveNoObject}}};
	Json emptied = MakeDocument(1);
	CHECK(MigrateDocument(emptied, VersionedDocumentFormat{"Gadget", Emptying}).GetError() ==
	      "Gadget version 1 cannot be read: its migration left no object");
}
