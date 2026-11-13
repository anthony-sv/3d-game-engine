#pragma once

// Internal to the Script module: the state of the scripting runtime that ScriptEngine (C# scripts) and VisualScriptEngine
// (visual scripts) share, and the entry points of Strada.Interop.Host they call. Main thread only, like both facades.

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Script/ScriptBindings.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLive.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

		// Visual scripts: documents are the content form of .sgraph files, variables the stored script field JSON. references
		// is the bundle of the other graphs a document's nodes refer to; a length of 0 means none.
		int32_t (*VisualScriptDescribeLibrary)(void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptAnalyze)(char const* document, int32_t documentLength, char const* references, int32_t referencesLength,
		                               void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptLoad)(uint64_t graph, char const* name, int32_t nameLength, char const* document, int32_t documentLength,
		                            char const* references, int32_t referencesLength, void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptUnload)(uint64_t graph) = nullptr;
		int32_t (*VisualScriptCreateInstance)(uint64_t entity, uint64_t graph, char const* variables, int32_t variablesLength) = nullptr;
		int32_t (*VisualScriptDestroyInstance)(uint64_t entity) = nullptr;
		int32_t (*VisualScriptDestroyAllInstances)() = nullptr;
		int32_t (*VisualScriptInvoke)(uint64_t entity, int32_t scriptEvent, float timeStep, uint64_t other) = nullptr;
		int32_t (*VisualScriptDescribeInstanceVariables)(uint64_t entity, void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptSetInstanceVariables)(uint64_t entity, char const* variables, int32_t variablesLength) = nullptr;
		// The visual-script debugger (VisualScriptDebugger): a JSON request whose JSON reply comes through receive (error
		// replies included), and the binary trace block (NotFound, receive not called, while no debugger is attached).
		int32_t (*VisualScriptDebug)(char const* request, int32_t requestLength, void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptTakeTrace)(void* context, ReceiveFunction receive) = nullptr;
		// The live channel (VisualScriptEngine::GetLiveLog): the events JSON of the receipts and runtime problems whose serial
		// is after `after` (NotFound, receive not called, when there are none), and the clearing of the log at a play
		// session's start.
		int32_t (*VisualScriptTakeLiveEvents)(uint64_t after, void* context, ReceiveFunction receive) = nullptr;
		int32_t (*VisualScriptClearLiveEvents)() = nullptr;
		// The steps the runtime's outermost executions ran since it started, their nested executions' included: read before
		// and after an invoke while the profiler records (VisualScriptEngine), to give the graph's zone its steps.
		int32_t (*VisualScriptReadSteps)(uint64_t* steps) = nullptr;
	};

	// A graph a sent document refers to, with what was sent for it (the full document of a library, the interface of the
	// others; null when the graph was absent, then its error), so that the same references can be written again.
	struct SentReference
	{
		AssetHandle Graph;
		VisualScriptReferenceUse Use = VisualScriptReferenceUse::Verification;
		// The graph's shared document, whose full form or interface was sent; null when the graph was absent.
		Ref<VisualScriptDocument const> Document;
		// The graph's asset path (the name of a memory asset); empty when it is not registered.
		std::string Name;
		// Why the graph was absent; empty when Document is set.
		std::string Error;
	};

	// The texts of a graph's document as references send it, kept per handle so that analyses and sends of an unchanged
	// neighbourhood serialize nothing: valid while the graph's shared document is Document (texts are written when first
	// needed, empty until then).
	struct VisualScriptReferenceText
	{
		Ref<VisualScriptDocument const> Document;
		// Whether the texts were written with the DevelopmentOnly nodes Disabled (DisableDevelopmentOnlyNodes).
		bool DevelopmentNodesDisabled = false;
		std::string Full;
		std::string Interface;
	};

	// What the engine knows of the programs the runtime has for a graph handle (VisualScriptEngine::EnsureLoaded).
	struct VisualScriptProgramRecord
	{
		// The asset object and version whose document the runtime has: comparing them is all an unchanged graph costs.
		Ref<VisualScriptAsset> Asset;
		uint64_t Version = 0;
		// The newest document of the program last sent, so an asset that changed object or version but not its program (a
		// save, then a refresh; a node moved) is not sent again, and whether that program compiled (no Error diagnostics) for
		// the game assembly the runtime has. A later document of the same program replaces the one sent without being sent.
		Ref<VisualScriptDocument const> Document;
		bool DocumentCompiles = false;
		// The document of the handle's working program, which the handle's instances run: the newest document of the last
		// program sent that compiled (a broken one sent later leaves it running). Null when the handle has none as far as
		// the engine has seen: no document compiled, or a creation failed (it no longer compiles for the game assembly).
		Ref<VisualScriptDocument const> WorkingDocument;
		// What the last send used beside Document, and what the working program was compiled with beside WorkingDocument: a
		// program is its document and the references it was compiled with (other graphs' documents and names), ascending by
		// handle. A reference whose graph keeps its program (a library) or link interface (another script) in a new shared
		// document takes that document, as Document does.
		std::vector<SentReference> References;
		std::vector<SentReference> WorkingReferences;
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
		// Changes whenever another program is sent or one is sent again after a game-assembly change, the graph comes back
		// after being absent, or the game assembly changes: the entities whose instance could not be created may get one now.
		uint64_t Generation = 0;
		// Platform::GetTime() when the working program last became another program (VisualScriptEngine::GetWorkingSince).
		double WorkingTime = 0.0;
		// The content text of the document last sent and the document it was written of, so that a document sent again
		// unchanged (a dependent of an edited library, a moved graph) is not written again; empty until a document is sent.
		Ref<VisualScriptDocument const> TextDocument;
		std::string Text;

		// Whether the working program runs: the handle has one, compiled for the game assembly the runtime has (after a
		// game-assembly change, sending it again or creating an instance tells). VisualScriptEngine::HasWorkingProgram answers
		// this, and only then does the log say that a broken document leaves the previous version running.
		bool HasWorkingProgram() const { return WorkingDocument != nullptr && !IsStale; }
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
		// The texts references send of each graph referred to so far, by handle.
		std::unordered_map<AssetHandle, VisualScriptReferenceText> VisualScriptReferenceTexts;
		// The entities with a graph instance, each with the graph it runs (VisualScriptEngine::DescribeLibraryUsers counts them).
		std::unordered_map<UUID, AssetHandle> GraphInstances;
		Ref<VisualScriptLibrary const> Library;
		bool IsLibraryUnavailable = false;
		// The last generation given to a program record.
		uint64_t LastProgramGeneration = 0;
		// VisualScriptDebugger::Attach succeeded and Detach has not since; the runtime's debugger goes with Shutdown, as this.
		bool VisualScriptDebuggerAttached = false;
		// Whether DevelopmentOnly nodes run (VisualScriptEngine::SetDevelopmentNodesEnabled): when they do not, the runtime
		// receives every document with them Disabled.
		bool DevelopmentNodesEnabled = true;
		// The most recent analyses, least recently made first, at most VisualScriptEngine::MaxKeptAnalyses and one per self
		// (the graph analyzed; invalid for documents of no asset): VisualScriptEngine::Analyze reads the next analysis of a
		// graph with its entry. Reuse is by equal bytes alone, so an entry never goes stale.
		std::vector<std::pair<AssetHandle, Ref<VisualScriptAnalysis const>>> VisualScriptAnalyses;
		// VisualScriptEngine::GetLastAnalysisTimings and TakeLoadTimings.
		VisualScriptAnalysisTimings LastAnalysisTimings;
		VisualScriptLoadTimings LoadTimings;
		// The live log's current revision (VisualScriptEngine::GetLiveLog); its Serial is the last event's the engine took.
		Ref<VisualScriptLiveLog const> LiveLog = CreateRef<VisualScriptLiveLog const>();
		// Whether a failure to take or read the live events was logged, so that it is logged once until a take succeeds.
		bool IsLiveLogFailureLogged = false;
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
