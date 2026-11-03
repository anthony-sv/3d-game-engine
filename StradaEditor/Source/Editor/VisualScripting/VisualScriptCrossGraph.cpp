#include "Editor/VisualScripting/VisualScriptCrossGraph.h"

#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <optional>
#include <set>
#include <unordered_set>

namespace Strada
{
	namespace VisualScriptEditing
	{
		namespace
		{
			using Format = VisualScriptSerializer;

			constexpr std::string_view SendType = "Event.Send";
			constexpr std::string_view BroadcastType = "Event.Broadcast";
			constexpr std::string_view CustomEventType = "Event.Custom";
			constexpr std::string_view CallType = "Function.Call";
			constexpr std::string_view GetType = "Variable.Get";
			constexpr std::string_view SetType = "Variable.Set";
			constexpr std::string_view RunsGraphType = "RunsGraph";
			constexpr std::string_view EntityType = "Strada.Entity";
			constexpr char const* NameSetting = "Name";
			constexpr char const* ParametersSetting = "Parameters";
			constexpr char const* FunctionSetting = "Function";
			constexpr char const* VariableSetting = "Variable";

			bool IsCrossGraphType(std::string_view type)
			{
				return type == SendType || type == BroadcastType || type == CallType || type == GetType || type == SetType ||
				       type == RunsGraphType;
			}

			bool IsSender(std::string_view type)
			{
				return type == SendType || type == BroadcastType;
			}

			VisualScriptPin MakePin(std::string name, VisualScriptPinDirection direction, std::string type)
			{
				VisualScriptPin pin;
				pin.Name = std::move(name);
				pin.Direction = direction;
				pin.Type = std::move(type);
				return pin;
			}

			VisualScriptPin MakeExecPin(std::string name, VisualScriptPinDirection direction)
			{
				return MakePin(std::move(name), direction, std::string(VisualScriptExecType));
			}

			VisualScriptPin MakeTargetPin()
			{
				VisualScriptPin pin = MakePin("Target", VisualScriptPinDirection::In, std::string(EntityType));
				pin.DefaultsToSelf = true;
				return pin;
			}

			// An ID setting as the runtime reads it: an integer from 1 to the format's largest ID.
			std::optional<uint32_t> ReadId(Json const& settings, char const* key)
			{
				if (!settings.is_object())
				{
					return std::nullopt;
				}
				auto const found = settings.find(key);
				if (found == settings.end() || !found->is_number_integer())
				{
					return std::nullopt;
				}
				if (found->is_number_unsigned())
				{
					uint64_t const value = found->get<uint64_t>();
					return value >= 1 && value <= Format::MaxID ? std::optional<uint32_t>(static_cast<uint32_t>(value)) : std::nullopt;
				}
				int64_t const value = found->get<int64_t>();
				return value >= 1 && value <= static_cast<int64_t>(Format::MaxID) ? std::optional<uint32_t>(static_cast<uint32_t>(value))
				                                                                  : std::nullopt;
			}

			std::string const* ReadString(Json const& settings, char const* key)
			{
				if (!settings.is_object())
				{
					return nullptr;
				}
				auto const found = settings.find(key);
				return found != settings.end() && found->is_string() ? &found->get_ref<std::string const&>() : nullptr;
			}

			// An event's Parameters as (name, type) pairs: none for a setting that is not a list of { Name, Type } strings, an
			// empty list without the setting.
			std::optional<std::vector<std::pair<std::string, std::string>>> ReadParameters(VisualScriptNode const& node)
			{
				std::vector<std::pair<std::string, std::string>> parameters;
				if (!node.Settings.is_object())
				{
					return std::nullopt;
				}
				auto const found = node.Settings.find(ParametersSetting);
				if (found == node.Settings.end())
				{
					return parameters;
				}
				if (!found->is_array())
				{
					return std::nullopt;
				}
				for (Json const& entry : *found)
				{
					std::string const* const name = ReadString(entry, "Name");
					std::string const* const type = ReadString(entry, "Type");
					if (name == nullptr || type == nullptr)
					{
						return std::nullopt;
					}
					parameters.emplace_back(*name, *type);
				}
				return parameters;
			}

