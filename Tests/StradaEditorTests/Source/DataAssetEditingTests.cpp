#include "Asset/TestDataAsset.h"
#include "TestUtilities.h"

#include "Editor/Commands/SetDataAssetCommand.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Strada;
using Strada::Testing::TestData;
using Strada::Testing::TestDataShape;

namespace
{
	// A project open in an editor model, with the tests' data asset type registered around it.
	struct DataAssetFixture
	{
		// Declared first: it unregisters after the asset directory closed.
		Testing::TestDataTypeScope Type;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};

		DataAssetFixture() { REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk()); }

		std::filesystem::path GetFile(std::string_view path) const
		{
			return Context.GetProject()->GetAssetDirectory() / FileSystem::PathFromUtf8(path);
		}

		std::string ReadFile(std::string_view path) const
		{
			Result<Buffer> const bytes = FileSystem::ReadBinaryFile(GetFile(path));
			REQUIRE(bytes.IsOk());
			return std::string(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
		}

		// The file's document, read as JSON.
		Json ReadDocument(std::string_view path) const
		{
			Json document = Json::parse(ReadFile(path), nullptr, false);
			REQUIRE(document.is_object());
			return document;
		}

		AssetHandle Create(std::string_view path, Json const& fields = Json::object())
		{
			Result<AssetHandle> const created = Operations.CreateDataAsset(Type.GetType(), path, fields);
			REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
			return created.GetValue();
		}

		static Ref<TypedDataAsset<TestData>> Get(AssetHandle asset)
		{
			Ref<TypedDataAsset<TestData>> loaded = AssetManager::GetAsset<TypedDataAsset<TestData>>(asset);
			REQUIRE(loaded != nullptr);
			return loaded;
		}

		// The assets the refresh found modified.
		std::vector<AssetHandle> Refresh()
		{
			Result<AssetRefreshResult> const refreshed = Operations.RefreshAssets();
			REQUIRE(refreshed.IsOk());
			return refreshed.GetValue().Modified;
		}

		size_t GetUndoCount() const { return Context.GetHistory().GetUndoCount(); }
	};

	void MoveWriteTime(std::filesystem::path const& file)
	{
		std::error_code error;
		std::filesystem::file_time_type const written = std::filesystem::last_write_time(file, error);
		REQUIRE_FALSE(error);
		std::filesystem::last_write_time(file, written + std::chrono::seconds(10), error);
		REQUIRE_FALSE(error);
	}
}

TEST_CASE("EditorOperations: data asset files hold the default fields plus the given ones, read strictly, never over another file")
{
	DataAssetFixture fixture;
	AssetHandle const crate =
		fixture.Create("Data/Crate.stestdata", Json::object({{"Size", 2.5}, {"Entries", Json::array({{{"Name", "A"}}})}}));
	CHECK(AssetManager::FindByPath("Data/Crate.stestdata") == crate);
	CHECK(AssetManager::GetAssetType(crate) == fixture.Type.GetType());
	Ref<TypedDataAsset<TestData>> const loaded = DataAssetFixture::Get(crate);
	TestData const& data = loaded->GetData();
	CHECK(data.Size == doctest::Approx(2.5f));
	CHECK(data.Shape == TestDataShape::Box);
	REQUIRE(data.Entries.size() == 1);
	CHECK(data.Entries[0].Name == "A");
	CHECK(data.Entries[0].Weight == 1.0f);
	// The file is in the current version with every field.
	Json const document = fixture.ReadDocument("Data/Crate.stestdata");
	CHECK(document["Strada"]["Version"] == 2);
	CHECK(document["Strada"]["Type"] == "TestData");
	CHECK(document["TestData"].size() == 5);

	struct Refusal
	{
		std::string Path;
		Json Fields;
		std::string Message;
	};
	std::vector<Refusal> const refusals = {
		{"Data/Crate.stestdata", Json::object(), "'Data/Crate.stestdata' already exists"},
		{"Data/Crate.smat", Json::object(), "TestData files end with .stestdata, got 'Data/Crate.smat'"},
		{"Data/Bad.stestdata", Json::object({{"Size", -1.0}}), "TestData.Size: must be at least 0"},
		{"Data/Bad.stestdata", Json::object({{"Entries", Json::array({{{"Name", "A"}}, {{"Name", "A"}}})}}),
	     "TestData: two entries are named 'A'"},
	};
	for (Refusal const& refusal : refusals)
	{
		CAPTURE(refusal.Fields.dump());
		Result<AssetHandle> const refused = fixture.Operations.CreateDataAsset(fixture.Type.GetType(), refusal.Path, refusal.Fields);
		REQUIRE(refused.IsError());
		CHECK(refused.GetError() == refusal.Message);
	}
	Result<AssetHandle> const unknown =
		fixture.Operations.CreateDataAsset(fixture.Type.GetType(), "Data/Bad.stestdata", Json::object({{"Nope", 1}}));
	REQUIRE(unknown.IsError());
	CHECK(unknown.GetError().find("has no field 'Nope'") != std::string::npos);
	CHECK(fixture.Operations.CreateDataAsset(fixture.Type.GetType(), "../Outside.stestdata").IsError());
	CHECK_FALSE(FileSystem::Exists(fixture.GetFile("Data/Bad.stestdata")));
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Game" / "Outside.stestdata"));

	// Only data asset types are created here; materials have CreateMaterial.
	Result<AssetHandle> const material = fixture.Operations.CreateDataAsset(AssetType::Material, "Data/Rock.smat");
	REQUIRE(material.IsError());
	CHECK(material.GetError() == "Material is no data asset type");

	// Files need a project.
	REQUIRE(fixture.Operations.CloseProject().IsOk());
	Result<AssetHandle> const closed = fixture.Operations.CreateDataAsset(fixture.Type.GetType(), "Data/Late.stestdata");
	REQUIRE(closed.IsError());
	CHECK(closed.GetError() == "no project is open");
}

