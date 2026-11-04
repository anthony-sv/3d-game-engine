#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The live channel (spec-slice4 section 8): what each edit of a running graph did to its instances (receipts) and what went
// wrong while graphs ran (runtime problems), as VisualScriptEngine::GetLiveLog gives them to the editor.
namespace Strada
{
	enum class VisualScriptLiveOutcome : uint8_t
	{
		// The instances run the new program (Instances may be 0: no entity runs the graph now).
		Applied = 0,
		// A running step of the old program holds the swap; a later Applied receipt with Deferred follows.
		Pending,
		// The document does not compile; the instances keep the last version that worked.
		KeptLastWorking,
		// The document does not compile and the graph has no version that runs.
		NotRunning
	};

	// A variable a receipt names: Detail is the name it was matched by ("Velocity"), or why it was reset ("type changed from
	// int to float", "new").
	struct VisualScriptLiveVariable
	{
		std::string Name;
		std::string Detail;

		bool operator==(VisualScriptLiveVariable const& other) const = default;
	};

	// A node a receipt names: one whose state the edit reset, or where it cancelled a wait or a stopped execution.
	struct VisualScriptLiveNode
	{
		// The document holding the node: the graph, or a library.
		AssetHandle Document;
		uint32_t Function = 0;
		uint32_t Node = 0;
		std::string Title;
		// A reset state: "Removed", "Changed" or "Disabled". A cancellation: the scripting runtime's sentence ("the edit
		// removed or changed Delay (node 12)").
		std::string Reason;
		// A cancellation of a stopped execution rather than of a wait.
		bool Stopped = false;

		bool operator==(VisualScriptLiveNode const& other) const = default;
	};

	// A Timeline of an instance after the edit: its time, and the time it had when the edit shortened it below that.
	struct VisualScriptLiveTimeline
	{
		uint32_t Node = 0;
		// NaN when the time is not finite.
		double Time = 0.0;
		std::optional<double> ClampedFrom;
	};

	// What an edit did to one instance's waits, stopped executions and Timelines.
	struct VisualScriptLiveInstance
	{
		UUID Entity = UUID::Invalid();
		uint32_t WaitsKept = 0;
		uint32_t StopsKept = 0;
		std::vector<VisualScriptLiveNode> Cancelled;
		std::vector<VisualScriptLiveTimeline> Timelines;
	};

	// A receipt: what one load of a graph's program did to the instances that run the graph.
	struct VisualScriptLiveEdit
	{
		// The live events' serial, which grows with every event.
		uint64_t Serial = 0;
		AssetHandle Graph;
		// The graph's path (the name of a memory asset), as the scripting runtime's messages give it.
		std::string Name;
		// The other graph whose change made this graph compile again (a library it calls, a script whose variables it reads);
		// invalid when the graph's own document changed.
		AssetHandle Cause;
		VisualScriptLiveOutcome Outcome = VisualScriptLiveOutcome::Applied;
		// An Applied receipt that ends an earlier Pending one: the step that held the swap finished.
		bool Deferred = false;
		// The instances swapped, and those a running step of the old program still holds.
		uint32_t Instances = 0;
		uint32_t Pending = 0;
		// What the edit carried of the variables, the same for every instance: kept by ID, matched by name, reset (with
		// why), added and removed.
		std::vector<std::string> KeptVariables;
		std::vector<VisualScriptLiveVariable> MatchedVariables, ResetVariables;
		std::vector<std::string> AddedVariables, RemovedVariables;
		// The node states (Do Once, Do N, Flip Flop, Gate, stored outputs) kept, and those reset.
		uint32_t KeptStates = 0;
		std::vector<VisualScriptLiveNode> ResetStates;
		// The instances with waits, stopped executions or Timelines, at most 64; MoreInstances counts the others.
		std::vector<VisualScriptLiveInstance> InstanceNotes;
		uint32_t MoreInstances = 0;
		// KeptLastWorking and NotRunning: the errors of the document ("1 error; first: <message> (node <id>)").
		std::string Errors;
		// Platform::GetTime() when the engine took it.
		double Time = 0.0;
	};

