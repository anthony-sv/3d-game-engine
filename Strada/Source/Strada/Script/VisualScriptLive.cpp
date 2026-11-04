#include "stpch.h"
#include "Strada/Script/VisualScriptLive.h"

#include "Strada/Script/ReplyReader.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <unordered_map>

namespace Strada
{
	namespace
	{
		using ScriptEngineInternal::ReplyReader;

		// The scripting runtime's outcomes, in the order of the engine's first three (Kept is read as KeptLastWorking).
		constexpr std::array<std::string_view, 3> OutcomeNames = {"Swapped", "Pending", "Kept"};
		constexpr std::array<std::string_view, 2> SeverityNames = {"Error", "Warning"};
		// Why a variable was reset.
		constexpr std::array<std::string_view, 2> ResetReasonNames = {"TypeChanged", "New"};
		// Why a node's state was reset.
		constexpr std::array<std::string_view, 3> StateReasonNames = {"Removed", "Changed", "Disabled"};

		enum class ResetReason : uint8_t
		{
			TypeChanged = 0,
			New
		};

		// A serial: positive, and never after the events' Last.
		uint64_t ReadSerial(ReplyReader& reader, Json const& json, std::string_view path, uint64_t last)
		{
			uint64_t const serial = reader.UInt(json, "Serial", path);
			if (reader.IsOk() && (serial == 0 || serial > last))
			{
				reader.Fail(ReplyReader::Member(path, "Serial"), "not a serial from 1 to Last");
			}
			return serial;
		}

		// The elements of an array member, each read by read(element, path); stops at the first error.
		template<typename TRead>
		void ReadElements(ReplyReader& reader, Json const& object, std::string_view name, std::string_view path, TRead&& read)
		{
			Json const* const array = reader.Array(object, name, path);
			if (array == nullptr)
			{
				return;
			}
			std::string const member = ReplyReader::Member(path, name);
			for (size_t index = 0; index < array->size() && reader.IsOk(); index++)
			{
				read((*array)[index], ReplyReader::Element(member, index));
			}
		}

		std::vector<std::string> ReadNames(ReplyReader& reader, Json const& object, std::string_view name, std::string_view path)
		{
			std::vector<std::string> names;
			ReadElements(reader, object, name, path,
			             [&reader, &names](Json const& element, std::string const& elementPath)
			             {
							 names.push_back(reader.String(element, elementPath));
						 });
			return names;
		}

		// A node of a receipt: a reset state (no Stopped, a reason of StateReasonNames) or a cancellation (with Stopped).
		VisualScriptLiveNode ReadNode(ReplyReader& reader, Json const& json, std::string_view path, bool cancelled)
		{
			VisualScriptLiveNode node;
			if (!reader.ExpectObject(json, path))
			{
				return node;
			}
			node.Document = reader.Handle(json, "Document", path);
			node.Function = reader.UInt32(json, "Function", path);
			node.Node = reader.UInt32(json, "Node", path);
			node.Title = reader.String(json, "Title", path);
			if (cancelled)
			{
				node.Stopped = reader.Bool(json, "Stopped", path);
				node.Reason = reader.String(json, "Reason", path);
			}
			else
			{
				uint8_t const reason = reader.Enum<uint8_t>(json, "Reason", path, StateReasonNames);
				node.Reason = std::string(StateReasonNames[reason]);
			}
			return node;
		}