			template<typename T>
			T const* FindById(std::vector<T> const& elements, uint32_t id)
			{
				auto const found = std::find_if(elements.begin(), elements.end(),
				                                [id](T const& element)
				                                {
													return element.ID == id;
												});
				return found != elements.end() ? &*found : nullptr;
			}

			// The custom event of the name in the document's event graph that receives it: the enabled one, else the first.
			VisualScriptNode const* FindCustomEvent(VisualScriptDocument const& document, std::string_view name)
			{
				VisualScriptNode const* receiver = nullptr;
				for (VisualScriptNode const& node : document.Graph.Nodes)
				{
					std::string const* const eventName = node.Type == CustomEventType ? ReadString(node.Settings, NameSetting) : nullptr;
					if (eventName == nullptr || *eventName != name)
					{
						continue;
					}
					if (receiver == nullptr ||
					    (receiver->Enabled == VisualScriptNodeEnabled::Disabled && node.Enabled != VisualScriptNodeEnabled::Disabled))
					{
						receiver = &node;
					}
				}
				return receiver;
			}

			std::set<std::string> GetCustomEventNames(VisualScriptDocument const& document)
			{
				std::set<std::string> names;
				for (VisualScriptNode const& node : document.Graph.Nodes)
				{
					if (std::string const* const name = node.Type == CustomEventType ? ReadString(node.Settings, NameSetting) : nullptr)
					{
						names.insert(*name);
					}
				}
				return names;
			}

			// The parameter of a function a pin name means: the one of that name, else the one that had it (FormerNames).
			VisualScriptParameter const* FindParameter(std::vector<VisualScriptParameter> const& parameters, std::string_view name)
			{
				auto const named = std::find_if(parameters.begin(), parameters.end(),
				                                [name](VisualScriptParameter const& parameter)
				                                {
													return parameter.Name == name;
												});
				if (named != parameters.end())
				{
					return &*named;
				}
				auto const former = std::find_if(parameters.begin(), parameters.end(),
				                                 [name](VisualScriptParameter const& parameter)
				                                 {
													 return std::find(parameter.FormerNames.begin(), parameter.FormerNames.end(), name) !=
					                                        parameter.FormerNames.end();
												 });
				return former != parameters.end() ? &*former : nullptr;
			}

			// The pin a pin path belongs to: "Hit" of "Hit.Point".
			std::string_view GetRootPin(std::string_view path)
			{
				return path.substr(0, path.find('.'));
			}

			// The pins of a node its graph links, its literals and its split pins use, by direction, each once in name order.
			std::pair<std::set<std::string>, std::set<std::string>> GetUsedPins(VisualScriptGraph const& graph,
			                                                                    VisualScriptNode const& node)
			{
				std::set<std::string> inputs;
				std::set<std::string> outputs;
				for (VisualScriptLink const& link : graph.Links)
				{
					if (link.To.Node == node.ID)
					{
						inputs.emplace(GetRootPin(link.To.Pin));
					}
					if (link.From.Node == node.ID)
					{
						outputs.emplace(GetRootPin(link.From.Pin));
					}
				}
				if (node.Inputs.is_object())
				{
					for (auto const& [pin, value] : node.Inputs.items())
					{
						inputs.emplace(GetRootPin(pin));
					}
				}
				for (std::string const& path : node.Split.Inputs)
				{
					inputs.emplace(GetRootPin(path));
				}
				for (std::string const& path : node.Split.Outputs)
				{
					outputs.emplace(GetRootPin(path));
				}
				return {std::move(inputs), std::move(outputs)};
			}