TEST_CASE("SetDataAssetCommand: an edit writes the file and keeps the asset object with a new version; undo and redo write it back")
{
	DataAssetFixture fixture;
	AssetHandle const crate = fixture.Create("Data/Crate.stestdata");
	Ref<TypedDataAsset<TestData>> const asset = DataAssetFixture::Get(crate);
	uint64_t const created = asset->GetVersion();
	std::string const before = fixture.ReadFile("Data/Crate.stestdata");
	size_t const steps = fixture.GetUndoCount();
	CHECK_FALSE(fixture.Context.IsDirty());

	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 4.0}, {"Shape", "Sphere"}})).IsOk());
	CHECK(DataAssetFixture::Get(crate).get() == asset.get());
	CHECK(asset->GetVersion() != created);
	CHECK(asset->GetData().Size == 4.0f);
	CHECK(asset->GetData().Shape == TestDataShape::Sphere);
	CHECK(fixture.ReadDocument("Data/Crate.stestdata")["TestData"]["Size"] == 4.0);
	std::string const after = fixture.ReadFile("Data/Crate.stestdata");
	CommandHistory const& history = fixture.Context.GetHistory();
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoDescription() == "Edit TestData");
	CHECK(history.GetUndoEditedAsset() == crate);
	// An edit of an asset file, not of the scene.
	CHECK_FALSE(fixture.Context.IsDirty());
	// The write was the editor's: the refresh keeps the object.
	CHECK(fixture.Refresh().empty());
	CHECK(DataAssetFixture::Get(crate).get() == asset.get());

	uint64_t const edited = asset->GetVersion();
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile("Data/Crate.stestdata") == before);
	CHECK(asset->GetData() == TestData{});
	CHECK(asset->GetVersion() != edited);
	CHECK(fixture.Refresh().empty());
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(fixture.ReadFile("Data/Crate.stestdata") == after);
	CHECK(asset->GetData().Size == 4.0f);
	CHECK(DataAssetFixture::Get(crate).get() == asset.get());

	// A patch that changes nothing is no step; a refused one changes nothing.
	uint64_t const redone = asset->GetVersion();
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 4.0}})).IsOk());
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(asset->GetVersion() == redone);
	struct Refusal
	{
		Json Patch;
		std::string Message;
	};
	std::vector<Refusal> const refusals = {
		{Json::object({{"Size", -2.0}}), "TestData.Size: must be at least 0"},
		{Json::object({{"Entries", Json::array({{{"Name", "B"}}, {{"Name", "B"}}})}}), "TestData: two entries are named 'B'"},
		{Json::object({{"Entries", Json::array({{{"Name", "B"}, {"Weight", 11.0}}})}}), ""},
		{Json::object({{"Nope", true}}), ""},
		{Json::array({1, 2}), ""},
	};
	for (Refusal const& refusal : refusals)
	{
		CAPTURE(refusal.Patch.dump());
		Result<void> const refused = fixture.Operations.SetDataAssetFields(crate, refusal.Patch);
		REQUIRE(refused.IsError());
		if (!refusal.Message.empty())
		{
			CHECK(refused.GetError() == refusal.Message);
		}
		CHECK(history.GetUndoCount() == steps + 1);
		CHECK(asset->GetVersion() == redone);
		CHECK(fixture.ReadFile("Data/Crate.stestdata") == after);
	}

	// A description names the step.
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Shape", "Box"}}), "Make Box").IsOk());
	CHECK(history.GetUndoDescription() == "Make Box");

	SetDataAssetCommand const command(crate, Json::object({{"Size", 1.0}}));
	CHECK(command.GetEditedAsset() == crate);
	CHECK(command.GetMemoryUse() == 0);
	CHECK_FALSE(command.ModifiesScene());
}

