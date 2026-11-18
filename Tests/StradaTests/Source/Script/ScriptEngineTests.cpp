#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Script/DotNetHost.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	ScriptClassInfo const& RequireClass(std::string_view name)
	{
		ScriptClassInfo const* scriptClass = ScriptEngine::FindClass(name);
		REQUIRE_MESSAGE(scriptClass != nullptr, std::string(name));
		return *scriptClass;
	}

	ScriptFieldInfo const& RequireField(ScriptClassInfo const& scriptClass, std::string_view name)
	{
		ScriptFieldInfo const* field = scriptClass.FindField(name);
		REQUIRE_MESSAGE(field != nullptr, std::string(name));
		return *field;
	}

	std::vector<std::string> GetFieldNames(ScriptClassInfo const& scriptClass)
	{
		std::vector<std::string> names;
		for (ScriptFieldInfo const& field : scriptClass.Fields)
		{
			names.push_back(field.Name);
		}
		return names;
	}
}

TEST_CASE("ScriptEngine: loads game scripts and describes their classes")
{
	uint64_t const logStart = Log::GetNextEntryIndex();
	Testing::ScriptEngineScope scripting;
	CHECK(ScriptEngine::HasGameAssembly());
	CHECK(ScriptEngine::GetGameAssemblyPath().filename() == "Strada.TestScripts.dll");

	// Concrete, non-generic Script classes that can be created; sorted by name.
	std::vector<std::string> names;
	for (ScriptClassInfo const& scriptClass : ScriptEngine::GetClasses())
	{
		names.push_back(scriptClass.Name);
	}
	CHECK(names ==
	      std::vector<std::string>{"Strada.Tests.ActionProbe",     "Strada.Tests.AssetProbe",       "Strada.Tests.AudioProbe",
	                               "Strada.Tests.BaseBehaviour",   "Strada.Tests.CameraProbe",      "Strada.Tests.CharacterProbe",
	                               "Strada.Tests.ClickRecorder",   "Strada.Tests.ComponentWriter",  "Strada.Tests.ContactReader",
	                               "Strada.Tests.CurveProbe",      "Strada.Tests.DerivedBehaviour", "Strada.Tests.FieldLists",
	                               "Strada.Tests.FieldReporter",   "Strada.Tests.FieldTypes",       "Strada.Tests.FirstPersonController",
	                               "Strada.Tests.FixedStepMover",  "Strada.Tests.HierarchyProbe",   "Strada.Tests.HostProbe",
	                               "Strada.Tests.InputFrameProbe", "Strada.Tests.InputProbe",       "Strada.Tests.Lifecycle",
	                               "Strada.Tests.Mover",           "Strada.Tests.ParentMover",      "Strada.Tests.PhysicsMaterialProbe",
	                               "Strada.Tests.PhysicsProbe",    "Strada.Tests.PrefabMember",     "Strada.Tests.PrefabProbe",
	                               "Strada.Tests.QueryProbe",      "Strada.Tests.QueryReader",      "Strada.Tests.SceneSwitcher",
	                               "Strada.Tests.SelfTest",        "Strada.Tests.Spawner",          "Strada.Tests.Spinner",
	                               "Strada.Tests.StepProbe",       "Strada.Tests.Thrower",          "Strada.Tests.TransformProbe",
	                               "Strada.Tests.UIWriter",        "Strada.Tests.ValueGuard",       "Strada.Tests.VisualScriptProbe"});
	CHECK(ScriptEngine::FindClass("Strada.Tests.NotAScript") == nullptr);
	CHECK(ScriptEngine::FindClass("Strada.Tests.Missing") == nullptr);
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.NoDefaultConstructor is ignored"));
	CHECK(Testing::WasLogged(logStart, "ThrowingConstructor cannot be created"));

	ScriptClassInfo const& mover = RequireClass("Strada.Tests.Mover");
	CHECK(GetFieldNames(mover) == std::vector<std::string>{"Speed", "Direction"});
	CHECK(RequireField(mover, "Speed").DefaultValue == ScriptFieldValue::FromFloat(1.0f));
	CHECK(RequireField(mover, "Direction").DefaultValue == ScriptFieldValue::FromVector3({1.0f, 0.0f, 0.0f}));

	// Base classes' serialized fields come first.
	ScriptClassInfo const& derived = RequireClass("Strada.Tests.DerivedBehaviour");
	CHECK(GetFieldNames(derived) == std::vector<std::string>{"BaseValue", "Extra"});
	CHECK(RequireField(derived, "BaseValue").DefaultValue == ScriptFieldValue::FromFloat(1.0f));
}