			std::string DescribeTypes(std::vector<std::pair<std::string, std::string>> const& parameters)
			{
				std::string text;
				for (auto const& [name, type] : parameters)
				{
					text += text.empty() ? type : ", " + type;
				}
				return text;
			}

			bool HaveSameTypes(std::vector<std::pair<std::string, std::string>> const& a,
			                   std::vector<std::pair<std::string, std::string>> const& b)
			{
				return std::equal(a.begin(), a.end(), b.begin(), b.end(),
				                  [](auto const& first, auto const& second)
				                  {
									  return first.second == second.second;
								  });
			}

			bool HaveSameSignature(VisualScriptFunction const& a, VisualScriptFunction const& b)
			{
				return a.ID == b.ID && a.Pure == b.Pure && a.Inputs == b.Inputs && a.Outputs == b.Outputs;
			}

			struct Problem
			{
				VisualScriptReferrerProblem Kind = VisualScriptReferrerProblem::Removed;
				std::string Message;
			};

			// A call of one of the graph's functions, which worked when the graph was a library with the function.
			std::optional<Problem> CheckCall(VisualScriptGraph const& graph, VisualScriptNode const& node,
			                                 VisualScriptDocument const& before, VisualScriptDocument const& after)
			{
				std::optional<uint32_t> const id = ReadId(node.Settings, FunctionSetting);
				VisualScriptFunction const* const was =
					id && before.Kind == VisualScriptDocumentKind::Library ? FindById(before.Functions, *id) : nullptr;
				if (was == nullptr)
				{
					return std::nullopt;
				}
				std::string const subject = fmt::format("{} (node {})", was->Name, node.ID);
				if (after.Kind != VisualScriptDocumentKind::Library)
				{
					return Problem{VisualScriptReferrerProblem::Removed,
					               fmt::format("{} calls {}, and its graph is no function library now", subject, was->Name)};
				}
				VisualScriptFunction const* const is = FindById(after.Functions, *id);
				if (is == nullptr)
				{
					return Problem{VisualScriptReferrerProblem::Removed, fmt::format("{} calls {}, which is gone", subject, was->Name)};
				}
				if (is->Pure != was->Pure)
				{
					return Problem{VisualScriptReferrerProblem::Changed,
					               fmt::format("{} calls {}, which is {} now", subject, is->Name, is->Pure ? "pure" : "not pure")};
				}
				auto const [inputs, outputs] = GetUsedPins(graph, node);
				auto const check = [&](std::set<std::string> const& pins, bool input) -> std::optional<Problem>
				{
					std::vector<VisualScriptParameter> const& wasParameters = input ? was->Inputs : was->Outputs;
					std::vector<VisualScriptParameter> const& isParameters = input ? is->Inputs : is->Outputs;
					char const* const role = input ? "input" : "output";
					for (std::string const& pin : pins)
					{
						VisualScriptParameter const* const old = FindParameter(wasParameters, pin);
						if (old == nullptr)
						{
							continue;
						}
						VisualScriptParameter const* const now = FindParameter(isParameters, pin);
						if (now == nullptr)
						{
							return Problem{VisualScriptReferrerProblem::Changed,
							               fmt::format("{} uses the {} {}, which {} no longer has", subject, role, pin, is->Name)};
						}
						if (now->Type != old->Type)
						{
							return Problem{
								VisualScriptReferrerProblem::Changed,
								fmt::format("{} uses the {} {}, whose type is {} now (was {})", subject, role, pin, now->Type, old->Type)};
						}
					}
					return std::nullopt;
				};
				if (std::optional<Problem> problem = check(inputs, true))
				{
					return problem;
				}
				return check(outputs, false);
			}

