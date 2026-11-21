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
	struct PlayerWindowSettings;

	// A named value of a C# enum field.
	struct ScriptEnumerator
	{
		std::string Name;
		// Of the field's (integer) type.
		ScriptFieldValue Value;
	};

	// A serialized field of a script class: a public field, or one marked [SerializeField], of a supported type.
	// A field of a script class or a public variable of a graph. A list field (a C# T[] or List<T>, a graph's public array;
	// Type a list type) holds values its element type describes: Range, AcceptedAssetType and the enumerators are each
	// element's (Docs/Architecture.md §10.3).
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
		// Enum fields, stored as their integer values: the enumerators in declaration order, values of the element type for
		// a list. Empty for other fields.
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
		// The player's window settings changed (PlayerSettingsStore::SetWindow, Script/PlayerSettings.h): the application's
		// window is to take them after this frame, as its own rules allow (the player resizes its window, makes it fullscreen
		// and sets vsync, minding its command line; the editor's play mode keeps them without applying them).
		virtual void RequestWindowSettings(PlayerWindowSettings const& window) = 0;
	};

	// A contact event as one of its two entities' handlers see it (Strada.Collision): the event, and whether the handling
	// entity is its Second, whose view of the points ReverseContactPoint gives.
	struct HandledContactEvent
	{
		ContactEvent const* Event = nullptr;
		bool FromSecond = false;

		// The handling entity, and the other one.
		UUID GetEntity() const { return FromSecond ? Event->Second : Event->First; }
		UUID GetOther() const { return FromSecond ? Event->First : Event->Second; }
		// The event's point at the index (below Event->PointCount), seen from the handling entity.
		ContactPoint GetPoint(uint32_t index) const
		{
			return FromSecond ? ReverseContactPoint(Event->Points[index]) : Event->Points[index];
		}
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

		// Loads the game's script assembly in place of the current one; fails while instances exist (GetInstanceCount). The
		// file stays unlocked, so it can be rebuilt and loaded again (hot reload).
		[[nodiscard]] static Result<void> LoadGameAssembly(std::filesystem::path const& path);
		// Unloads the game's scripts, if any: the instances of both kinds go with them (without OnDestroy or Event.Destroy),
		// and visual scripts compile again when next used.
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
		// The values the serialized fields of the entity's instance hold now, in the stored form, by name (the fields of
		// every class it derives from included). Fails when the entity has no instance.
		[[nodiscard]] static Result<ScriptFieldMap> GetInstanceFields(UUID entity);
		// The serialized fields of the entity's instance whose values differ from those the last GetChangedInstanceFields of
		// that instance answered, as GetInstanceFields gives them: every field at the instance's first, and a field given a
		// value since (SetInstanceFields), whatever became of it. The runtime compares each field with a copy of the value it
		// last answered without converting it to the stored form (a list's elements copied in their own type), so an unchanged
		// list costs one comparison of its elements, and a changed one is answered whole (Docs/Architecture.md §11, "Long
		// lists"). Fails when the entity has no instance.
		[[nodiscard]] static Result<ScriptFieldMap> GetChangedInstanceFields(UUID entity);
		// Sets fields of the entity's instance from stored values, as CreateInstance does: values of fields the class does
		// not have are skipped, and values of another type are logged and skipped. Fails when the entity has no instance.
		[[nodiscard]] static Result<void> SetInstanceFields(UUID entity, ScriptFieldMap const& fields);
		static bool HasInstance(UUID entity);
		// Without calling OnDestroy.
		static void DestroyInstance(UUID entity);
		static void DestroyAllInstances();
		// The instances of the running scene: C# script instances plus graph instances (VisualScriptEngine). The game
		// assembly cannot change while there are any.
		static size_t GetInstanceCount();

		// Lifecycle calls on the entity's instance, if it has one; script exceptions are logged and do not propagate.
		static void InvokeOnCreate(UUID entity);
		static void InvokeOnUpdate(UUID entity, float deltaTime);
		static void InvokeOnFixedUpdate(UUID entity, float fixedDeltaTime);
		// Then destroys the instance.
		static void InvokeOnDestroy(UUID entity);
		static void InvokeContactEvent(UUID entity, UUID other, ContactEventType type);
		// The contact event whose handlers run now, C# scripts' and graphs' alike (Strada.Collision), while a
		// ContactEventScope lives; null outside one.
		static HandledContactEvent const* GetHandledContactEvent();
		// OnClick(button) of the entity's instance: the entity is the clicked UI button or one of its ancestors.
		static void InvokeOnClick(UUID entity, UUID button);
		// OnActionPressed(action) and OnActionReleased(action) of the entity's instance, the action being the one an
		// InputActionEventScope names (Docs/Architecture.md §7.4, "Events"); nothing runs outside one.
		static void InvokeOnActionPressed(UUID entity);
		static void InvokeOnActionReleased(UUID entity);
		// The name of the input action whose edge is being dispatched, C# scripts' and graphs' alike, while an
		// InputActionEventScope lives; nothing outside one.
		static std::optional<std::string_view> GetHandledAction();

		// Makes a contact event, seen from one of its entities, the one being handled (GetHandledContactEvent) for its
		// lifetime: the scene keeps one around the InvokeContactEvent calls of that entity's C# script and graph. Scopes
		// nest, each restoring the one before; the event must outlive the scope. Without an initialized engine it does
		// nothing.
		class ContactEventScope
		{
		public:
			ContactEventScope(ContactEvent const& event, bool fromSecond);
			~ContactEventScope();
			ContactEventScope(ContactEventScope const&) = delete;
			ContactEventScope& operator=(ContactEventScope const&) = delete;

		private:
			HandledContactEvent m_Handled;
			HandledContactEvent const* m_Previous = nullptr;
			bool m_IsActive = false;
		};

		// Makes an input action the one whose edge is being dispatched (GetHandledAction) for its lifetime: the scene keeps
		// one around the InvokeOnActionPressed or InvokeOnActionReleased calls of every script instance for one edge. Scopes
		// nest, each restoring the one before; the name's characters must outlive the scope. Without an initialized engine
		// it does nothing.
		class InputActionEventScope
		{
		public:
			explicit InputActionEventScope(std::string_view action);
			~InputActionEventScope();
			InputActionEventScope(InputActionEventScope const&) = delete;
			InputActionEventScope& operator=(InputActionEventScope const&) = delete;

		private:
			std::string_view m_Action;
			std::string_view const* m_Previous = nullptr;
			bool m_IsActive = false;
		};
	};
}
