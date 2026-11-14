#include "Asset/TestDataAsset.h"
#include "EditorLocationsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace Strada;
using Strada::Testing::TestData;

namespace
{
	// A data asset type registered for a moment: its commands outlive it.
	struct PassingData
	{
		float Value = 0.0f;
	};

	// A type named after a built-in command domain.
	struct GraphData
	{
		float Value = 0.0f;
	};
}

template<>
struct Strada::StructTraits<PassingData>
{
	static constexpr std::string_view Name = "PassingData";
	static constexpr auto Fields = std::make_tuple(Field("Value", &PassingData::Value).Doc("A value."));
};

template<>
struct Strada::StructTraits<GraphData>
{
	static constexpr std::string_view Name = "GraphData";
	static constexpr auto Fields = std::make_tuple(Field("Value", &GraphData::Value).Doc("A value."));
};

namespace
{
	// An editor model with every command of the editor registered after the tests' data asset type.
	struct DataAssetCommandFixture
	{
		// Declared first: it unregisters after the asset directory closed.
		Testing::TestDataTypeScope Type;
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;

		explicit DataAssetCommandFixture(bool withProject = true)
		{
			EditorCommandEnvironment environment;
			environment.Locations = Testing::MakeEditorLocations(Directory.GetPath());
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			if (withProject)
			{
				REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			}
		}

		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		CommandError Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.GetError();
		}

		std::filesystem::path GetFile(std::string_view path) const
		{
			return Context.GetProject()->GetAssetDirectory() / FileSystem::PathFromUtf8(path);
		}

		std::string ReadFile(std::string_view path) const
		{
			Result<std::string> text = FileSystem::ReadTextFile(GetFile(path));
			REQUIRE(text.IsOk());
			return text.TakeValue();
		}
	};
}

TEST_CASE("DataAssetCommands: a registered type gets <domain>.create, get and set, with its fields in their schemas, documented")
{
	DataAssetCommandFixture fixture;
	CommandDefinition const* create = fixture.Registry.Find("test-data.create");
	CommandDefinition const* get = fixture.Registry.Find("test-data.get");
	CommandDefinition const* set = fixture.Registry.Find("test-data.set");
	REQUIRE(create != nullptr);
	REQUIRE(get != nullptr);
	REQUIRE(set != nullptr);
	CHECK_FALSE(create->ReadOnly);
	CHECK(get->ReadOnly);
	CHECK_FALSE(set->ReadOnly);
	CHECK(create->Description.find(".stestdata") != std::string::npos);
	CHECK(create->Description.find("Not undoable") != std::string::npos);
	CHECK(set->Description.find("Undoable") != std::string::npos);
	CHECK(get->Parameters["required"] == Json::array({"testData"}));
	CHECK(set->Parameters["required"] == Json::array({"testData", "fields"}));

	// The fields' schema names every field with its type, range and description, and leaves the values to the field table.
	for (CommandDefinition const* command : {create, set})
	{
		Json const& fields = command->Parameters["properties"]["fields"];
		CHECK(fields["additionalProperties"] == true);
		REQUIRE(fields["properties"].size() == 5);
		CHECK(fields["properties"]["Size"]["description"] == "float, at least 0: Edge length of a box, diameter of a sphere.");
		CHECK(fields["properties"]["Shape"]["description"].get<std::string>().ends_with(": The shape the size measures."));
		CHECK(fields["properties"]["Next"]["description"].get<std::string>().find("a reference to a TestData asset") != std::string::npos);
		CHECK(fields["properties"]["Entries"]["description"].get<std::string>().find("at most 16 elements") != std::string::npos);
		CHECK_FALSE(fields["properties"]["Size"].contains("type"));
	}
	// asset.list lists assets of the type by its name.
	Json const& types = fixture.Registry.Find("asset.list")->Parameters["properties"]["type"]["enum"];
	CHECK(std::find(types.begin(), types.end(), Json("TestData")) != types.end());

	// The reference documents the type's commands as the pattern's example.
	Result<std::string> const reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	for (char const* name : {"test-data.create", "test-data.get", "test-data.set"})
	{
		CAPTURE(name);
		CHECK(reference.GetValue().find(std::string("`") + name + "`") != std::string::npos);
	}
}