			// A Get or Set of one of the graph's variables, which worked while the graph was a script with the variable public.
			std::optional<Problem> CheckVariable(VisualScriptNode const& node, VisualScriptDocument const& before,
			                                     VisualScriptDocument const& after)
			{
				std::optional<uint32_t> const id = ReadId(node.Settings, VariableSetting);
				VisualScriptVariable const* const was =
					id && before.Kind == VisualScriptDocumentKind::Script ? FindById(before.Variables, *id) : nullptr;
				if (was == nullptr || !was->Public)
				{
					return std::nullopt;
				}
				bool const set = node.Type == SetType;
				std::string const subject =
					fmt::format("{} {} (node {}) {} {}", set ? "Set" : "Get", was->Name, node.ID, set ? "writes" : "reads", was->Name);
				if (after.Kind != VisualScriptDocumentKind::Script)
				{
					return Problem{VisualScriptReferrerProblem::Removed, subject + ", and its graph is a function library now"};
				}
				VisualScriptVariable const* const is = FindById(after.Variables, *id);
				if (is == nullptr)
				{
					return Problem{VisualScriptReferrerProblem::Removed, subject + ", which is gone"};
				}
				if (!is->Public)
				{
					return Problem{VisualScriptReferrerProblem::Private, subject + ", which is private now"};
				}
				if (is->Type != was->Type)
				{
					return Problem{VisualScriptReferrerProblem::Changed,
					               fmt::format("{}, whose type is {} now (was {})", subject, is->Type, was->Type)};
				}
				return std::nullopt;
			}

			// A Send or Broadcast of one of the graph's custom events, which worked while the graph was a script with the event.
			std::optional<Problem> CheckTypedSender(VisualScriptNode const& node, VisualScriptDocument const& before,
			                                        VisualScriptDocument const& after)
			{
				std::string const* const name = ReadString(node.Settings, NameSetting);
				VisualScriptNode const* const was =
					name != nullptr && before.Kind == VisualScriptDocumentKind::Script ? FindCustomEvent(before, *name) : nullptr;
				if (was == nullptr)
				{
					return std::nullopt;
				}
				std::string const subject =
					fmt::format("{} {} (node {}) sends {}", node.Type == SendType ? "Send" : "Broadcast", *name, node.ID, *name);
				if (after.Kind != VisualScriptDocumentKind::Script)
				{
					return Problem{VisualScriptReferrerProblem::Removed, subject + ", and its graph is a function library now"};
				}
				VisualScriptNode const* const is = FindCustomEvent(after, *name);
				if (is == nullptr)
				{
					return Problem{VisualScriptReferrerProblem::Removed, subject + ", which its graph no longer has"};
				}
				std::optional<std::vector<std::pair<std::string, std::string>>> const wasParameters = ReadParameters(*was);
				std::optional<std::vector<std::pair<std::string, std::string>>> const isParameters = ReadParameters(*is);
				if (wasParameters && isParameters && !HaveSameTypes(*wasParameters, *isParameters))
				{
					return Problem{VisualScriptReferrerProblem::Changed,
					               fmt::format("{}, which takes ({}) now (took ({}))", subject, DescribeTypes(*isParameters),
					                           DescribeTypes(*wasParameters))};
				}
				return std::nullopt;
			}

			std::optional<Problem> CheckRunsGraph(VisualScriptNode const& node, VisualScriptDocument const& before,
			                                      VisualScriptDocument const& after)
			{
				if (before.Kind != VisualScriptDocumentKind::Script || after.Kind == VisualScriptDocumentKind::Script)
				{
					return std::nullopt;
				}
				return Problem{
					VisualScriptReferrerProblem::Removed,
					fmt::format("Runs Graph (node {}) asks whether an entity runs its graph, which is a function library now", node.ID)};
			}
		}