TEST_CASE("SetDataAssetCommand: edits with one merge key are one step named by the last; other keys and assets make steps of their own")
{
	DataAssetFixture fixture;
	AssetHandle const crate = fixture.Create("Data/Crate.stestdata");
	AssetHandle const barrel = fixture.Create("Data/Barrel.stestdata");
	std::string const before = fixture.ReadFile("Data/Crate.stestdata");
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const steps = history.GetUndoCount();
	constexpr uint64_t Key = 0x4441544100000001ull;

	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 2.0}}), "Drag Size", Key).IsOk());
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 3.0}}), "Drag Size Further", Key).IsOk());
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoDescription() == "Drag Size Further");
	// The same key on another asset, and another key on the same asset, start steps.
	REQUIRE(fixture.Operations.SetDataAssetFields(barrel, Json::object({{"Size", 5.0}}), {}, Key).IsOk());
	CHECK(history.GetUndoCount() == steps + 2);
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 6.0}}), {}, Key + 1).IsOk());
	CHECK(history.GetUndoCount() == steps + 3);
	// Without a key, every edit is a step.
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 7.0}})).IsOk());
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 8.0}})).IsOk());
	CHECK(history.GetUndoCount() == steps + 5);

	for (int step = 0; step < 5; step++)
	{
		REQUIRE(fixture.Operations.Undo().IsOk());
	}
	CHECK(fixture.ReadFile("Data/Crate.stestdata") == before);
	CHECK(DataAssetFixture::Get(barrel)->GetData().Size == 1.0f);
}

TEST_CASE("SetDataAssetCommand: read-only files are refused by edits and by undo; data assets in memory and other assets are refused")
{
	DataAssetFixture fixture;
	AssetHandle const crate = fixture.Create("Data/Crate.stestdata");
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 2.0}})).IsOk());
	std::string const bytes = fixture.ReadFile("Data/Crate.stestdata");
	CommandHistory const& history = fixture.Context.GetHistory();
	size_t const steps = history.GetUndoCount();
	{
		Testing::ReadOnlyFileScope const readOnly(fixture.GetFile("Data/Crate.stestdata"));
		CHECK(SetDataAssetCommand::CheckEditable(crate).IsError());
		Result<void> const refused = fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 3.0}}));
		REQUIRE(refused.IsError());
		CHECK(refused.GetError() == "the file of TestData 'Data/Crate.stestdata' is read-only");
		Result<void> const undone = fixture.Operations.Undo();
		REQUIRE(undone.IsError());
		CHECK(undone.GetError().find("is read-only") != std::string::npos);
		CHECK(history.GetUndoCount() == steps);
		CHECK(fixture.ReadFile("Data/Crate.stestdata") == bytes);
		CHECK(DataAssetFixture::Get(crate)->GetData().Size == 2.0f);
	}
	CHECK(SetDataAssetCommand::CheckEditable(crate).IsOk());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(DataAssetFixture::Get(crate)->GetData().Size == 1.0f);

	// A data asset made in memory has no file to keep an edit.
	TypedDataAssetType<TestData> const* const type = DataAssetRegistry::Find<TestData>();
	REQUIRE(type != nullptr);
	AssetHandle const memory = AssetManager::AddMemoryAsset(type->CreateAsset(), "Scratch");
	Result<void> const inMemory = fixture.Operations.SetDataAssetFields(memory, Json::object({{"Size", 3.0}}));
	REQUIRE(inMemory.IsError());
	CHECK(inMemory.GetError() == "TestData 'Scratch' has no file of its own and cannot be edited");

	// Other assets, and handles of nothing.
	Result<AssetHandle> const material = fixture.Operations.CreateMaterial("Materials/Rock.smat");
	REQUIRE(material.IsOk());
	Result<void> const other = fixture.Operations.SetDataAssetFields(material.GetValue(), Json::object({{"Size", 3.0}}));
	REQUIRE(other.IsError());
	CHECK(other.GetError() == "asset 'Rock.smat' is a Material, not a data asset");
	CHECK(fixture.Operations.SetDataAssetFields(AssetHandle(424242), Json::object()).IsError());

	// A missing file.
	REQUIRE(FileSystem::Remove(fixture.GetFile("Data/Crate.stestdata")).IsOk());
	fixture.Refresh();
	Result<void> const missing = fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 3.0}}));
	REQUIRE(missing.IsError());
	CHECK(missing.GetError() == "asset file 'Data/Crate.stestdata' is missing");
}