TEST_CASE("ScriptEngine: serialized fields have the types, defaults and attributes of the script")
{
	Testing::ScriptEngineScope scripting;
	ScriptClassInfo const& fields = RequireClass("Strada.Tests.FieldTypes");
	// Public and [SerializeField] fields only: not private, static, readonly or of unsupported types.
	CHECK(GetFieldNames(fields) == std::vector<std::string>{"Flag",  "Count", "Mask",   "Big",     "Huge",  "Speed",  "Precise",
	                                                        "Title", "Size",  "Offset", "Weights", "Turn",  "Tint",   "Target",
	                                                        "Model", "Shape", "Level",  "Access",  "Small", "Hidden", "m_Secret"});

	CHECK(RequireField(fields, "Flag").DefaultValue == ScriptFieldValue::FromBool(true));
	CHECK(RequireField(fields, "Count").DefaultValue == ScriptFieldValue::FromInt32(-3));
	CHECK(RequireField(fields, "Mask").DefaultValue == ScriptFieldValue::FromUInt32(7));
	CHECK(RequireField(fields, "Big").DefaultValue == ScriptFieldValue::FromInt64(-9000000000));
	CHECK(RequireField(fields, "Huge").DefaultValue == ScriptFieldValue::FromUInt64(18000000000000000000ULL));
	CHECK(RequireField(fields, "Precise").DefaultValue == ScriptFieldValue::FromDouble(0.125));
	CHECK(RequireField(fields, "Title").DefaultValue == ScriptFieldValue::FromString("Hello"));
	CHECK(RequireField(fields, "Size").DefaultValue == ScriptFieldValue::FromVector2({1.0f, 2.0f}));
	CHECK(RequireField(fields, "Weights").DefaultValue == ScriptFieldValue::FromVector4({1.0f, 2.0f, 3.0f, 4.0f}));
	CHECK(RequireField(fields, "Turn").DefaultValue == ScriptFieldValue::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
	CHECK(RequireField(fields, "Tint").DefaultValue == ScriptFieldValue::FromColor({0.5f, 0.25f, 1.0f, 1.0f}));
	CHECK(RequireField(fields, "Target").DefaultValue == ScriptFieldValue::FromEntity(UUID::Invalid()));
	CHECK(RequireField(fields, "Model").DefaultValue == ScriptFieldValue::FromAsset(AssetHandle()));
	CHECK(RequireField(fields, "m_Secret").DefaultValue == ScriptFieldValue::FromInt32(42));

	ScriptFieldInfo const& speed = RequireField(fields, "Speed");
	CHECK(speed.DefaultValue == ScriptFieldValue::FromFloat(2.5f));
	CHECK(speed.Tooltip == "Units per second");
	REQUIRE(speed.Range.has_value());
	CHECK(*speed.Range == glm::vec2(0.0f, 10.0f));
	CHECK_FALSE(speed.Hidden);
	CHECK(RequireField(fields, "Hidden").Hidden);
	CHECK_FALSE(RequireField(fields, "Count").Range.has_value());

	// Typed asset references accept one asset type; AssetHandle fields accept any.
	ScriptFieldInfo const& shape = RequireField(fields, "Shape");
	CHECK(shape.Type == ScriptFieldType::Asset);
	CHECK(shape.AcceptedAssetType == AssetType::Mesh);
	CHECK(shape.DefaultValue == ScriptFieldValue::FromAsset(AssetHandle()));
	CHECK(RequireField(fields, "Model").AcceptedAssetType == AssetType::None);

	// Enums are stored as integers of their size and list their enumerators in declaration order.
	ScriptFieldInfo const& level = RequireField(fields, "Level");
	CHECK(level.Type == ScriptFieldType::Int32);
	CHECK(level.DefaultValue == ScriptFieldValue::FromInt32(5));
	REQUIRE(level.Enumerators.size() == 3);
	CHECK(level.Enumerators[0].Name == "Easy");
	CHECK(level.Enumerators[0].Value == ScriptFieldValue::FromInt32(0));
	CHECK(level.Enumerators[2].Name == "Hard");
	CHECK(level.Enumerators[2].Value == ScriptFieldValue::FromInt32(5));
	CHECK_FALSE(level.IsFlags);
	ScriptFieldInfo const& access = RequireField(fields, "Access");
	CHECK(access.Type == ScriptFieldType::UInt32);
	CHECK(access.IsFlags);
	CHECK(access.DefaultValue == ScriptFieldValue::FromUInt32(3));
	CHECK(access.Enumerators.size() == 4);
	ScriptFieldInfo const& small = RequireField(fields, "Small");
	CHECK(small.Type == ScriptFieldType::Int32);
	CHECK(small.DefaultValue == ScriptFieldValue::FromInt32(200));
	CHECK(RequireField(fields, "Count").Enumerators.empty());
}

TEST_CASE("ScriptEngine: C# arrays and lists of every element type are list fields, and fields of other types are reported once")
{
	uint64_t const logStart = Log::GetNextEntryIndex();
	Testing::ScriptEngineScope scripting;
	ScriptClassInfo const& lists = RequireClass("Strada.Tests.FieldLists");
	// T[] and List<T> of every element type, in declaration order; not the fields of types no field type stores.
	CHECK(GetFieldNames(lists) ==
	      std::vector<std::string>{
			  "Flags",     "Counts",    "Masks",      "Bigs",       "Huges",    "Speeds",    "Precises",   "Titles",    "Sizes",
			  "Offsets",   "Weights",   "Turns",      "Tints",      "Targets",  "Crates",    "Models",     "Shapes",    "Levels",
			  "Accesses",  "Smalls",    "FlagList",   "CountList",  "MaskList", "BigList",   "HugeList",   "SpeedList", "PreciseList",
			  "TitleList", "SizeList",  "OffsetList", "WeightList", "TurnList", "TintList",  "TargetList", "CrateList", "ModelList",
			  "ShapeList", "LevelList", "AccessList", "SmallList",  "Missing",  "UnsetTurn", "UnsetTurns"});

	// Each array and its list have the list type of their element's field type.
	struct Row
	{
		std::string_view Array;
		std::string_view List;
		ScriptFieldType ElementType;
	};
	constexpr std::array<Row, 20> Rows = {{
		{"Flags", "FlagList", ScriptFieldType::Bool},         {"Counts", "CountList", ScriptFieldType::Int32},
		{"Masks", "MaskList", ScriptFieldType::UInt32},       {"Bigs", "BigList", ScriptFieldType::Int64},
		{"Huges", "HugeList", ScriptFieldType::UInt64},       {"Speeds", "SpeedList", ScriptFieldType::Float},
		{"Precises", "PreciseList", ScriptFieldType::Double}, {"Titles", "TitleList", ScriptFieldType::String},
		{"Sizes", "SizeList", ScriptFieldType::Vector2},      {"Offsets", "OffsetList", ScriptFieldType::Vector3},
		{"Weights", "WeightList", ScriptFieldType::Vector4},  {"Turns", "TurnList", ScriptFieldType::Quaternion},
		{"Tints", "TintList", ScriptFieldType::Color},        {"Targets", "TargetList", ScriptFieldType::Entity},
		{"Crates", "CrateList", ScriptFieldType::Prefab},     {"Models", "ModelList", ScriptFieldType::Asset},
		{"Shapes", "ShapeList", ScriptFieldType::Asset},      {"Levels", "LevelList", ScriptFieldType::Int32},
		{"Accesses", "AccessList", ScriptFieldType::UInt32},  {"Smalls", "SmallList", ScriptFieldType::Int32},
	}};
	for (Row const& row : Rows)
	{
		CAPTURE(row.Array);
		ScriptFieldType const listType = MakeScriptFieldListType(row.ElementType);
		CHECK(RequireField(lists, row.Array).Type == listType);
		CHECK(RequireField(lists, row.List).Type == listType);
		CHECK(RequireField(lists, row.Array).DefaultValue.GetType() == listType);
	}

	// Defaults read from a default instance, each element of its type; a null array is the empty list.
	auto const list = [](ScriptFieldType elementType, std::vector<ScriptFieldValue> elements)
	{
		return ScriptFieldValue::FromList(elementType, std::move(elements));
	};
	CHECK(RequireField(lists, "Flags").DefaultValue ==
	      list(ScriptFieldType::Bool, {ScriptFieldValue::FromBool(true), ScriptFieldValue::FromBool(false)}));
	CHECK(RequireField(lists, "Counts").DefaultValue ==
	      list(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(1), ScriptFieldValue::FromInt32(-2), ScriptFieldValue::FromInt32(3)}));
	CHECK(RequireField(lists, "Bigs").DefaultValue == list(ScriptFieldType::Int64, {ScriptFieldValue::FromInt64(-9000000000)}));
	CHECK(RequireField(lists, "Huges").DefaultValue ==
	      list(ScriptFieldType::UInt64, {ScriptFieldValue::FromUInt64(18000000000000000000ULL)}));
	CHECK(RequireField(lists, "Titles").DefaultValue ==
	      list(ScriptFieldType::String, {ScriptFieldValue::FromString("Hello"), ScriptFieldValue::FromString("")}));
	CHECK(RequireField(lists, "Turns").DefaultValue ==
	      list(ScriptFieldType::Quaternion, {ScriptFieldValue::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f))}));
	CHECK(RequireField(lists, "Tints").DefaultValue ==
	      list(ScriptFieldType::Color, {ScriptFieldValue::FromColor({0.5f, 0.25f, 1.0f, 1.0f})}));
	CHECK(RequireField(lists, "Targets").DefaultValue == list(ScriptFieldType::Entity, {ScriptFieldValue::FromEntity(UUID::Invalid())}));
	CHECK(RequireField(lists, "Offsets").DefaultValue == list(ScriptFieldType::Vector3, {}));
	CHECK(RequireField(lists, "OffsetList").DefaultValue ==
	      list(ScriptFieldType::Vector3, {ScriptFieldValue::FromVector3({1.0f, 2.0f, 3.0f})}));
	CHECK(RequireField(lists, "Missing").DefaultValue == list(ScriptFieldType::Float, {}));
	// C#'s zero quaternion, which the engine refuses, is described as the identity, alone and in a list.
	ScriptFieldValue const identity = ScriptFieldValue::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	CHECK(RequireField(lists, "UnsetTurn").DefaultValue == identity);
	CHECK(RequireField(lists, "UnsetTurns").DefaultValue == list(ScriptFieldType::Quaternion, {identity, identity}));

	// The attributes and the enum or asset type describe each element.
	ScriptFieldInfo const& speeds = RequireField(lists, "Speeds");
	CHECK(speeds.Tooltip == "Units per second, each");
	CHECK(speeds.Range == std::optional<glm::vec2>(glm::vec2(0.0f, 10.0f)));
	CHECK(RequireField(lists, "Shapes").AcceptedAssetType == AssetType::Mesh);
	CHECK(RequireField(lists, "ShapeList").AcceptedAssetType == AssetType::Mesh);
	CHECK(RequireField(lists, "Models").AcceptedAssetType == AssetType::None);
	ScriptFieldInfo const& levels = RequireField(lists, "Levels");
	REQUIRE(levels.Enumerators.size() == 3);
	CHECK(levels.Enumerators[2].Name == "Hard");
	CHECK(levels.Enumerators[2].Value == ScriptFieldValue::FromInt32(5));
	CHECK(levels.DefaultValue == list(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(5), ScriptFieldValue::FromInt32(0)}));
	CHECK(RequireField(lists, "LevelList").DefaultValue == list(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(1)}));
	ScriptFieldInfo const& accesses = RequireField(lists, "Accesses");
	CHECK(accesses.IsFlags);
	CHECK(accesses.Enumerators.size() == 4);
	CHECK(accesses.DefaultValue == list(ScriptFieldType::UInt32, {ScriptFieldValue::FromUInt32(3)}));
	CHECK(RequireField(lists, "Smalls").Enumerators.size() == 2);

	// Fields no field type stores, and a list longer than lists are, are reported once each class, with their types.
	std::string const reported = "Script class Strada.Tests.FieldLists: not serialized or shown in the inspector, as no field type "
								 "stores them: Table (Dictionary<String, Int32>), Jagged (Int32[][]), Grid (Int32[,]), Nested "
								 "(List<List<Int32>>), Transforms (List<Matrix4>), Oversized (its default holds 5000 elements, more "
								 "than a list's 4096)";
	CHECK(Testing::CountLogged(logStart, reported) == 1);
	CHECK(Testing::CountLogged(logStart, "Script class Strada.Tests.FieldTypes: not serialized or shown in the inspector, as no field "
	                                     "type stores them: Unsupported (Dictionary<String, Int32>)") == 1);
	// Classes whose serialized fields all have field types are not reported.
	CHECK_FALSE(Testing::WasLogged(logStart, "Script class Strada.Tests.Mover:"));
}