TEST_CASE("DataAssetCommands: assets are created, read and edited, and set is a step that editor.undo and editor.redo take")
{
	DataAssetCommandFixture fixture;
	Json const created = fixture.Ok(
		"test-data.create", Json::object({{"path", "Data/Crate.stestdata"}, {"fields", {{"Size", 2.0}, {"Entries", {{{"Name", "A"}}}}}}}));
	CHECK(created["testData"]["type"] == "TestData");
	CHECK(created["testData"]["path"] == "Data/Crate.stestdata");
	CHECK(created["testData"]["reference"] == "asset://Data/Crate.stestdata");
	CHECK(created["fields"]["Size"] == 2.0);
	CHECK(created["fields"]["Shape"] == "Box");
	CHECK(created["fields"]["Entries"] == Json::array({{{"Name", "A"}, {"Weight", 1.0}}}));
	std::string const id = created["testData"]["id"].get<std::string>();
	// Creating a file is not a step.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 0);

	CHECK(fixture.Ok("test-data.get", Json::object({{"testData", id}})) == created);
	CHECK(fixture.Ok("test-data.get", Json::object({{"testData", "asset://Data/Crate.stestdata"}})) == created);

	AssetHandle const crate = AssetManager::FindByPath("Data/Crate.stestdata");
	Ref<TypedDataAsset<TestData>> const asset = AssetManager::GetAsset<TypedDataAsset<TestData>>(crate);
	REQUIRE(asset != nullptr);
	uint64_t const version = asset->GetVersion();
	std::string const before = fixture.ReadFile("Data/Crate.stestdata");
	Json const set = fixture.Ok("test-data.set", Json::object({{"testData", id}, {"fields", {{"Shape", "Sphere"}, {"Size", 3.0}}}}));
	CHECK(set["fields"]["Shape"] == "Sphere");
	CHECK(set["fields"]["Size"] == 3.0);
	CHECK(set["fields"]["Entries"] == created["fields"]["Entries"]);
	std::string const after = fixture.ReadFile("Data/Crate.stestdata");
	Json const status = fixture.Ok("editor.status");
	CHECK(status["undo"] == "Edit TestData");
	CHECK(status["scene"]["dirty"] == false);
	// Hot reload: the asset object stays and its version moves; the refresh does not take the editor's write for a change.
	CHECK(AssetManager::GetAsset<TypedDataAsset<TestData>>(crate).get() == asset.get());
	CHECK(asset->GetVersion() != version);
	CHECK(fixture.Ok("asset.refresh")["modified"].empty());
	CHECK(asset->GetData().Size == 3.0f);

	CHECK(fixture.Ok("editor.undo")["undone"] == "Edit TestData");
	CHECK(fixture.ReadFile("Data/Crate.stestdata") == before);
	CHECK(fixture.Ok("test-data.get", Json::object({{"testData", id}}))["fields"] == created["fields"]);
	CHECK(asset->GetData().Size == 2.0f);
	CHECK(fixture.Ok("editor.redo")["redone"] == "Edit TestData");
	CHECK(fixture.ReadFile("Data/Crate.stestdata") == after);
	CHECK(fixture.Ok("test-data.get", Json::object({{"testData", id}}))["fields"] == set["fields"]);
	CHECK(AssetManager::GetAsset<TypedDataAsset<TestData>>(crate).get() == asset.get());
}