		std::vector<VisualScriptPin> MakeVisualScriptCrossGraphPins(VisualScriptNode const& node, VisualScriptDocument const& target)
		{
			std::vector<VisualScriptPin> pins;
			if (node.Type == RunsGraphType)
			{
				pins.push_back(MakeTargetPin());
				pins.push_back(MakePin("Result", VisualScriptPinDirection::Out, "bool"));
				return pins;
			}
			if (node.Type == CallType)
			{
				std::optional<uint32_t> const id = ReadId(node.Settings, FunctionSetting);
				VisualScriptFunction const* const function =
					id && target.Kind == VisualScriptDocumentKind::Library ? FindById(target.Functions, *id) : nullptr;
				if (function == nullptr)
				{
					return pins;
				}
				if (!function->Pure)
				{
					pins.push_back(MakeExecPin("Execute", VisualScriptPinDirection::In));
				}
				for (VisualScriptParameter const& input : function->Inputs)
				{
					pins.push_back(MakePin(input.Name, VisualScriptPinDirection::In, input.Type));
				}
				if (!function->Pure)
				{
					pins.push_back(MakeExecPin("Then", VisualScriptPinDirection::Out));
				}
				for (VisualScriptParameter const& output : function->Outputs)
				{
					pins.push_back(MakePin(output.Name, VisualScriptPinDirection::Out, output.Type));
				}
				return pins;
			}
			if (node.Type == GetType || node.Type == SetType)
			{
				std::optional<uint32_t> const id = ReadId(node.Settings, VariableSetting);
				VisualScriptVariable const* const variable =
					id && target.Kind == VisualScriptDocumentKind::Script ? FindById(target.Variables, *id) : nullptr;
				if (variable == nullptr)
				{
					return pins;
				}
				bool const set = node.Type == SetType;
				if (set)
				{
					pins.push_back(MakeExecPin("Execute", VisualScriptPinDirection::In));
				}
				pins.push_back(MakeTargetPin());
				if (set)
				{
					pins.push_back(MakePin("Value", VisualScriptPinDirection::In, variable->Type));
					pins.push_back(MakeExecPin("Then", VisualScriptPinDirection::Out));
				}
				pins.push_back(MakePin("Value", VisualScriptPinDirection::Out, variable->Type));
				return pins;
			}
			if (IsSender(node.Type))
			{
				std::string const* const name = ReadString(node.Settings, NameSetting);
				VisualScriptNode const* const event =
					name != nullptr && target.Kind == VisualScriptDocumentKind::Script ? FindCustomEvent(target, *name) : nullptr;
				std::optional<std::vector<std::pair<std::string, std::string>>> const parameters =
					event != nullptr ? ReadParameters(*event) : std::nullopt;
				if (!parameters)
				{
					return pins;
				}
				pins.push_back(MakeExecPin("Execute", VisualScriptPinDirection::In));
				if (node.Type == SendType)
				{
					pins.push_back(MakeTargetPin());
				}
				for (auto const& [parameter, type] : *parameters)
				{
					pins.push_back(MakePin(parameter, VisualScriptPinDirection::In, type));
				}
				pins.push_back(MakeExecPin("Then", VisualScriptPinDirection::Out));
				if (node.Type == BroadcastType)
				{
					pins.push_back(MakePin("Count", VisualScriptPinDirection::Out, "int"));
				}
			}
			return pins;
		}