	// A Function.Call on the stack of the execution a runtime problem happened in.
	struct VisualScriptCallFrame
	{
		AssetHandle Document;
		uint32_t Function = 0;
		uint32_t Node = 0;

		bool operator==(VisualScriptCallFrame const& other) const = default;
	};

	// An error that aborted an execution, or a warning, at a node of a graph instance: one entry per graph, document, node,
	// entity and severity, counted.
	struct VisualScriptRuntimeProblem
	{
		uint64_t Serial = 0;
		VisualScriptSeverity Severity = VisualScriptSeverity::Error;
		// The graph the instance runs.
		AssetHandle Graph;
		// The document holding the node: the graph, or a library.
		AssetHandle Document;
		uint32_t Function = 0;
		uint32_t Node = 0;
		std::string Title;
		UUID Entity = UUID::Invalid();
		// The latest.
		std::string Message;
		// How often it happened since play started; at least 1.
		uint32_t Count = 0;
		// The calls that led to the node, innermost last (at most 8).
		std::vector<VisualScriptCallFrame> Calls;
		// Platform::GetTime() when the engine first and last took it.
		double FirstTime = 0.0;
		double LastTime = 0.0;
	};

	// One revision of the live log. A revision never changes: taking new events makes a new revision, which shares the
	// unchanged entries with the old one (they are immutable and shared), so whoever holds a revision may read it for as long
	// as it holds it, while edits made meanwhile (a window's commit in the middle of a frame) make newer ones.
	struct VisualScriptLiveLog
	{
		// The receipts a revision keeps: the newest.
		static constexpr size_t MaxEdits = 64;

		// The last event's serial.
		uint64_t Serial = 0;
		// The occurrences of runtime problems the scripting runtime could not keep (it keeps 512 distinct ones).
		uint32_t Dropped = 0;
		// The last MaxEdits receipts, oldest first.
		std::vector<Ref<VisualScriptLiveEdit const>> Edits;
		// This play session's runtime problems, by serial (a problem that happens again moves to the end).
		std::vector<Ref<VisualScriptRuntimeProblem const>> Problems;
	};

	// A graph whose program calls a library (VisualScriptEngine::DescribeLibraryUsers).
	struct VisualScriptLibraryUser
	{
		AssetHandle Graph;
		// The graph's path (the name of a memory asset).
		std::string Name;
		// Whether its working program was compiled with the library's current document: else it runs an earlier version.
		bool RunsCurrent = false;

		bool operator==(VisualScriptLibraryUser const& other) const = default;
	};

	// Reads the events JSON the scripting runtime writes (spec-slice4 section 8.4): its receipts, with Outcome Swapped read as
	// Applied and Kept as KeptLastWorking (the engine decides between KeptLastWorking and NotRunning), and its runtime
	// problems; last and dropped get its Last and Dropped. Times and Cause stay unset. Fails, naming the first member that is
	// not of its form, and then leaves last and dropped unchanged. Pure; any thread.
	[[nodiscard]] Result<std::pair<std::vector<VisualScriptLiveEdit>, std::vector<VisualScriptRuntimeProblem>>>
	ParseVisualScriptLiveEvents(std::string_view json, uint64_t& last, uint32_t& dropped);

	// The revision after log with the events taken since (the next revision rule, spec-slice4 section 8.5): the receipts
	// appended in their order and the oldest beyond MaxEdits dropped; each problem replacing the one of its graph, document,
	// node, entity and severity, at the end, with that one's FirstTime; Serial and Dropped from the events. A Deferred receipt
	// that has no Cause takes that of the Pending receipt of its graph it ends. log is unchanged, and the entries of the new
	// revision that did not change are log's. Pure; any thread.
	[[nodiscard]] Ref<VisualScriptLiveLog const> AppendVisualScriptLiveEvents(VisualScriptLiveLog const& log,
	                                                                          std::vector<VisualScriptLiveEdit> edits,
	                                                                          std::vector<VisualScriptRuntimeProblem> problems,
	                                                                          uint64_t serial, uint32_t dropped);
}