TEST_CASE("DataAssetCommands: values the type refuses, other assets, files that cannot change and a missing project are errors")
{
	DataAssetCommandFixture fixture;
	std::string const id =
		fixture.Ok("test-data.create", Json::object({{"path", "Data/Crate.stestdata"}}))["testData"]["id"].get<std::string>();
	std::string const bytes = fixture.ReadFile("Data/Crate.stestdata");

	// The field table's messages reach the caller as InvalidParams, and nothing is written.
	struct Refusal
	{
		Json Fields;
		std::string Message;
	};
	for (Refusal const& refusal :
	     {Refusal{Json::object({{"Size", -1.0}}), "TestData.Size: must be at least 0"},
	      Refusal{Json::object({{"Nope", 1}}), "has no field 'Nope'"},
	      Refusal{Json::object({{"Entries", Json::array({{{"Name", "A"}}, {{"Name", "A"}}})}}), "TestData: two entries are named 'A'"}})
	{
		CAPTURE(refusal.Fields.dump());
		CommandError const created =
			fixture.Fails("test-data.create", Json::object({{"path", "Data/Bad.stestdata"}, {"fields", refusal.Fields}}));
		CHECK(created.Code == AutomationErrorCode::InvalidParams);
		CHECK(created.Message.find(refusal.Message) != std::string::npos);
		CommandError const set = fixture.Fails("test-data.set", Json::object({{"testData", id}, {"fields", refusal.Fields}}));
		CHECK(set.Code == AutomationErrorCode::InvalidParams);
		CHECK(set.Message.find(refusal.Message) != std::string::npos);
	}
	CHECK_FALSE(FileSystem::Exists(fixture.GetFile("Data/Bad.stestdata")));
	CHECK(fixture.ReadFile("Data/Crate.stestdata") == bytes);
	CHECK(fixture.Fails("test-data.create", Json::object({{"path", "Data/Crate.stestdata"}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("test-data.create", Json::object({{"path", "Data/Crate.smat"}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("test-data.create", Json::object({{"path", "../Escape.stestdata"}})).Code == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("test-data.set", Json::object({{"testData", id}})).Code == AutomationErrorCode::InvalidParams);

	// Assets that are not there, and assets of other types.
	CommandError const missing = fixture.Fails("test-data.get", Json::object({{"testData", "asset://Nope.stestdata"}}));
	CHECK(missing.Code == AutomationErrorCode::AssetNotFound);
	CHECK(missing.Data["path"] == "testData");
	fixture.Ok("material.create", Json::object({{"path", "Materials/Rock.smat"}}));
	CommandError const material = fixture.Fails("test-data.get", Json::object({{"testData", "asset://Materials/Rock.smat"}}));
	CHECK(material.Code == AutomationErrorCode::InvalidParams);
	CHECK(material.Message == "testData: asset 'asset://Materials/Rock.smat' is a Material, not a TestData");
	CHECK(fixture.Fails("test-data.set", Json::object({{"testData", "asset://Materials/Rock.smat"}, {"fields", Json::object()}})).Code ==
	      AutomationErrorCode::InvalidParams);

	// Data assets in memory and read-only files cannot be edited.
	TypedDataAssetType<TestData> const* const type = DataAssetRegistry::Find<TestData>();
	REQUIRE(type != nullptr);
	AssetHandle const memory = AssetManager::AddMemoryAsset(type->CreateAsset(), "Scratch");
	CHECK(fixture.Ok("test-data.get", Json::object({{"testData", memory.ToString()}}))["fields"]["Size"] == 1.0);
	CHECK(fixture.Fails("test-data.set", Json::object({{"testData", memory.ToString()}, {"fields", {{"Size", 2.0}}}})).Code ==
	      AutomationErrorCode::InvalidOperation);
	{
		Testing::ReadOnlyFileScope const readOnly(fixture.GetFile("Data/Crate.stestdata"));
		CommandError const readOnlyError = fixture.Fails("test-data.set", Json::object({{"testData", id}, {"fields", {{"Size", 2.0}}}}));
		CHECK(readOnlyError.Code == AutomationErrorCode::InvalidOperation);
		CHECK(readOnlyError.Message == "the file of TestData 'Data/Crate.stestdata' is read-only");
	}
	// A file that does not load.
	REQUIRE(FileSystem::WriteTextFile(fixture.GetFile("Data/Crate.stestdata"), "{ damaged").IsOk());
	AssetManager::UnloadAsset(AssetManager::FindByPath("Data/Crate.stestdata"));
	CHECK(fixture.Fails("test-data.get", Json::object({{"testData", id}})).Code == AutomationErrorCode::FileError);
	CHECK(fixture.Fails("test-data.set", Json::object({{"testData", id}, {"fields", {{"Size", 2.0}}}})).Code ==
	      AutomationErrorCode::FileError);
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 0);

	// Files need a project.
	REQUIRE(fixture.Operations.CloseProject().IsOk());
	CHECK(fixture.Fails("test-data.create", Json::object({{"path", "Data/Late.stestdata"}})).Code == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("test-data.set", Json::object({{"testData", id}, {"fields", Json::object()}})).Code ==
	      AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("test-data.get", Json::object({{"testData", id}})).Code == AutomationErrorCode::AssetNotFound);
}

TEST_CASE("DataAssetCommands: without assets the commands are unavailable, and a type unregistered since answers so")
{
	Testing::TestDataTypeScope const type;
	Testing::TemporaryDirectory directory;
	EditorContext context;
	EditorOperations operations(context);
	CommandRegistry registry;
	DataAssetTypeSpecification<PassingData> specification;
	specification.Name = "PassingData";
	specification.Extension = ".spassing";
	specification.Description = "A type registered for a moment.";
	Result<AssetType> const passing = DataAssetRegistry::Register(specification);
	REQUIRE(passing.IsOk());
	EditorCommandEnvironment environment;
	environment.Locations = Testing::MakeEditorLocations(directory.GetPath());
	REQUIRE(RegisterEditorCommands(registry, operations, std::move(environment)).IsOk());
	REQUIRE(DataAssetRegistry::Unregister(passing.GetValue()).IsOk());
	REQUIRE(registry.Contains("passing-data.get"));

	auto const run = [&registry](std::string_view name, Json params)
	{
		std::optional<CommandResult> result;
		registry.Execute(name, params,
		                 [&result](CommandResult value)
		                 {
							 result.emplace(std::move(value));
						 });
		REQUIRE(result.has_value());
		REQUIRE(result->IsError());
		return result->GetError();
	};
	for (char const* const name : {"test-data.create", "test-data.get", "test-data.set"})
	{
		CAPTURE(name);
		Json const params = Json::object({{"path", "Data/Crate.stestdata"}, {"testData", "1"}, {"fields", Json::object()}});
		Json accepted = Json::object();
		for (auto const& [key, value] : params.items())
		{
			if (registry.Find(name)->Parameters["properties"].contains(key))
			{
				accepted[key] = value;
			}
		}
		CHECK(run(name, accepted).Code == AutomationErrorCode::Unavailable);
	}
	Testing::AssetManagerScope const assets;
	CommandError const gone = run("passing-data.get", Json::object({{"passingData", "1"}}));
	CHECK(gone.Code == AutomationErrorCode::Unavailable);
	CHECK(gone.Message == "the data asset type 'PassingData' is no longer registered");
}

TEST_CASE("DataAssetCommands: a type whose commands would repeat a built-in domain's makes registering the editor's commands fail")
{
	DataAssetTypeSpecification<GraphData> specification;
	specification.Name = "Graph";
	specification.Extension = ".sgraphdata";
	specification.Description = "A type whose domain is the visual scripts' own.";
	Result<AssetType> const graph = DataAssetRegistry::Register(specification);
	REQUIRE(graph.IsOk());
	Testing::TemporaryDirectory directory;
	EditorContext context;
	EditorOperations operations(context);
	CommandRegistry registry;
	EditorCommandEnvironment environment;
	environment.Locations = Testing::MakeEditorLocations(directory.GetPath());
	Result<void> const registered = RegisterEditorCommands(registry, operations, std::move(environment));
	REQUIRE(DataAssetRegistry::Unregister(graph.GetValue()).IsOk());
	REQUIRE(registered.IsError());
	CHECK(registered.GetError() == "command 'graph.create' is already registered");
}

TEST_CASE("DataAssetCommands: the engine's Curve has curve.create, get and set, documented, and a set of keys out of order is refused")
{
	DataAssetCommandFixture fixture;
	CommandDefinition const* create = fixture.Registry.Find("curve.create");
	REQUIRE(create != nullptr);
	CHECK(create->Description.find(".scurve") != std::string::npos);
	REQUIRE(fixture.Registry.Find("curve.set") != nullptr);
	CHECK(fixture.Registry.Find("curve.get")->Parameters["required"] == Json::array({"curve"}));
	CHECK(create->Parameters["properties"]["fields"]["properties"]["Keys"]["description"].get<std::string>().find(
			  "at most 1024 elements") != std::string::npos);
	Json const& types = fixture.Registry.Find("asset.list")->Parameters["properties"]["type"]["enum"];
	CHECK(std::find(types.begin(), types.end(), Json("Curve")) != types.end());
	Result<std::string> const reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	for (char const* name : {"curve.create", "curve.get", "curve.set"})
	{
		CAPTURE(name);
		CHECK(reference.GetValue().find(std::string("`") + name + "`") != std::string::npos);
	}

	Json const keys = Json::parse(R"([{ "Time": 0, "Value": 0, "Interpolation": "Linear" }, { "Time": 2, "Value": 4 }])");
	Json const created = fixture.Ok("curve.create", Json::object({{"path", "Curves/Ramp.scurve"}, {"fields", {{"Keys", keys}}}}));
	CHECK(created["curve"]["type"] == "Curve");
	CHECK(created["fields"]["PostExtrapolation"] == "Hold");
	CHECK(
		created["fields"]["Keys"][1] ==
		Json::parse(R"({ "Time": 2.0, "Value": 4.0, "Interpolation": "Cubic", "HasIn": false, "In": 0.0, "HasOut": false, "Out": 0.0 })"));
	std::string const id = created["curve"]["id"].get<std::string>();
	AssetHandle const handle = AssetManager::FindByPath("Curves/Ramp.scurve");
	Ref<TypedDataAsset<Curve>> const curve = AssetManager::GetAsset<TypedDataAsset<Curve>>(handle);
	REQUIRE(curve != nullptr);
	CHECK(curve->GetData().Evaluate(1.0) == 2.0f);

	// One step, undone and redone, which the next lookup's evaluation follows.
	std::string const before = fixture.ReadFile("Curves/Ramp.scurve");
	fixture.Ok("curve.set", Json::object({{"curve", id}, {"fields", {{"PostExtrapolation", "Linear"}}}}));
	CHECK(fixture.Ok("editor.status")["undo"] == "Edit Curve");
	CHECK(curve->GetData().Evaluate(3.0) == 6.0f);
	CHECK(fixture.Ok("editor.undo")["undone"] == "Edit Curve");
	CHECK(fixture.ReadFile("Curves/Ramp.scurve") == before);
	CHECK(curve->GetData().Evaluate(3.0) == 4.0f);
	CHECK(fixture.Ok("editor.redo")["redone"] == "Edit Curve");
	CHECK(curve->GetData().Evaluate(3.0) == 6.0f);

	// Keys out of order are the curve's own rule (Validate), refused with InvalidParams; nothing is written.
	std::string const edited = fixture.ReadFile("Curves/Ramp.scurve");
	CommandError const unordered = fixture.Fails(
		"curve.set",
		Json::object({{"curve", id}, {"fields", {{"Keys", Json::parse(R"([{ "Time": 1, "Value": 0 }, { "Time": 0, "Value": 1 }])")}}}}));
	CHECK(unordered.Code == AutomationErrorCode::InvalidParams);
	CHECK(unordered.Message.find("Keys[1].Time: keys are in time order: 0 comes after 1") != std::string::npos);
	CHECK(fixture.ReadFile("Curves/Ramp.scurve") == edited);
}
