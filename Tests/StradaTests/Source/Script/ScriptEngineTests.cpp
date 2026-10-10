#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Script/DotNetHost.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
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
	CHECK(names == std::vector<std::string>{
					   "Strada.Tests.AssetProbe", "Strada.Tests.AudioProbe", "Strada.Tests.BaseBehaviour", "Strada.Tests.ComponentWriter",
					   "Strada.Tests.DerivedBehaviour", "Strada.Tests.FieldTypes", "Strada.Tests.HierarchyProbe", "Strada.Tests.HostProbe",
					   "Strada.Tests.InputProbe", "Strada.Tests.Lifecycle", "Strada.Tests.Mover", "Strada.Tests.PhysicsProbe",
					   "Strada.Tests.PrefabMember", "Strada.Tests.PrefabProbe", "Strada.Tests.SceneSwitcher", "Strada.Tests.Spawner",
					   "Strada.Tests.Thrower", "Strada.Tests.TransformProbe", "Strada.Tests.ValueGuard"});
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
		CHECK(ScriptEngine::GetClasses().size() == 19);
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