		Result<uint32_t> RetargetNode(VisualScriptDocument& document, uint32_t node, Json settings,
		                              std::span<VisualScriptPin const> newPins)
		{
			VisualScriptGraph* graph = nullptr;
			VisualScriptNode* target = nullptr;
			auto const find = [&](VisualScriptGraph& candidate)
			{
				auto const found = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(),
				                                [node](VisualScriptNode const& element)
				                                {
													return element.ID == node;
												});
				if (found != candidate.Nodes.end())
				{
					graph = &candidate;
					target = &*found;
				}
			};
			find(document.Graph);
			for (VisualScriptFunction& function : document.Functions)
			{
				if (target == nullptr)
				{
					find(function.Graph);
				}
			}
			if (target == nullptr)
			{
				return MakeError("the document has no node {}", node);
			}
			if (!IsCrossGraphType(target->Type))
			{
				return MakeError("node {} ({}) names no other graph: only Event.Send, Event.Broadcast, Function.Call, Variable.Get, "
				                 "Variable.Set and RunsGraph do",
				                 node, target->Type);
			}
			if (!target->Settings.is_object() || !target->Inputs.is_object())
			{
				return MakeError("node {} has Settings or Inputs that are not objects", node);
			}
			if (!settings.is_object())
			{
				return Error{"the settings must be an object: each member is set, a null member removed"};
			}
			Json updated = target->Settings;
			for (auto const& [name, value] : settings.items())
			{
				if (!Format::IsValidText(name, Format::MaxPinNameBytes))
				{
					return MakeError("{}: expected a setting name of 1 to {} bytes without control characters",
					                 Json(name).dump(-1, ' ', false, Json::error_handler_t::replace), Format::MaxPinNameBytes);
				}
				if (value.is_null())
				{
					updated.erase(name);
					continue;
				}
				if (!Format::IsValidValue(value))
				{
					return MakeError("the setting {} holds a value the format does not take (too deep, too long or not finite)", name);
				}
				updated[name] = value;
			}
			if (updated.size() > Format::MaxSettingsMembers)
			{
				return MakeError("a node holds at most {} settings", Format::MaxSettingsMembers);
			}

			auto const keeps = [newPins](std::string_view path, VisualScriptPinDirection direction)
			{
				std::string_view const root = GetRootPin(path);
				return std::any_of(newPins.begin(), newPins.end(),
				                   [root, direction](VisualScriptPin const& pin)
				                   {
									   return pin.Direction == direction && pin.Name == root;
								   });
			};
			size_t const links = graph->Links.size();
			std::erase_if(graph->Links,
			              [&](VisualScriptLink const& link)
			              {
							  return (link.To.Node == node && !keeps(link.To.Pin, VisualScriptPinDirection::In)) ||
				                     (link.From.Node == node && !keeps(link.From.Pin, VisualScriptPinDirection::Out));
						  });
			uint32_t const removed = static_cast<uint32_t>(links - graph->Links.size());
			Json inputs = Json::object();
			for (auto const& [pin, value] : target->Inputs.items())
			{
				if (keeps(pin, VisualScriptPinDirection::In))
				{
					inputs[pin] = value;
				}
			}
			std::erase_if(target->Split.Inputs,
			              [&](std::string const& path)
			              {
							  return !keeps(path, VisualScriptPinDirection::In);
						  });
			std::erase_if(target->Split.Outputs,
			              [&](std::string const& path)
			              {
							  return !keeps(path, VisualScriptPinDirection::Out);
						  });
			target->Settings = std::move(updated);
			target->Inputs = std::move(inputs);
			return removed;
		}

		Result<void> SetDocumentKind(VisualScriptDocument& document, VisualScriptDocumentKind kind)
		{
			if (VisualScriptDocumentKindToString(kind).empty())
			{
				return MakeError("{} is no kind of visual script: Script or Library", static_cast<uint32_t>(kind));
			}
			document.Kind = kind;
			return {};
		}

		Result<Json> FindVisualScriptEventParameters(VisualScriptDocument const& target, std::string_view name)
		{
			if (VisualScriptNode const* const event = FindCustomEvent(target, name))
			{
				// find answers end() for settings that are not an object too.
				auto const found = event->Settings.find(ParametersSetting);
				return found != event->Settings.end() ? *found : Json::array();
			}
			std::set<std::string> const names = GetCustomEventNames(target);
			if (names.empty())
			{
				return MakeError("the graph has no custom event {}: it has no custom events", name);
			}
			std::string listed;
			for (std::string const& candidate : names)
			{
				listed += listed.empty() ? candidate : ", " + candidate;
			}
			return MakeError("the graph has no custom event {}; its custom events: {}", name, listed);
		}

		std::string DescribeVisualScriptLibraryOnEntity(std::string_view name)
		{
			return fmt::format("{} is a function library: graphs call its functions; it runs on no entity", name);
		}

