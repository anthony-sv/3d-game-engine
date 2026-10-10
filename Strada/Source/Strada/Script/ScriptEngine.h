#pragma once

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
