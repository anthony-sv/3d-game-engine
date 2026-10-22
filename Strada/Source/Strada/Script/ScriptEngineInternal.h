#pragma once

// Internal to the Script module: the state of the scripting runtime that ScriptEngine (C# scripts) and VisualScriptEngine
// (visual scripts) share, and the entry points of Strada.Interop.Host they call. Main thread only, like both facades.

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Script/ScriptBindings.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strada::ScriptEngineInternal
{
	// The statuses of Strada.Interop.Host.
	enum class ScriptStatus : int32_t
	{
		Success = 0,
		Failure = 1,
		NotFound = 2,
		BindingMismatch = 3
	};

	// Strada.Interop.ScriptEvent.
	enum class ScriptEvent : int32_t
	{
		Create = 0,
		Update = 1,
		FixedUpdate = 2,
		Destroy = 3,
		CollisionEnter = 4,
		CollisionExit = 5,
		TriggerEnter = 6,
		TriggerExit = 7,
		Click = 8
	};

	// How entry points answer with text: receive(context, text, length), called synchronously while the entry point runs
	// (the text is copied at once) and, for the visual-script entry points, exactly once when they succeed.
	using ReceiveFunction = void (*)(void* context, char const* text, int32_t length);

	// The [UnmanagedCallersOnly] entry points of Strada.Interop.Host, resolved by name when the engine initializes.
	struct HostFunctions
	{
		int32_t (*Initialize)(ScriptBinding const* bindings, int32_t count) = nullptr;
		int32_t (*Shutdown)() = nullptr;
		int32_t (*LoadGameAssembly)(char const* path, int32_t pathLength) = nullptr;
		int32_t (*UnloadGameAssembly)() = nullptr;
		int32_t (*DescribeClasses)(void* context, ReceiveFunction receive) = nullptr;
		int32_t (*CreateInstance)(char const* className, int32_t classNameLength, uint64_t entity, char const* fields,
		                          int32_t fieldsLength) = nullptr;
		int32_t (*SetInstanceFields)(uint64_t entity, char const* fields, int32_t fieldsLength) = nullptr;
		int32_t (*DescribeInstanceFields)(uint64_t entity, void* context, ReceiveFunction receive) = nullptr;
		int32_t (*DestroyInstance)(uint64_t entity) = nullptr;
		int32_t (*DestroyAllInstances)() = nullptr;
		int32_t (*Invoke)(uint64_t entity, int32_t scriptEvent, float timeStep, uint64_t other) = nullptr;

		// Visual scripts: documents are the content form of .sgraph files, variables the stored script field JSON.
		int32_t (*VisualScriptDescribeLibrary)(void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptAnalyze)(char const* document, int32_t documentLength, void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptLoad)(uint64_t graph, char const* name, int32_t nameLength, char const* document, int32_t documentLength,
		                            void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptUnload)(uint64_t graph) = nullptr;
		int32_t (*VisualScriptCreateInstance)(uint64_t entity, uint64_t graph, char const* variables, int32_t variablesLength) = nullptr;
		int32_t (*VisualScriptDestroyInstance)(uint64_t entity) = nullptr;
		int32_t (*VisualScriptDestroyAllInstances)() = nullptr;
		int32_t (*VisualScriptInvoke)(uint64_t entity, int32_t scriptEvent, float timeStep, uint64_t other) = nullptr;
		int32_t (*VisualScriptDescribeInstanceVariables)(uint64_t entity, void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptSetInstanceVariables)(uint64_t entity, char const* variables, int32_t variablesLength) = nullptr;
	};

	// What the engine knows of the programs the runtime has for a graph handle (VisualScriptEngine::EnsureLoaded).
	struct VisualScriptProgramRecord
	{
		// The asset object and version whose document the runtime has: comparing them is all an unchanged graph costs.
		Ref<VisualScriptAsset> Asset;
		uint64_t Version = 0;
		// The document last sent, so an asset that changed object or version but not content (a save, then a refresh) is
		// not sent again, and whether it compiled (no Error diagnostics) for the game assembly the runtime has.
		Ref<VisualScriptDocument const> Document;
		bool DocumentCompiles = false;
		// The document of the handle's working program, which the handle's instances run: the last one sent that compiled
		// (a broken one sent later leaves it running). Null when the handle has none as far as the engine has seen: no
		// document compiled, or a creation failed (it no longer compiles for the game assembly).
		Ref<VisualScriptDocument const> WorkingDocument;
		// The game assembly changed since the working document compiled, so it may not compile any more: sending it again
		// or creating an instance tells.
		bool IsStale = false;
		// The game assembly changed while the document last sent did not compile: it is sent again at the next
		// EnsureLoaded, since it may compile now and the engine must know which document instances run.
		bool IsSendPending = false;
		// The name the runtime's messages give the graph (its path), and the asset registry's version when it was taken:
		// the name is looked up again only after the registry changed, and a new one (a moved or renamed asset) is sent.
		std::string Name;
		uint64_t RegistryVersion = 0;
		// Whether the last EnsureLoaded found the graph absent.
		bool IsAbsent = false;
		// Changes whenever another document is sent or one is sent again after a game-assembly change, the graph comes back
		// after being absent, or the game assembly changes: the entities whose instance could not be created may get one now.
		uint64_t Generation = 0;
	};

	struct ScriptEngineData
	{
		HostFunctions Host;
		std::filesystem::path GameAssemblyPath;
		std::vector<ScriptClassInfo> Classes;
		Scene* SceneContext = nullptr;
		ScriptHost* Application = nullptr;
		// The entities with a C# script instance.
		std::unordered_set<UUID> Instances;

		// Visual scripts: the programs by graph handle, the entities with a graph instance, and the node library (read when
		// first needed and dropped when the game assembly changes; IsLibraryUnavailable remembers a failure until then).
		std::unordered_map<AssetHandle, VisualScriptProgramRecord> VisualScriptPrograms;
		std::unordered_set<UUID> GraphInstances;
		Ref<VisualScriptLibrary const> Library;
		bool IsLibraryUnavailable = false;
		// The last generation given to a program record.
		uint64_t LastProgramGeneration = 0;
	};

	// The engine's state; null while ScriptEngine is not initialized.
	ScriptEngineData* GetData();

	// A size as an entry point's length (clamped; nothing the engine sends comes near the limit).
	int32_t ToLength(size_t size);
	// A ReceiveFunction whose context is a std::string: assigns the text (no text gives an empty string).
	void ReceiveText(void* context, char const* text, int32_t length);
	// Stored script field JSON, compact; empty for no entries (the runtime reads an empty text as none).
	std::string ToJsonText(ScriptFieldMap const& fields);
	// Tells the application that a script's code failed (the runtime logged why): ScriptHost::OnScriptException.
	void ReportScriptException();
	ScriptEvent ToScriptEvent(ContactEventType type);

	// Called whenever the runtime was asked to load or unload a game assembly: it then destroyed every graph instance and
	// made every program stale (programs compile again from their documents when next used). Drops the node library,
	// changes every program's generation (one broken by a missing game type may work now) and marks every record stale.
	// Defined by VisualScriptEngine.
	void OnGameAssemblyChanged();
}