		void ReadVariables(ReplyReader& reader, Json const& json, std::string_view path, VisualScriptLiveEdit& edit)
		{
			if (!reader.ExpectObject(json, path))
			{
				return;
			}
			edit.KeptVariables = ReadNames(reader, json, "Kept", path);
			ReadElements(reader, json, "Matched", path,
			             [&reader, &edit](Json const& element, std::string const& elementPath)
			             {
							 VisualScriptLiveVariable variable;
							 variable.Name = reader.String(element, "Name", elementPath);
							 variable.Detail = reader.String(element, "From", elementPath);
							 edit.MatchedVariables.push_back(std::move(variable));
						 });
			ReadElements(reader, json, "Reset", path,
			             [&reader, &edit](Json const& element, std::string const& elementPath)
			             {
							 VisualScriptLiveVariable variable;
							 variable.Name = reader.String(element, "Name", elementPath);
							 if (reader.Enum<ResetReason>(element, "Reason", elementPath, ResetReasonNames) == ResetReason::TypeChanged)
							 {
								 std::string const from = reader.String(element, "From", elementPath);
								 std::string const to = reader.String(element, "To", elementPath);
								 variable.Detail = fmt::format("type changed from {} to {}", from, to);
							 }
							 else
							 {
								 variable.Detail = "new";
							 }
							 edit.ResetVariables.push_back(std::move(variable));
						 });
			edit.AddedVariables = ReadNames(reader, json, "Added", path);
			edit.RemovedVariables = ReadNames(reader, json, "Removed", path);
		}

		void ReadStates(ReplyReader& reader, Json const& json, std::string_view path, VisualScriptLiveEdit& edit)
		{
			if (!reader.ExpectObject(json, path))
			{
				return;
			}
			edit.KeptStates = reader.UInt32(json, "Kept", path);
			ReadElements(reader, json, "Reset", path,
			             [&reader, &edit](Json const& element, std::string const& elementPath)
			             {
							 edit.ResetStates.push_back(ReadNode(reader, element, elementPath, false));
						 });
		}

		VisualScriptLiveTimeline ReadTimeline(ReplyReader& reader, Json const& json, std::string_view path)
		{
			VisualScriptLiveTimeline timeline;
			if (!reader.ExpectObject(json, path))
			{
				return timeline;
			}
			timeline.Node = reader.UInt32(json, "Node", path);
			timeline.Time = reader.NullableNumber(json, "Time", path).value_or(std::numeric_limits<double>::quiet_NaN());
			timeline.ClampedFrom = reader.NullableNumber(json, "ClampedFrom", path);
			return timeline;
		}

		VisualScriptLiveInstance ReadInstance(ReplyReader& reader, Json const& json, std::string_view path)
		{
			VisualScriptLiveInstance instance;
			if (!reader.ExpectObject(json, path))
			{
				return instance;
			}
			instance.Entity = reader.Entity(json, "Entity", path);
			instance.WaitsKept = reader.UInt32(json, "WaitsKept", path);
			instance.StopsKept = reader.UInt32(json, "StopsKept", path);
			ReadElements(reader, json, "Cancelled", path,
			             [&reader, &instance](Json const& element, std::string const& elementPath)
			             {
							 instance.Cancelled.push_back(ReadNode(reader, element, elementPath, true));
						 });
			ReadElements(reader, json, "Timelines", path,
			             [&reader, &instance](Json const& element, std::string const& elementPath)
			             {
							 instance.Timelines.push_back(ReadTimeline(reader, element, elementPath));
						 });
			return instance;
		}

		VisualScriptLiveEdit ReadEdit(ReplyReader& reader, Json const& json, std::string_view path, uint64_t last)
		{
			VisualScriptLiveEdit edit;
			if (!reader.ExpectObject(json, path))
			{
				return edit;
			}
			edit.Serial = ReadSerial(reader, json, path, last);
			edit.Graph = reader.Handle(json, "Graph", path);
			edit.Name = reader.String(json, "Name", path);
			edit.Outcome = reader.Enum<VisualScriptLiveOutcome>(json, "Outcome", path, OutcomeNames);
			edit.Deferred = reader.Bool(json, "Deferred", path);
			if (reader.IsOk() && edit.Deferred && edit.Outcome != VisualScriptLiveOutcome::Applied)
			{
				reader.Fail(ReplyReader::Member(path, "Deferred"), "true for an outcome other than Swapped");
			}
			edit.Instances = reader.UInt32(json, "Instances", path);
			edit.Pending = reader.UInt32(json, "Pending", path);
			if (Json const* const variables = reader.Require(json, "Variables", path))
			{
				ReadVariables(reader, *variables, ReplyReader::Member(path, "Variables"), edit);
			}
			if (Json const* const states = reader.Require(json, "States", path))
			{
				ReadStates(reader, *states, ReplyReader::Member(path, "States"), edit);
			}
			ReadElements(reader, json, "InstanceNotes", path,
			             [&reader, &edit](Json const& element, std::string const& elementPath)
			             {
							 edit.InstanceNotes.push_back(ReadInstance(reader, element, elementPath));
						 });
			edit.MoreInstances = reader.UInt32(json, "MoreInstances", path);
			edit.Errors = reader.String(json, "Errors", path);
			if (reader.IsOk() && (edit.Outcome == VisualScriptLiveOutcome::KeptLastWorking) == edit.Errors.empty())
			{
				reader.Fail(ReplyReader::Member(path, "Errors"),
				            edit.Errors.empty() ? "empty for Kept" : "not empty for Swapped or Pending");
			}
			return edit;
		}