TEST_CASE("SetDataAssetCommand: a step follows its moved file, and undoing the first edit of a version-1 file writes the current version")
{
	DataAssetFixture fixture;
	std::filesystem::path const fixtureFile =
		FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "DataAssets" / "TestData" / "Version1.stestdata";
	Result<std::string> const version1 = FileSystem::ReadTextFile(fixtureFile);
	REQUIRE(version1.IsOk());
	REQUIRE(FileSystem::CreateDirectories(fixture.GetFile("Data")).IsOk());
	REQUIRE(FileSystem::WriteTextFile(fixture.GetFile("Data/Old.stestdata"), version1.GetValue()).IsOk());
	fixture.Refresh();
	AssetHandle const old = AssetManager::FindByPath("Data/Old.stestdata");
	REQUIRE(old.IsValid());
	Json const fields = DataAssetFixture::Get(old)->SerializeFields();

	REQUIRE(fixture.Operations.SetDataAssetFields(old, Json::object({{"Shape", "Box"}, {"Size", 9.0}})).IsOk());
	REQUIRE(fixture.Operations.MoveAsset(old, "Moved/Old.stestdata").IsOk());
	REQUIRE(fixture.Operations.Undo().IsOk());
	// The values of the version-1 file, in the current version.
	Json const undone = fixture.ReadDocument("Moved/Old.stestdata");
	CHECK(undone["Strada"]["Version"] == 2);
	CHECK(undone["TestData"] == fields);
	CHECK_FALSE(undone["TestData"].contains("Radius"));
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(fixture.ReadDocument("Moved/Old.stestdata")["TestData"]["Size"] == 9.0);
}

TEST_CASE("SetDataAssetCommand: after an edit, a change written outside the editor is found by the refresh and loads another object")
{
	DataAssetFixture fixture;
	AssetHandle const crate = fixture.Create("Data/Crate.stestdata");
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Size", 2.0}})).IsOk());
	Ref<TypedDataAsset<TestData>> const edited = DataAssetFixture::Get(crate);
	uint64_t const epoch = DataAssetRegistry::GetEpoch();

	Json document = fixture.ReadDocument("Data/Crate.stestdata");
	document["TestData"]["Size"] = 6.0;
	REQUIRE(FileSystem::WriteTextFile(fixture.GetFile("Data/Crate.stestdata"), DumpJson(document)).IsOk());
	MoveWriteTime(fixture.GetFile("Data/Crate.stestdata"));
	CHECK(fixture.Refresh() == std::vector<AssetHandle>{crate});
	CHECK(DataAssetRegistry::GetEpoch() != epoch);
	Ref<TypedDataAsset<TestData>> const reloaded = DataAssetFixture::Get(crate);
	CHECK(reloaded.get() != edited.get());
	CHECK(reloaded->GetVersion() != edited->GetVersion());
	CHECK(reloaded->GetData().Size == 6.0f);

	// The next edit starts from what the file holds now, and its undo gives that back.
	REQUIRE(fixture.Operations.SetDataAssetFields(crate, Json::object({{"Shape", "Sphere"}})).IsOk());
	CHECK(reloaded->GetData().Size == 6.0f);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadDocument("Data/Crate.stestdata")["TestData"] == document["TestData"]);
}

TEST_CASE("PlayMode: data asset edits made while playing reach the game at once and stay undoable after Stop")
{
	Testing::TestDataTypeScope const type;
	Testing::AssetManagerScope const assets;
	Testing::TemporaryDirectory directory;
	EditorContext context;
	EditorOperations operations(context);
	EditorScripts scripts(context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll");
	PlayMode play(context, scripts);
	REQUIRE(operations.CreateProject(directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	Result<AssetHandle> const created = operations.CreateDataAsset(type.GetType(), "Data/Crate.stestdata");
	REQUIRE(created.IsOk());
	AssetHandle const crate = created.GetValue();
	Ref<TypedDataAsset<TestData>> const asset = AssetManager::GetAsset<TypedDataAsset<TestData>>(crate);
	REQUIRE(asset != nullptr);
	size_t const steps = context.GetHistory().GetUndoCount();

	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	REQUIRE(operations.SetDataAssetFields(crate, Json::object({{"Size", 3.0}})).IsOk());
	// The game's users of the asset see it through the asset object they look up, which stays.
	CHECK(AssetManager::GetAsset<TypedDataAsset<TestData>>(crate).get() == asset.get());
	CHECK(asset->GetData().Size == 3.0f);
	play.Stop();

	// Stopping restores the scene, not the asset's file: the edit stays, with its step.
	CHECK(asset->GetData().Size == 3.0f);
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(context.GetHistory().GetUndoDescription() == "Edit TestData");
	CHECK_FALSE(context.IsDirty());
	REQUIRE(operations.Undo().IsOk());
	CHECK(asset->GetData().Size == 1.0f);
}