		std::string_view VisualScriptReferrerProblemToString(VisualScriptReferrerProblem problem)
		{
			switch (problem)
			{
				case VisualScriptReferrerProblem::Removed:
					return "Removed";
				case VisualScriptReferrerProblem::Private:
					return "Private";
				case VisualScriptReferrerProblem::Changed:
					return "Changed";
				case VisualScriptReferrerProblem::Stale:
					return "Stale";
			}
			return {};
		}

		bool HaveSameVisualScriptOffer(VisualScriptDocument const& before, VisualScriptDocument const& after)
		{
			if (before.Kind != after.Kind || !std::equal(before.Functions.begin(), before.Functions.end(), after.Functions.begin(),
			                                             after.Functions.end(), HaveSameSignature))
			{
				return false;
			}
			return HaveSameVisualScriptProgram(MakeVisualScriptInterface(before), MakeVisualScriptInterface(after));
		}

		std::vector<VisualScriptReferrer>
		FindVisualScriptBrokenReferrers(AssetHandle graph, VisualScriptDocument const& before, VisualScriptDocument const& after,
		                                std::span<std::pair<AssetHandle, VisualScriptDocument const*> const> scripts)
		{
			// The custom event names untyped senders may have reached this graph by: those of the script it was, less those of
			// the script it is (a library receives nothing).
			std::set<std::string> gone;
			if (before.Kind == VisualScriptDocumentKind::Script)
			{
				gone = GetCustomEventNames(before);
				if (after.Kind == VisualScriptDocumentKind::Script)
				{
					for (std::string const& name : GetCustomEventNames(after))
					{
						gone.erase(name);
					}
				}
			}

			std::vector<VisualScriptReferrer> referrers;
			std::unordered_set<AssetHandle> visited;
			for (auto const& [handle, document] : scripts)
			{
				if (handle == graph || document == nullptr || !visited.insert(handle).second)
				{
					continue;
				}
				auto const visit = [&](VisualScriptGraph const& nodes)
				{
					for (VisualScriptNode const& node : nodes.Nodes)
					{
						std::optional<Problem> problem;
						if (std::optional<AssetHandle> const named = ReadVisualScriptGraphSetting(node); named && *named == graph)
						{
							if (node.Type == CallType)
							{
								problem = CheckCall(nodes, node, before, after);
							}
							else if (node.Type == GetType || node.Type == SetType)
							{
								problem = CheckVariable(node, before, after);
							}
							else if (IsSender(node.Type))
							{
								problem = CheckTypedSender(node, before, after);
							}
							else if (node.Type == RunsGraphType)
							{
								problem = CheckRunsGraph(node, before, after);
							}
						}
						else if (IsSender(node.Type) && IsLocalVisualScriptReference(node))
						{
							std::string const* const name = ReadString(node.Settings, NameSetting);
							if (name != nullptr && gone.contains(*name))
							{
								problem = Problem{VisualScriptReferrerProblem::Stale,
								                  fmt::format("{} {} (node {}) sends {} by name, which the edited graph no longer has: it "
								                              "may have reached that graph and now never does",
								                              node.Type == SendType ? "Send" : "Broadcast", *name, node.ID, *name)};
							}
						}
						if (problem)
						{
							referrers.push_back({handle, node.ID, node.Type, problem->Kind, std::move(problem->Message)});
						}
					}
				};
				visit(document->Graph);
				for (VisualScriptFunction const& function : document->Functions)
				{
					visit(function.Graph);
				}
			}
			std::sort(referrers.begin(), referrers.end(),
			          [](VisualScriptReferrer const& a, VisualScriptReferrer const& b)
			          {
						  uint64_t const first = a.Graph.GetUUID().GetValue();
						  uint64_t const second = b.Graph.GetUUID().GetValue();
						  return first != second ? first < second : a.Node < b.Node;
					  });
			return referrers;
		}
	}
}