TEST_CASE("ScriptEngine: shuts down, starts again and replaces game assemblies")
{
	{
		Testing::ScriptEngineScope scripting;
		ScriptEngine::UnloadGameAssembly();
		CHECK_FALSE(ScriptEngine::HasGameAssembly());
		CHECK(ScriptEngine::GetClasses().empty());
		CHECK(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
		CHECK(ScriptEngine::FindClass("Strada.Tests.Mover") != nullptr);
		// Loading again replaces the assembly.
		CHECK(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
		CHECK(ScriptEngine::GetClasses().size() == 39);
		CHECK(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath().parent_path() / "Missing.dll").IsError());
	}
	CHECK_FALSE(ScriptEngine::IsInitialized());
	CHECK(ScriptEngine::FindClass("Strada.Tests.Mover") == nullptr);

	// The runtime stays loaded; a new start only sets up the scripting runtime again.
	Testing::ScriptEngineScope again;
	CHECK(ScriptEngine::FindClass("Strada.Tests.Mover") != nullptr);
}

TEST_CASE("ScriptEngine: fails cleanly without Strada.ScriptCore")
{
	Testing::TemporaryDirectory directory;
	ScriptEngineSettings settings;
	settings.ScriptCoreDirectory = directory.GetPath();
	Result<void> const result = ScriptEngine::Init(settings);
	REQUIRE(result.IsError());
	CHECK(result.GetError().find("Strada.ScriptCore.dll was not found") != std::string::npos);
	CHECK_FALSE(ScriptEngine::IsInitialized());
}

TEST_CASE("ScriptEngine: the newest hostfxr of a .NET installation is used, preferring one shipped with the application")
{
	Testing::TemporaryDirectory directory;
	auto const addHostFxr = [](std::filesystem::path const& installation, std::string const& version)
	{
		std::filesystem::path const fxr = installation / "host" / "fxr" / version;
		REQUIRE(FileSystem::CreateDirectories(fxr).IsOk());
		// Every platform's file name: the search looks for its own.
		for (char const* name : {"hostfxr.dll", "libhostfxr.so", "libhostfxr.dylib"})
		{
			REQUIRE(FileSystem::WriteTextFile(fxr / name, "").IsOk());
		}
	};

	std::filesystem::path const application = directory.GetPath() / "Game";
	std::filesystem::path const shipped = application / "dotnet";
	addHostFxr(shipped, "8.0.21");
	addHostFxr(shipped, "10.0.9");
	addHostFxr(shipped, "10.0.10-preview.1");
	addHostFxr(shipped, "9.0.4");
	// Directories that are not versions, and versions without hostfxr, are skipped.
	REQUIRE(FileSystem::CreateDirectories(shipped / "host" / "fxr" / "latest").IsOk());
	REQUIRE(FileSystem::CreateDirectories(shipped / "host" / "fxr" / "11.0.0").IsOk());

	std::optional<std::filesystem::path> const hostFxr = DotNet::FindHostFxr(shipped);
	REQUIRE(hostFxr.has_value());
	CHECK(hostFxr->parent_path().filename() == "10.0.10-preview.1");
	CHECK(DotNet::FindInstallation(application) == shipped);
	CHECK_FALSE(DotNet::FindHostFxr(directory.GetPath() / "Empty").has_value());

	// Releases come after their previews.
	addHostFxr(shipped, "10.0.10");
	CHECK(DotNet::FindHostFxr(shipped)->parent_path().filename() == "10.0.10");
}
