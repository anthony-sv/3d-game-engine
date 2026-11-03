#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	// A breakpoint as the debugger is configured with it: a node of a document (a script graph or a function library). Type is
	// the node's Type when the breakpoint was set: the runtime stops only at a compiled node of that Type. HitCount: stop from
	// the nth time the condition holds (0 and 1: every time).
	struct VisualScriptBreakpointSetting
	{
		AssetHandle Document;
		uint32_t Node = 0;
		std::string Type;
		bool Enabled = true;
		std::string Condition;
		uint32_t HitCount = 0;

		bool operator==(VisualScriptBreakpointSetting const& other) const = default;
	};

	// The instance of a script graph whose executions stop at its breakpoints; an invalid entity: any instance.
	struct VisualScriptDebugTarget
	{
		AssetHandle Graph;
		UUID Entity = UUID::Invalid();

		bool operator==(VisualScriptDebugTarget const& other) const = default;
	};

	// A sub-graph whose flow is counted for the take (a document and a function of it; 0: a script graph's event graph).
	struct VisualScriptDebugWatch
	{
		AssetHandle Document;
		uint32_t Function = 0;

		bool operator==(VisualScriptDebugWatch const& other) const = default;
	};

	struct VisualScriptDebugConfiguration
	{
		// The configuring side's number of this configuration, answered back by the runtime's state.
		uint32_t Revision = 0;
		std::vector<VisualScriptBreakpointSetting> Breakpoints;
		std::vector<VisualScriptDebugTarget> Targets;
		std::vector<VisualScriptDebugWatch> Watches;
		bool BreakOnErrors = false;

		bool operator==(VisualScriptDebugConfiguration const& other) const = default;
	};

	enum class VisualScriptDebugAction : uint8_t
	{
		Continue = 0,
		StepInto,
		StepOver,
		StepOut
	};

	enum class VisualScriptStopReason : uint8_t
	{
		Breakpoint = 0,
		Step,
		Break,
		RunTo,
		Error,
		Passed
	};

	// Where a stopped execution stands: entering a node, continuing a flow entry (a Sequence or a loop), or failing.
	enum class VisualScriptStopPlace : uint8_t
	{
		Enter = 0,
		Continue,
		Error
	};

	enum class VisualScriptStopFrameKind : uint8_t
	{
		Graph = 0,
		Call,
		Delivery,
		Native
	};

	enum class VisualScriptStartKind : uint8_t
	{
		Event = 0,
		Tick,
		Timeline,
		Resume,
		SendEvent,
		Delivery
	};

	// Stopped, Ended, Waiting and Dismissed are the runtime's answers to a resume. Armed and Resumed are the editor's debug
	// session's only: a run-to or a step armed with nothing to resume, and a Continue with no stop that resumed the scene the
	// debugger still held paused.
	enum class VisualScriptResumeResult : uint8_t
	{
		Stopped = 0,
		Ended,
		Waiting,
		Dismissed,
		Armed,
		Resumed
	};

	// Which run a value of a stopped frame comes from: the stopped execution's, another or an earlier one's, none (the
	// default), or nobody can say (no traced run stamped the frame).
	enum class VisualScriptValueCurrency : uint8_t
	{
		Current = 0,
		Earlier,
		NotComputed,
		Unknown
	};

	// Why a breakpoint does not stop: the running program has no node of its Type under its ID, no instance runs a program
	// with the document, or (the editor's only) its target entity is gone.
	enum class VisualScriptBreakpointInactivity : uint8_t
	{
		None = 0,
		NotInProgram,
		NotRunning,
		TargetGone
	};

	// A value as the runtime encodes it: exactly one of Value (literal forms, a string cut at 256 bytes, an array's first 32
	// elements as JSON objects of this encoding without Type, or null for a null reference), Entity (entities, and with Text
	// components and script instances) and Text (what has no literal form, and non-finite numbers and math structs, such as
	// "NaN" or "(Infinity, 0, 1)"), with Length and Truncated for cut strings and arrays. Values are not formatted for
	// display here: the editor formats them.
	struct VisualScriptDebugValue
	{
		std::string Name;
		// The type ID; empty for a value without one (array elements, and Before).
		std::string Type;
		Json Value;
		std::optional<UUID> Entity;
		// False for an entity the scene no longer has.
		bool EntityValid = true;
		std::string Text;
		// A cut string's whole length (UTF-16 units) or an array's element count.
		std::optional<uint32_t> Length;
		bool Truncated = false;
		VisualScriptValueCurrency Currency = VisualScriptValueCurrency::Current;
		// Variables: their ID and whether public.
		uint32_t ID = 0;
		bool Public = false;
		// Something else changed it while the execution was stopped (variables and event-graph outputs).
		bool Changed = false;
		// An unlinked input of a stopped execution a live edit moved to a new program: this value is the new program's
		// literal, and Before is the snapshot's, as the JSON object of this encoding without Type ({"Value": 10}).
		bool Edited = false;
		Json Before;

		bool operator==(VisualScriptDebugValue const& other) const = default;
	};

	// A value found changed elsewhere while stopped: a variable (Node 0, Name the variable) or an output (Node, Name the pin).
	struct VisualScriptChangedValue
	{
		uint32_t Node = 0;
		std::string Name;

		bool operator==(VisualScriptChangedValue const& other) const = default;
	};

	// A formula evaluated against a stopped frame: its value, or its error.
	struct VisualScriptEvaluated
	{
		std::string Formula;
		std::optional<VisualScriptDebugValue> Value;
		std::string Error;

		bool operator==(VisualScriptEvaluated const& other) const = default;
	};

	// A frame of a stop's call stack. A Native frame (engine code between two executions) has only its Kind and Title
	// ("Entity.Instantiate").
	struct VisualScriptStopFrame
	{
		VisualScriptStopFrameKind Kind = VisualScriptStopFrameKind::Graph;
		UUID Entity = UUID::Invalid();
		// The instance's graph.
		AssetHandle Graph;
		// The document holding the node: the graph, or a library.
		AssetHandle Document;
		uint32_t Function = 0;
		std::string FunctionName;
		uint32_t Node = 0;
		std::string Title;
		// The entry pin at an Enter position, the exit a Continue takes; empty otherwise.
		std::string Pin;
		// "next: Then1", "next iteration: Index 3", or empty.
		std::string Note;

		bool operator==(VisualScriptStopFrame const& other) const = default;
	};

	// Where the execution that suspended started, for a resumed fiber (Title is empty when an edit removed the node).
	struct VisualScriptStopOrigin
	{
		AssetHandle Document;
		uint32_t Node = 0;
		std::string Title;

		bool operator==(VisualScriptStopOrigin const& other) const = default;
	};

	// How the stopped execution (the chain's leaf) started.
	struct VisualScriptStopStart
	{
		VisualScriptStartKind Kind = VisualScriptStartKind::Event;
		AssetHandle Graph;
		AssetHandle Document;
		uint32_t Node = 0;
		std::string Title;
		std::string Pin;
		std::optional<VisualScriptStopOrigin> Origin;

		bool operator==(VisualScriptStopStart const& other) const = default;
	};

	// The debugger's record of a stopped execution (resumable), of a breakpoint an execution that cannot stop passed
	// (Passed, read-only) or of an aborting error (Error, read-only).
	struct VisualScriptStop
	{
		uint64_t ID = 0;
		VisualScriptStopReason Reason = VisualScriptStopReason::Breakpoint;
		bool Resumable = true;
		UUID Entity = UUID::Invalid();
		AssetHandle Graph;
		VisualScriptStopPlace Place = VisualScriptStopPlace::Enter;
		// The error; for Passed why it could not stop; empty otherwise.
		std::string Message;
		bool Migrated = false;
		std::vector<VisualScriptChangedValue> Changed;
		uint32_t ChangedMore = 0;
		// The breakpoint it stopped at: (document, node).
		std::optional<std::pair<AssetHandle, uint32_t>> Breakpoint;
		// Innermost first, across the chain.
		std::vector<VisualScriptStopFrame> Frames;
		VisualScriptStopStart Start;

		bool operator==(VisualScriptStop const& other) const = default;
	};

	struct VisualScriptBreakpointState
	{
		AssetHandle Document;
		uint32_t Node = 0;
		uint32_t Hits = 0;
		uint32_t Reached = 0;
		bool Active = true;
		VisualScriptBreakpointInactivity Reason = VisualScriptBreakpointInactivity::None;
		// The condition's error ("line 1, column 1: unknown name angle: did you mean Angle?"); empty when it has none.
		std::string Error;

		bool operator==(VisualScriptBreakpointState const& other) const = default;
	};

	// Where a step's chain waits in a latent node.
	struct VisualScriptStepWait
	{
		AssetHandle Document;
		uint32_t Function = 0;
		uint32_t Node = 0;
		std::string Title;
		UUID Entity = UUID::Invalid();

		bool operator==(VisualScriptStepWait const& other) const = default;
	};

	// The step that runs or waits: the stop it was made from, its action and where its chain waits (none while it does not).
	struct VisualScriptStepping
	{
		uint64_t Stop = 0;
		VisualScriptDebugAction Action = VisualScriptDebugAction::StepOver;
		std::optional<VisualScriptStepWait> WaitsIn;

		bool operator==(VisualScriptStepping const& other) const = default;
	};

	struct VisualScriptDebugState
	{
		// The revision of the configuration the runtime has.
		uint32_t Revision = 0;
		// Changes whenever a stop is made, resumed, migrated, cancelled or ends, and whenever a step starts waiting, ends or is
		// cancelled.
		uint64_t StopRevision = 0;
		std::optional<uint64_t> Current;
		// Breakpoint hits beyond the most stops the runtime keeps, since the last resume.
		uint32_t Passed = 0;
		std::optional<VisualScriptStepping> Stepping;
		std::vector<VisualScriptStop> Stops;
		std::vector<VisualScriptBreakpointState> Breakpoints;

		bool operator==(VisualScriptDebugState const& other) const = default;
	};

	struct VisualScriptResumeOutcome
	{
		VisualScriptResumeResult Result = VisualScriptResumeResult::Ended;
		// An aborting error ended a continued execution (the facade reported it as a script exception).
		bool Failed = false;
		// The stop the resume reached.
		std::optional<uint64_t> Stop;
		uint64_t StopRevision = 0;

		bool operator==(VisualScriptResumeOutcome const& other) const = default;
	};

	// An active loop or Sequence of a stopped frame. Kind is ForLoop (Index, First, Last), ForEach (Index, Length, Element),
	// While (nothing more) or Sequence (Next: the output it runs next).
	struct VisualScriptLoopState
	{
		uint32_t Node = 0;
		std::string Kind;
		int64_t Index = 0;
		int64_t First = 0;
		int64_t Last = 0;
		uint32_t Length = 0;
		std::optional<VisualScriptDebugValue> Element;
		std::string Next;

		bool operator==(VisualScriptLoopState const& other) const = default;
	};

	struct VisualScriptNodeValues
	{
		uint32_t Node = 0;
		std::vector<VisualScriptDebugValue> Inputs;
		std::vector<VisualScriptDebugValue> Outputs;

		bool operator==(VisualScriptNodeValues const& other) const = default;
	};

	// One frame of a stop, described: its node's inputs and outputs, the start event, the parameters, locals and variables,
	// the active loops, the other nodes the stopped execution computed in it and the formulas evaluated against it.
	struct VisualScriptStopValues
	{
		uint64_t Stop = 0;
		uint32_t Frame = 0;
		UUID Entity = UUID::Invalid();
		AssetHandle Graph;
		AssetHandle Document;
		uint32_t Function = 0;
		VisualScriptNodeValues Node;
		std::vector<VisualScriptDebugValue> Event;
		std::vector<VisualScriptDebugValue> Parameters;
		std::vector<VisualScriptDebugValue> Locals;
		std::vector<VisualScriptDebugValue> Variables;
		std::vector<VisualScriptLoopState> Loops;
		std::vector<VisualScriptNodeValues> Computed;
		std::vector<VisualScriptEvaluated> Evaluated;
		// The values left out (a reply holds at most 4,096), Computed first.
		uint32_t Truncated = 0;

		bool operator==(VisualScriptStopValues const& other) const = default;
	};

	// A name a condition on a node can read: its form (what a condition writes), type ID, where it comes from (Loop, Local,
	// Parameter, Event or Variable) and the loop or event node it belongs to (0 for others).
	struct VisualScriptConditionName
	{
		std::string Name;
		std::string Form;
		std::string Type;
		std::string Source;
		uint32_t Node = 0;

		bool operator==(VisualScriptConditionName const& other) const = default;
	};

	// A condition checked against a node: no Error when it compiles; Reads names what it reads, Scope every readable name.
	// Checked: a loaded program compiles the document (VisualScriptDebugger::CheckCondition refuses an unchecked one).
	struct VisualScriptConditionCheck
	{
		bool Checked = false;
		std::optional<std::string> Error;
		std::vector<std::string> Reads;
		std::vector<VisualScriptConditionName> Scope;

		bool operator==(VisualScriptConditionCheck const& other) const = default;
	};

	// Where an instance's live values come from: its event graph's frame, the last call of a watched function, or nowhere.
	enum class VisualScriptValueSource : uint8_t
	{
		Frame = 0,
		LastCall,
		None
	};

	struct VisualScriptTimelineState
	{
		uint32_t Node = 0;
		double Time = 0.0;
		double Length = 0.0;
		bool Playing = false;
		bool Reverse = false;

		bool operator==(VisualScriptTimelineState const& other) const = default;
	};

	// The live values of an instance to read: a function's frame (0: the event graph's), the outputs of these nodes, every
	// variable and the timelines.
	struct VisualScriptValueRequest
	{
		UUID Entity = UUID::Invalid();
		uint32_t Function = 0;
		std::vector<uint32_t> Nodes;
		bool Variables = false;
		bool Timelines = false;
	};

	struct VisualScriptLiveValues
	{
		// The instance whose frame the values are from (for a watched function's last call, the caller's).
		UUID Entity = UUID::Invalid();
		AssetHandle Graph;
		VisualScriptValueSource Source = VisualScriptValueSource::None;
		std::vector<VisualScriptDebugValue> Variables;
		std::vector<VisualScriptTimelineState> Timelines;
		std::vector<VisualScriptNodeValues> Nodes;
		uint32_t Truncated = 0;

		bool operator==(VisualScriptLiveValues const& other) const = default;
	};

	enum class VisualScriptInstanceState : uint8_t
	{
		Idle = 0,
		Waiting,
		Stopped
	};

	// What a fiber waits for: its latent node, the node's kind (Delay, RetriggerableDelay, WaitFrames, WaitUntil, Tween,
	// Interval, Timeline) and, as the kind has them, the seconds left, the updates left and the next tick's index.
	struct VisualScriptInstanceWait
	{
		AssetHandle Document;
		uint32_t Function = 0;
		uint32_t Node = 0;
		std::string Title;
		std::string Kind;
		std::optional<double> Remaining;
		std::optional<uint32_t> Frames;
		std::optional<int64_t> Index;

		bool operator==(VisualScriptInstanceWait const& other) const = default;
	};

	struct VisualScriptInstanceInfo
	{
		UUID Entity = UUID::Invalid();
		AssetHandle Graph;
		VisualScriptInstanceState State = VisualScriptInstanceState::Idle;
		std::vector<VisualScriptInstanceWait> Waits;
		std::vector<uint64_t> Stops;

		bool operator==(VisualScriptInstanceInfo const& other) const = default;
	};

	// A page of the instances (of one graph, or of one entity), in the order they were created; Limit 1 to 500.
	struct VisualScriptInstanceQuery
	{
		std::optional<AssetHandle> Graph;
		std::optional<UUID> Entity;
		uint32_t Offset = 0;
		uint32_t Limit = 100;
	};

	struct VisualScriptInstanceList
	{
		uint32_t Total = 0;
		std::vector<VisualScriptInstanceInfo> Instances;

		bool operator==(VisualScriptInstanceList const& other) const = default;
	};

	// How often the flow left a node through an exit (Exit empty: it ran without one) since the last take, and the order in
	// which the take first saw it.
	struct VisualScriptTraceStep
	{
		uint32_t Node = 0;
		std::string Exit;
		uint32_t Count = 0;
		uint32_t FirstOrder = 0;

		bool operator==(VisualScriptTraceStep const& other) const = default;
	};

	// The flow of one watched sub-graph: the document holding its nodes (a script graph or a library) and its function.
	struct VisualScriptGraphTrace
	{
		AssetHandle Document;
		uint32_t Function = 0;
		std::vector<VisualScriptTraceStep> Steps;

		bool operator==(VisualScriptGraphTrace const& other) const = default;
	};

	struct VisualScriptFrameTrace
	{
		uint64_t StopRevision = 0;
		// Steps not counted: a graph's or the take's entries ran out.
		uint32_t Dropped = 0;
		std::vector<VisualScriptGraphTrace> Graphs;

		bool operator==(VisualScriptFrameTrace const& other) const = default;
	};

	// What a recording keeps: the steps of one instance (none: every instance), of these documents (none: every document) and
	// of these nodes of them (none: every node; needs Documents, since node IDs repeat across graphs), with the values of
	// impure nodes' staged inputs and of outputs; a summary per node and exit and per pin instead of the steps; caps of the
	// listed steps and values (1 to 100,000; a summary has its own).
	struct VisualScriptRecordingOptions
	{
		std::optional<UUID> Entity;
		std::vector<AssetHandle> Documents;
		std::vector<uint32_t> Nodes;
		bool Values = true;
		bool Summary = true;
		uint32_t MaxSteps = 500;
		uint32_t MaxValues = 500;
	};

	struct VisualScriptRecordedStep
	{
		uint32_t Index = 0;
		uint32_t Frame = 0;
		uint64_t Execution = 0;
		UUID Entity = UUID::Invalid();
		AssetHandle Graph;
		AssetHandle Document;
		uint32_t Function = 0;
		uint32_t Node = 0;
		std::string Title;
		// Empty when the step took no exit.
		std::string Exit;

		bool operator==(VisualScriptRecordedStep const& other) const = default;
	};

	// A value a step saw: a staged input (Input) or an output of a node; Value has its Type and no Name.
	struct VisualScriptRecordedValue
	{
		uint32_t Step = 0;
		uint32_t Node = 0;
		std::string Pin;
		bool Input = false;
		VisualScriptDebugValue Value;

		bool operator==(VisualScriptRecordedValue const& other) const = default;
	};

	struct VisualScriptStepSummary
	{
		uint32_t Node = 0;
		std::string Title;
		std::string Exit;
		uint32_t Count = 0;
		uint32_t FirstFrame = 0;
		uint32_t LastFrame = 0;

		bool operator==(VisualScriptStepSummary const& other) const = default;
	};

	// The values of one pin over a recording: how many, the first and the last, and for numbers the smallest and the largest
	// (values without Type: Type is the summary's).
	struct VisualScriptValueSummary
	{
		uint32_t Node = 0;
		std::string Pin;
		bool Input = false;
		std::string Type;
		uint32_t Count = 0;
		VisualScriptDebugValue First;
		VisualScriptDebugValue Last;
		std::optional<VisualScriptDebugValue> Min;
		std::optional<VisualScriptDebugValue> Max;

		bool operator==(VisualScriptValueSummary const& other) const = default;
	};

	struct VisualScriptGraphSummary
	{
		AssetHandle Graph;
		AssetHandle Document;
		uint32_t Function = 0;
		std::vector<VisualScriptStepSummary> Steps;
		std::vector<VisualScriptValueSummary> Values;

		bool operator==(VisualScriptGraphSummary const& other) const = default;
	};

	// What a recording kept: the steps and values in order, or the summary; what the caps left out.
	struct VisualScriptRecording
	{
		std::vector<VisualScriptRecordedStep> Steps;
		std::vector<VisualScriptRecordedValue> Values;
		std::optional<std::vector<VisualScriptGraphSummary>> Summary;
		uint32_t TruncatedSteps = 0;
		uint32_t TruncatedValues = 0;

		bool operator==(VisualScriptRecording const& other) const = default;
	};

	// The visual scripts' debugger in the scripting runtime: breakpoints, stops and steps, traces and values. Only the editor
	// attaches it; the player and exported games never do, and their graphs run untraced. Available while
	// ScriptEngine::IsInitialized(); its state goes with ScriptEngine::Shutdown. Main thread only, between frames (the runtime
	// refuses to resume or detach while an execution runs). Every member that reaches the runtime answers an error when the
	// runtime is not initialized, when the runtime refuses the request ("<Code>: <message>", Code one of InvalidRequest,
	// NotAttached, NotFound and InvalidState) and when its reply cannot be read.
	class VisualScriptDebugger
	{
	public:
		// Attaching again changes nothing.
		[[nodiscard]] static Result<void> Attach();
		// Releases every stopped execution without running it (their count is answered) and drops what the debugger held;
		// no graph code runs. Nothing (0) when none is attached.
		static uint32_t Detach();
		static bool IsAttached();
		[[nodiscard]] static Result<void> Configure(VisualScriptDebugConfiguration const& configuration);
		// What ran in watched sub-graphs since the last take, and the stop revision; cheap while nothing ran.
		[[nodiscard]] static Result<VisualScriptFrameTrace> TakeTrace();
		[[nodiscard]] static Result<VisualScriptDebugState> GetState();
		// Runs graph code between frames: continues or steps a stop (none: the current one), or with all continues every
		// stop. An aborting error the runtime logged is reported as a script exception here (Failed).
		[[nodiscard]] static Result<VisualScriptResumeOutcome> Resume(std::optional<uint64_t> stop, VisualScriptDebugAction action,
		                                                              bool all = false);
		// Stops the next stop point of a targeted execution that can stop, with a document only those of that graph's
		// executions or at that library's nodes; cancel cancels the armed Break and the step.
		[[nodiscard]] static Result<void> Break(bool cancel, std::optional<AssetHandle> document = std::nullopt);
		// Arms a one-shot breakpoint on a node of a document; the caller then continues.
		[[nodiscard]] static Result<void> RunTo(AssetHandle document, uint32_t node);
		// One frame of a stop (0: the innermost; frames numbered as the stop's Frames, a Native frame answers an error), with
		// the formulas evaluated read-only in the frame's scope (at most 16).
		[[nodiscard]] static Result<VisualScriptStopValues> DescribeStop(uint64_t stop, uint32_t frame,
		                                                                 std::span<std::string const> evaluate = {});
		// These three work without the debugger (My Script's values, the Instance combo, condition checks).
		[[nodiscard]] static Result<VisualScriptLiveValues> ReadValues(VisualScriptValueRequest const& request);
		[[nodiscard]] static Result<VisualScriptInstanceList> DescribeInstances(VisualScriptInstanceQuery const& query);
		// The check of a condition on a node and the node's scope (an empty condition only lists the scope); an error naming
		// "checked when the graph runs" when no loaded program compiles the document.
		[[nodiscard]] static Result<VisualScriptConditionCheck> CheckCondition(AssetHandle document, uint32_t node,
		                                                                       std::string_view condition);
		// graph.trace's recording: starting replaces one that runs; the frame index the following steps belong to; stopping
		// answers what it kept.
		[[nodiscard]] static Result<void> StartRecording(VisualScriptRecordingOptions const& options);
		[[nodiscard]] static Result<void> MarkRecordingFrame(uint32_t frame);
		[[nodiscard]] static Result<VisualScriptRecording> StopRecording();
	};

	namespace ScriptEngineInternal
	{
		// The reader of the trace block VisualScriptTakeTrace answers, exposed for its fuzz test: refuses a damaged block,
		// naming what is wrong. Pure; any thread.
		[[nodiscard]] Result<VisualScriptFrameTrace> ReadVisualScriptFrameTrace(std::span<std::byte const> block);
		// The readers of the VisualScriptDebug replies, exposed for their fuzz test: each refuses a reply not of its form,
		// naming the first member that is not (its JSON path), and otherwise answers well-formed structures. Members the form
		// does not name are ignored. Error replies ({"Error", "Code"}) are refused as "<Code>: <message>". Pure; any thread.
		[[nodiscard]] Result<VisualScriptDebugState> ReadVisualScriptDebugState(std::string_view reply);
		[[nodiscard]] Result<VisualScriptResumeOutcome> ReadVisualScriptResumeOutcome(std::string_view reply);
		[[nodiscard]] Result<VisualScriptStopValues> ReadVisualScriptStopValues(std::string_view reply);
		[[nodiscard]] Result<VisualScriptLiveValues> ReadVisualScriptLiveValues(std::string_view reply);
		[[nodiscard]] Result<VisualScriptInstanceList> ReadVisualScriptInstanceList(std::string_view reply);
		[[nodiscard]] Result<VisualScriptConditionCheck> ReadVisualScriptConditionCheck(std::string_view reply);
		[[nodiscard]] Result<VisualScriptRecording> ReadVisualScriptRecording(std::string_view reply);
	}
}