		VisualScriptRuntimeProblem ReadProblem(ReplyReader& reader, Json const& json, std::string_view path, uint64_t last)
		{
			VisualScriptRuntimeProblem problem;
			if (!reader.ExpectObject(json, path))
			{
				return problem;
			}
			problem.Serial = ReadSerial(reader, json, path, last);
			problem.Severity = reader.Enum<VisualScriptSeverity>(json, "Severity", path, SeverityNames);
			problem.Graph = reader.Handle(json, "Graph", path);
			problem.Document = reader.Handle(json, "Document", path);
			problem.Function = reader.UInt32(json, "Function", path);
			problem.Node = reader.UInt32(json, "Node", path);
			problem.Title = reader.String(json, "Title", path);
			problem.Entity = reader.Entity(json, "Entity", path);
			problem.Message = reader.String(json, "Message", path);
			problem.Count = reader.UInt32(json, "Count", path);
			if (reader.IsOk() && problem.Count == 0)
			{
				reader.Fail(ReplyReader::Member(path, "Count"), "0: a problem happened at least once");
			}
			ReadElements(reader, json, "Calls", path,
			             [&reader, &problem](Json const& element, std::string const& elementPath)
			             {
							 VisualScriptCallFrame call;
							 if (reader.ExpectObject(element, elementPath))
							 {
								 call.Document = reader.Handle(element, "Document", elementPath);
								 call.Function = reader.UInt32(element, "Function", elementPath);
								 call.Node = reader.UInt32(element, "Node", elementPath);
							 }
							 problem.Calls.push_back(call);
						 });
			return problem;
		}

		// Entries of one kind come by serial: each after the one before it.
		template<typename T>
		void CheckAscending(ReplyReader& reader, std::vector<T> const& entries, std::string_view name)
		{
			for (size_t index = 1; index < entries.size() && reader.IsOk(); index++)
			{
				if (entries[index].Serial <= entries[index - 1].Serial)
				{
					reader.Fail(ReplyReader::Member(ReplyReader::Element(name, index), "Serial"), "not after the serial before it");
				}
			}
		}

		// What coalesces runtime problems: the graph, the document, the node, the entity and the severity.
		struct ProblemKey
		{
			uint64_t Graph = 0;
			uint64_t Document = 0;
			uint64_t Entity = 0;
			uint32_t Node = 0;
			VisualScriptSeverity Severity = VisualScriptSeverity::Error;

			bool operator==(ProblemKey const& other) const = default;
		};

		struct ProblemKeyHash
		{
			size_t operator()(ProblemKey const& key) const
			{
				size_t hash = std::hash<uint64_t>{}(key.Graph);
				auto const combine = [&hash](uint64_t value)
				{
					hash ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
				};
				combine(key.Document);
				combine(key.Entity);
				combine((static_cast<uint64_t>(key.Node) << 8) | static_cast<uint64_t>(key.Severity));
				return hash;
			}
		};

		ProblemKey KeyOf(VisualScriptRuntimeProblem const& problem)
		{
			return {problem.Graph.GetUUID().GetValue(), problem.Document.GetUUID().GetValue(), problem.Entity.GetValue(), problem.Node,
			        problem.Severity};
		}
	}

