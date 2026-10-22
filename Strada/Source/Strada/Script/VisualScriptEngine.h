#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Strada
{
	// What VisualScriptEngine has of the runtime's node library.
	enum class VisualScriptLibraryState : uint8_t
	{
		// The scripting runtime is not running.
		NoRuntime,
		// Not read yet: GetLibrary reads it (which takes a moment).
		NotBuilt,
		Built,
		// The runtime could not describe it, or its description could not be read (logged): so until the game assembly
		// changes or the engine shuts down.
		Failed
	};

	// The visual scripts of the scripting runtime: its node library, its analysis of documents, the programs it compiles
	// from graph assets and the graph instances of the running scene, which Scene drives like C# script instances. It works
	// while ScriptEngine::IsInitialized(), and its state goes with ScriptEngine::Shutdown. Main thread only.
	class VisualScriptEngine
	{
	public:
		// --- Library and analysis ---

		// The node library, read from the runtime when first asked for and kept until the game assembly changes or the
		// engine shuts down. Null without the runtime, or when the runtime cannot describe it (logged once until then).
		static Ref<VisualScriptLibrary const> GetLibrary();
		// Whether GetLibrary has the library, would read it, or cannot; reads nothing.
		static VisualScriptLibraryState GetLibraryState();
		// The lookup of enum types the public variables of visual scripts use (the inspector calls it every frame): it
		// searches the library this call gets and keeps, never calling into the runtime. Empty without a library, so enum
		// variables read as needing the scripting runtime.
		static VisualScriptEnumLookup MakeEnumLookup();
		// The enum type of a type ID in the library, which GetLibrary reads when it has none yet (after that a lookup costs
		// no call into the runtime); nothing for the other types and without a library.
		static std::optional<VisualScriptEnumInfo> FindEnum(std::string_view typeId);
		// The runtime's analysis of a document: its nodes with their resolved pins, and the problems found. Fails only
		// without the runtime, or when the runtime fails (logged).
		[[nodiscard]] static Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const& document);

		// --- Programs ---

		// Gives the runtime the graph asset's current document unless it has it: a new asset object or version whose
		// document differs from the one sent is sent again (a live edit while playing), one with the same document only
		// updates what is known. A document that does not compile is sent once, its errors logged once; the program the
		// handle had keeps running. It is sent again after a game-assembly change, as it may compile now. The runtime names
		// the graph by its path: a graph moved or renamed is sent again under its new one. Unchanged graphs cost a lookup and
		// a few comparisons, so Scene calls it every update. Fails, with the reason, when the graph is absent: not a
		// registered VisualScript asset, or its file does not load.
		[[nodiscard]] static Result<void> EnsureLoaded(AssetHandle graph);
		// Changes whenever another document of the graph is sent, the graph comes back after being absent, or the game
		// assembly changes: an entity whose instance could not be created may get one then. 0 for a graph never loaded.
		static uint64_t GetProgramGeneration(AssetHandle graph);
		// The document of the graph's working program, which its instances run: the last one sent that compiled (the asset
		// may hold a newer one, sent at the next EnsureLoaded, or a broken one, which leaves it running). Null when the
		// graph has no working program as far as the engine has seen, or was never loaded.
		static Ref<VisualScriptDocument const> GetWorkingDocument(AssetHandle graph);

		// --- Graph instances of the running scene (driven by Scene) ---

		// Creates the entity's instance of the graph (EnsureLoaded first), replacing the one it has, with the component's
		// variable values (stored script fields, by variable name; those of no public variable are skipped). Fails when the
		// graph is absent, or has no working program: the runtime logged its errors, and that is a script exception
		// (ScriptHost::OnScriptException).
		[[nodiscard]] static Result<void> CreateInstance(UUID entity, AssetHandle graph, ScriptFieldMap const& variables);
		static bool HasInstance(UUID entity);
		// Without running Event.Destroy.
		static void DestroyInstance(UUID entity);
		static void DestroyAllInstances();
		// Graph instances only; ScriptEngine::GetInstanceCount counts both kinds.
		static size_t GetInstanceCount();
		// The values the public variables of the entity's instance hold now, in the stored form, by name. Fails when the
		// entity has no instance (an instance the runtime no longer has is forgotten, as by the events).
		[[nodiscard]] static Result<ScriptFieldMap> GetInstanceVariables(UUID entity);
		// Sets public variables of the entity's instance from stored values, as CreateInstance does. Fails when the entity
		// has no instance, like GetInstanceVariables.
		[[nodiscard]] static Result<void> SetInstanceVariables(UUID entity, ScriptFieldMap const& variables);

		// Events of the entity's instance, if it has one. A graph that fails (the runtime logged why) is a script exception;
		// the instance runs on. An instance the runtime no longer has is forgotten.
		static void InvokeOnCreate(UUID entity);
		static void InvokeOnUpdate(UUID entity, float deltaTime);
		static void InvokeOnFixedUpdate(UUID entity, float fixedDeltaTime);
		// Then the instance is gone.
		static void InvokeOnDestroy(UUID entity);
		static void InvokeContactEvent(UUID entity, UUID other, ContactEventType type);
		// The entity is the clicked UI button or one of its ancestors.
		static void InvokeOnClick(UUID entity, UUID button);
	};
}
