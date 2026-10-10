#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Script/ScriptField.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class Scene;

	// A named value of a C# enum field.
	struct ScriptEnumerator
	{
		std::string Name;
		// Of the field's (integer) type.
		ScriptFieldValue Value;
	};

	// A serialized field of a script class: a public field, or one marked [SerializeField], of a supported type.
	struct ScriptFieldInfo
	{
		std::string Name;
		ScriptFieldType Type = ScriptFieldType::None;
		// The value a new instance starts with.
		ScriptFieldValue DefaultValue;
		// [HideInInspector]: saved with the scene but not shown.
		bool Hidden = false;
		// [Tooltip]; empty without one.
		std::string Tooltip;
		// [Range(min, max)].
		std::optional<glm::vec2> Range;
		// Asset fields of a typed reference (Mesh, Material, ...): the type of asset they accept. None accepts any asset.
		AssetType AcceptedAssetType = AssetType::None;
		// Enum fields, stored as their integer values: the enumerators in declaration order. Empty for other fields.
		std::vector<ScriptEnumerator> Enumerators;
		// A [Flags] enum: values combine enumerators bitwise.
		bool IsFlags = false;
	};

	// A class of the game assembly that derives from Strada.Script.
	struct ScriptClassInfo
	{
		// The full name a Script component refers to, e.g. "Game.PlayerController".
		std::string Name;
		// Base classes' fields first, then in declaration order.
		std::vector<ScriptFieldInfo> Fields;

		ScriptFieldInfo const* FindField(std::string_view name) const;
	};

	// A check reported by Strada.Testing.TestReporter.
	struct ScriptTestResult
	{
		std::string Name;
		bool Passed = false;
		// Why it failed; empty when it passed.
		std::string Message;
	};

	// The application that runs scenes with scripts (the editor's play mode, the runtime player): what Strada.Application,
	// Strada.SceneManager and Strada.Testing ask of it. Calls arrive on the main thread while scripts run, so a host acts on
	// scene loads and quitting once the scene's update has returned.
	class ScriptHost
	{
	public:
		virtual ~ScriptHost() = default;

		virtual bool IsEditor() const = 0;
		// SceneManager.LoadScene with a registered scene asset: the running scene is to be replaced after this frame.
		virtual void RequestSceneLoad(AssetHandle scene) = 0;
		// Application.Quit: running is to stop after this frame (the editor leaves play mode).
		virtual void RequestQuit() = 0;
		virtual void ReportTestResult(ScriptTestResult const& result) = 0;
		// TestReporter.Finish: the scripts' test run is complete.
		virtual void FinishTests() = 0;
		// A script's callback or constructor threw (the exception was logged).
		virtual void OnScriptException() = 0;
	};

	struct ScriptEngineSettings
	{
		// The directory of Strada.ScriptCore.dll and its runtimeconfig.json; empty for the executable's directory.
		std::filesystem::path ScriptCoreDirectory;
	};

	// The C# scripting runtime (.NET hosted through hostfxr; see DotNetHost.h). Initialized by Application after the
	// AudioEngine. The .NET runtime starts with the first Init and stays for the rest of the process, as it cannot be
	// unloaded; Shutdown unloads the game's scripts and leaves no other state, so Init may follow again. Main thread only.
	class ScriptEngine
	{
	public:
		[[nodiscard]] static Result<void> Init(ScriptEngineSettings const& settings = {});
		static void Shutdown();
		static bool IsInitialized();

		// Loads the game's script assembly in place of the current one; fails while instances exist. The file stays
		// unlocked, so it can be rebuilt and loaded again (hot reload).
		[[nodiscard]] static Result<void> LoadGameAssembly(std::filesystem::path const& path);
		static void UnloadGameAssembly();
		static bool HasGameAssembly();
		static std::filesystem::path const& GetGameAssemblyPath();
		// The game's script classes, sorted by name.
		static std::vector<ScriptClassInfo> const& GetClasses();
		static ScriptClassInfo const* FindClass(std::string_view name);

		// --- Instances of the running scene (driven by Scene) ---

		// The scene the scripts act on while it runs.
		static void SetSceneContext(Scene* scene);
		static Scene* GetSceneContext();
		// The application scripts talk to (not owned; it outlives its registration). Without one, their requests are logged
		// and ignored.
		static void SetHost(ScriptHost* host);
		static ScriptHost* GetHost();

		// Creates the entity's instance of a class and sets its fields. Stored values of fields the class no longer has
		// are skipped and values of another type are logged and skipped, keeping the field's default.
		[[nodiscard]] static Result<void> CreateInstance(UUID entity, std::string_view className, ScriptFieldMap const& fields);
		static bool HasInstance(UUID entity);
		// Without calling OnDestroy.
		static void DestroyInstance(UUID entity);
		static void DestroyAllInstances();
		static size_t GetInstanceCount();

		// Lifecycle calls on the entity's instance, if it has one; script exceptions are logged and do not propagate.
		static void InvokeOnCreate(UUID entity);
		static void InvokeOnUpdate(UUID entity, float deltaTime);
		static void InvokeOnFixedUpdate(UUID entity, float fixedDeltaTime);
		// Then destroys the instance.
		static void InvokeOnDestroy(UUID entity);
		static void InvokeContactEvent(UUID entity, UUID other, ContactEventType type);
	};
}