	Result<std::pair<std::vector<VisualScriptLiveEdit>, std::vector<VisualScriptRuntimeProblem>>>
	ParseVisualScriptLiveEvents(std::string_view json, uint64_t& last, uint32_t& dropped)
	{
		Result<Json> parsed = ParseJson(json);
		if (!parsed)
		{
			return MakeError("the live events are not JSON: {}", parsed.GetError());
		}
		Json const& root = parsed.GetValue();
		ReplyReader reader("the live events");
		std::vector<VisualScriptLiveEdit> edits;
		std::vector<VisualScriptRuntimeProblem> problems;
		uint64_t readLast = 0;
		uint32_t readDropped = 0;
		if (reader.ExpectObject(root, ""))
		{
			readLast = reader.UInt(root, "Last", "");
			readDropped = reader.UInt32(root, "Dropped", "");
			ReadElements(reader, root, "Edits", "",
			             [&reader, &edits, readLast](Json const& element, std::string const& elementPath)
			             {
							 edits.push_back(ReadEdit(reader, element, elementPath, readLast));
						 });
			ReadElements(reader, root, "Problems", "",
			             [&reader, &problems, readLast](Json const& element, std::string const& elementPath)
			             {
							 problems.push_back(ReadProblem(reader, element, elementPath, readLast));
						 });
			CheckAscending(reader, edits, "Edits");
			CheckAscending(reader, problems, "Problems");
		}
		if (!reader.IsOk())
		{
			return Error{reader.GetError()};
		}
		last = readLast;
		dropped = readDropped;
		return std::make_pair(std::move(edits), std::move(problems));
	}

	Ref<VisualScriptLiveLog const> AppendVisualScriptLiveEvents(VisualScriptLiveLog const& log, std::vector<VisualScriptLiveEdit> edits,
	                                                            std::vector<VisualScriptRuntimeProblem> problems, uint64_t serial,
	                                                            uint32_t dropped)
	{
		VisualScriptLiveLog next;
		next.Serial = serial;
		next.Dropped = dropped;

		next.Edits = log.Edits;
		next.Edits.reserve(next.Edits.size() + edits.size());
		for (VisualScriptLiveEdit& edit : edits)
		{
			if (edit.Deferred && !edit.Cause.IsValid())
			{
				// The receipt of the swap that a running step held, made when the swap was asked for.
				auto const pending =
					std::find_if(next.Edits.rbegin(), next.Edits.rend(),
				                 [&edit](Ref<VisualScriptLiveEdit const> const& earlier)
				                 {
									 return earlier->Graph == edit.Graph && earlier->Outcome == VisualScriptLiveOutcome::Pending;
								 });
				if (pending != next.Edits.rend())
				{
					edit.Cause = (*pending)->Cause;
				}
			}
			next.Edits.push_back(CreateRef<VisualScriptLiveEdit const>(std::move(edit)));
		}
		if (next.Edits.size() > VisualScriptLiveLog::MaxEdits)
		{
			next.Edits.erase(next.Edits.begin(),
			                 next.Edits.begin() + static_cast<std::ptrdiff_t>(next.Edits.size() - VisualScriptLiveLog::MaxEdits));
		}

		// A problem taken again replaces its entry, which leaves its place: the problems stay in the order of their serials. One
		// take lists a problem once; should one come twice, the later stands.
		std::unordered_map<ProblemKey, size_t, ProblemKeyHash> taken;
		taken.reserve(problems.size());
		for (size_t index = 0; index < problems.size(); index++)
		{
			taken.insert_or_assign(KeyOf(problems[index]), index);
		}
		next.Problems.reserve(log.Problems.size() + taken.size());
		for (Ref<VisualScriptRuntimeProblem const> const& kept : log.Problems)
		{
			auto const replacement = taken.find(KeyOf(*kept));
			if (replacement == taken.end())
			{
				next.Problems.push_back(kept);
			}
			else
			{
				problems[replacement->second].FirstTime = kept->FirstTime;
			}
		}
		for (size_t index = 0; index < problems.size(); index++)
		{
			if (taken.at(KeyOf(problems[index])) == index)
			{
				next.Problems.push_back(CreateRef<VisualScriptRuntimeProblem const>(std::move(problems[index])));
			}
		}
		return CreateRef<VisualScriptLiveLog const>(std::move(next));
	}
}
