#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptCanvasPins.h"
#include "Editor/VisualScripting/VisualScriptCrossGraph.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace Strada
{
	namespace
	{
		// The library pin behind a canvas pin: the analysis' while the canvas draws the node from it, else the template's.
		VisualScriptPin const* FindLibraryPin(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
		                                      VisualScriptNodeVisual const& visual, std::string_view pin,
		                                      VisualScriptPinDirection direction)
		{
			return FindVisualScriptCanvasLibraryPin(model.GetCanvas(), model.GetAnalysis(), &model.GetDisplayDocument(), library, visual,
			                                        pin, direction);
		}
	}

	std::optional<std::pair<VisualScriptElementKind, uint32_t>> UnpackVisualScriptElement(uint64_t packed)
	{
		auto const kind = static_cast<uint32_t>(packed >> 32);
		if (kind > static_cast<uint32_t>(VisualScriptElementKind::CustomEvent))
		{
			return std::nullopt;
		}
		return std::pair(static_cast<VisualScriptElementKind>(kind), static_cast<uint32_t>(packed & 0xFFFFFFFFu));
	}

	VisualScriptNodePins MakeVisualScriptNodePins(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
	                                              std::optional<VisualScriptNewNodePins> newNode)
	{
		return MakeVisualScriptCanvasNodePins(model.GetCanvas(), model.GetAnalysis(), &model.GetDisplayDocument(), library, newNode);
	}

	std::optional<VisualScriptPin> FindVisualScriptCanvasPin(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
	                                                         uint32_t node, std::string_view pin, VisualScriptPinDirection direction)
	{
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		std::optional<uint32_t> const index = canvas.FindNode(node);
		if (!index)
		{
			return std::nullopt;
		}
		std::optional<uint32_t> const pinIndex = canvas.FindPin(*index, pin, direction);
		if (!pinIndex)
		{
			return std::nullopt;
		}
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
		VisualScriptPinVisual const& visual = pins[*pinIndex];
		if (visual.HasFlag(VisualScriptPinFlags::Orphan))
		{
			return std::nullopt;
		}
		VisualScriptPin found;
		if (VisualScriptPin const* const libraryPin = FindLibraryPin(model, library, canvas.GetNodes()[*index], pin, direction))
		{
			found = *libraryPin;
		}
		found.Name = std::string(pin);
		found.Direction = direction;
		found.Type = visual.Type;
		return found;
	}

	VisualScriptGraph const* FindVisualScriptGraph(VisualScriptDocument const& document, uint32_t function)
	{
		if (function == 0)
		{
			return &document.Graph;
		}
		auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                [function](VisualScriptFunction const& candidate)
		                                {
											return candidate.ID == function;
										});
		return found != document.Functions.end() ? &found->Graph : nullptr;
	}

	VisualScriptGraph* FindVisualScriptGraph(VisualScriptDocument& document, uint32_t function)
	{
		if (function == 0)
		{
			return &document.Graph;
		}
		auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                [function](VisualScriptFunction const& candidate)
		                                {
											return candidate.ID == function;
										});
		return found != document.Functions.end() ? &found->Graph : nullptr;
	}

	std::optional<uint32_t> FindVisualScriptElementGraph(VisualScriptDocument const& document, uint32_t element)
	{
		auto const holds = [element](VisualScriptGraph const& graph)
		{
			bool const node = std::any_of(graph.Nodes.begin(), graph.Nodes.end(),
			                              [element](VisualScriptNode const& candidate)
			                              {
											  return candidate.ID == element;
										  });
			return node || std::any_of(graph.Comments.begin(), graph.Comments.end(),
			                           [element](VisualScriptComment const& candidate)
			                           {
										   return candidate.ID == element;
									   });
		};
		if (holds(document.Graph))
		{
			return 0u;
		}
		for (VisualScriptFunction const& function : document.Functions)
		{
			if (holds(function.Graph))
			{
				return function.ID;
			}
		}
		return std::nullopt;
	}

	VisualScriptNode const* FindVisualScriptNode(VisualScriptDocument const& document, uint32_t node)
	{
		auto const find = [node](VisualScriptGraph const& graph) -> VisualScriptNode const*
		{
			auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                                [node](VisualScriptNode const& candidate)
			                                {
												return candidate.ID == node;
											});
			return found != graph.Nodes.end() ? &*found : nullptr;
		};
		if (VisualScriptNode const* const found = find(document.Graph))
		{
			return found;
		}
		for (VisualScriptFunction const& function : document.Functions)
		{
			if (VisualScriptNode const* const found = find(function.Graph))
			{
				return found;
			}
		}
		return nullptr;
	}

	std::string GetVisualScriptNodeTitle(VisualScriptDocument const& document, VisualScriptAnalysis const* analysis,
	                                     VisualScriptNode const& node, VisualScriptGraphResolver const& resolver)
	{
		if (analysis != nullptr)
		{
			VisualScriptAnalyzedNode const* const analyzed = analysis->FindNode(node.ID);
			if (analyzed != nullptr && analyzed->Type == node.Type && !analyzed->Title.empty())
			{
				return analyzed->Title;
			}
		}
		return DeriveVisualScriptNodeTitle(node, document, resolver);
	}

	std::string GetVisualScriptGraphName(VisualScriptDocument const& document, uint32_t function)
	{
		if (function != 0)
		{
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [function](VisualScriptFunction const& candidate)
			                                {
												return candidate.ID == function;
											});
			if (found != document.Functions.end())
			{
				return found->Name;
			}
		}
		return "Event Graph";
	}

	VisualScriptOpenRequest MakeVisualScriptJump(AssetHandle graph, uint32_t function, std::vector<uint32_t> select)
	{
		VisualScriptOpenRequest request;
		request.Graph = graph;
		request.Function = function;
		request.Select = std::move(select);
		request.Frame = request.Select.empty() ? VisualScriptFrame::Keep : VisualScriptFrame::Selection;
		return request;
	}

	void SelectVisualScriptElement(VisualScriptPaneContext& context, VisualScriptElement element, std::optional<uint32_t> show)
	{
		if (show)
		{
			context.Model.ShowFunction(*show);
		}
		context.Shared.Element = element;
		context.Model.Select({}, VisualScriptSelectMode::Replace);
	}

	uint64_t MakeVisualScriptPaneNameMergeKey(VisualScriptPaneShared& shared)
	{
		// The lower 31 bits count; the counter wraps long before two runs in a row could share a key.
		shared.NameRuns = (shared.NameRuns + 1) & 0x7FFFFFFFu;
		return VisualScriptPaneNameMergeKey | shared.NameRuns;
	}

	std::string GetVisualScriptNameRuleMessage()
	{
		return fmt::format("expected a name of 1 to {} bytes without control characters, '.' or a leading or trailing space",
		                   VisualScriptSerializer::MaxNameBytes);
	}

	VisualScriptEnumLookup MakeVisualScriptEnumLookup(VisualScriptLibrary const* library)
	{
		if (library == nullptr)
		{
			return {};
		}
		return [library](std::string_view type)
		{
			return library->FindEnum(type);
		};
	}

	std::string DescribeVisualScriptType(VisualScriptLibrary const* library, std::string_view type)
	{
		if (library != nullptr)
		{
			if (VisualScriptTypeInfo const* const info = library->FindType(type); info != nullptr && !info->Title.empty())
			{
				return info->Title;
			}
		}
		return std::string(type);
	}

	bool IsVisualScriptPublicType(std::string_view type, VisualScriptLibrary const* library)
	{
		return FindVisualScriptExposableType(type).has_value() || (library != nullptr && library->FindEnum(type).has_value());
	}

	bool ContainsVisualScriptTextIgnoringCase(std::string_view text, std::string_view part)
	{
		if (part.empty())
		{
			return true;
		}
		auto const lower = [](char c)
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		};
		return std::search(text.begin(), text.end(), part.begin(), part.end(),
		                   [&lower](char a, char b)
		                   {
							   return lower(a) == lower(b);
						   }) != text.end();
	}

	glm::vec2 SnapVisualScriptToGrid(glm::vec2 position)
	{
		return glm::round(position / VisualScriptCanvasMetrics::GridSize) * VisualScriptCanvasMetrics::GridSize;
	}

	bool IsVisualScriptReflectedType(std::string_view type)
	{
		return type.find(':') != std::string_view::npos;
	}

	namespace
	{
		bool NamesID(Json const& settings, std::string_view setting, uint32_t id)
		{
			if (!settings.is_object())
			{
				return false;
			}
			auto const found = settings.find(setting);
			if (found == settings.end() || !found->is_number_integer())
			{
				return false;
			}
			return found->is_number_unsigned() ? found->get<uint64_t>() == id : found->get<int64_t>() == static_cast<int64_t>(id);
		}

		bool HasExecPin(std::span<VisualScriptPin const> pins, VisualScriptPinDirection direction)
		{
			return std::any_of(pins.begin(), pins.end(),
			                   [direction](VisualScriptPin const& pin)
			                   {
								   return pin.Type == VisualScriptExecType && pin.Direction == direction;
							   });
		}
	}

	std::vector<uint32_t> FindVisualScriptUses(VisualScriptGraph const& graph, VisualScriptElementKind kind, uint32_t id)
	{
		std::vector<uint32_t> nodes;
		for (VisualScriptNode const& node : graph.Nodes)
		{
			// A node naming another graph uses that graph's element of the ID, never this document's.
			bool const uses = !IsLocalVisualScriptReference(node) ? false
			                  : kind == VisualScriptElementKind::Function
			                      ? node.Type == "Function.Call" && NamesID(node.Settings, "Function", id)
			                      : (node.Type == "Variable.Get" || node.Type == "Variable.Set") && NamesID(node.Settings, "Variable", id);
			if (uses)
			{
				nodes.push_back(node.ID);
			}
		}
		return nodes;
	}

	uint32_t CountVisualScriptUses(VisualScriptDocument const& document, VisualScriptElementKind kind, uint32_t id)
	{
		auto count = static_cast<uint32_t>(FindVisualScriptUses(document.Graph, kind, id).size());
		for (VisualScriptFunction const& function : document.Functions)
		{
			if (kind != VisualScriptElementKind::Function || function.ID != id)
			{
				count += static_cast<uint32_t>(FindVisualScriptUses(function.Graph, kind, id).size());
			}
		}
		return count;
	}

	uint32_t CountVisualScriptSelfUses(VisualScriptDocument const& document, AssetHandle graph, uint32_t variable)
	{
		uint32_t found = 0;
		auto const count = [&](VisualScriptGraph const& nodes)
		{
			for (VisualScriptNode const& node : nodes.Nodes)
			{
				if ((node.Type == "Variable.Get" || node.Type == "Variable.Set") && NamesID(node.Settings, "Variable", variable) &&
				    ReadVisualScriptGraphSetting(node) == std::optional<AssetHandle>(graph))
				{
					found++;
				}
			}
		};
		count(document.Graph);
		for (VisualScriptFunction const& function : document.Functions)
		{
			count(function.Graph);
		}
		return found;
	}

	std::string DescribeVisualScriptValueEdit(VisualScriptDocument const& base, VisualScriptDocument const& pending)
	{
		// The first variable or local whose fields differ, else the first node whose values differ: its input when one alone
		// differs, else its title.
		auto const variable = [](auto const& before, auto const& after) -> std::optional<std::string>
		{
			for (auto const& element : after)
			{
				auto const old = std::find_if(before.begin(), before.end(),
				                              [&element](auto const& candidate)
				                              {
												  return candidate.ID == element.ID;
											  });
				if (old != before.end() && !(*old == element))
				{
					return old->Name;
				}
			}
			return std::nullopt;
		};
		auto const node = [&base](VisualScriptGraph const& graph) -> std::optional<std::string>
		{
			for (VisualScriptNode const& edited : graph.Nodes)
			{
				VisualScriptNode const* const old = FindVisualScriptNode(base, edited.ID);
				if (old == nullptr || (edited.Inputs == old->Inputs && edited.Settings == old->Settings))
				{
					continue;
				}
				if (edited.Settings == old->Settings && edited.Inputs.is_object() && old->Inputs.is_object())
				{
					std::vector<std::string> changed;
					for (auto const& [key, value] : edited.Inputs.items())
					{
						auto const was = old->Inputs.find(key);
						if (was == old->Inputs.end() || *was != value)
						{
							changed.push_back(key);
						}
					}
					if (changed.size() == 1)
					{
						return changed.front();
					}
				}
				return DeriveVisualScriptNodeTitle(*old, base);
			}
			return std::nullopt;
		};
		if (std::optional<std::string> name = variable(base.Variables, pending.Variables))
		{
			return *name;
		}
		for (VisualScriptFunction const& function : pending.Functions)
		{
			for (VisualScriptFunction const& old : base.Functions)
			{
				if (old.ID == function.ID)
				{
					if (std::optional<std::string> name = variable(old.Locals, function.Locals))
					{
						return *name;
					}
				}
			}
		}
		if (std::optional<std::string> name = node(pending.Graph))
		{
			return *name;
		}
		for (VisualScriptFunction const& function : pending.Functions)
		{
			if (std::optional<std::string> name = node(function.Graph))
			{
				return *name;
			}
		}
		return "a value";
	}

	std::string DescribeVisualScriptOtherUses(VisualScriptOtherUses uses, std::string_view noun, std::string_view nouns)
	{
		return fmt::format("{} {} in {} {}", uses.Nodes, uses.Nodes == 1 ? noun : nouns, uses.Scripts,
		                   uses.Scripts == 1 ? "other script" : "other scripts");
	}

	namespace
	{
		// The other scripts' documents an edit of a graph reaches, and what they make of it.
		class OfferChange
		{
		public:
			OfferChange(AssetHandle graph, VisualScriptDocument const& before, VisualScriptDocument const& after,
			            std::span<VisualScriptProjectGraph const> graphs)
				: m_Graph(graph),
				  m_Before(before),
				  m_After(after),
				  m_Graphs(graphs)
			{
				for (VisualScriptProjectGraph const& script : graphs)
				{
					if (script.Graph != graph && script.Document != nullptr)
					{
						m_Scripts.emplace_back(script.Graph, script.Document.get());
					}
				}
				m_Referrers = VisualScriptEditing::FindVisualScriptBrokenReferrers(graph, before, after, m_Scripts);
			}

			std::vector<VisualScriptOfferToast> Describe()
			{
				if (m_Before.Kind == VisualScriptDocumentKind::Library && m_After.Kind == VisualScriptDocumentKind::Library)
				{
					for (VisualScriptFunction const& function : m_Before.Functions)
					{
						DescribeFunction(function);
					}
				}
				if (m_Before.Kind == VisualScriptDocumentKind::Script && m_After.Kind == VisualScriptDocumentKind::Script)
				{
					for (VisualScriptVariable const& variable : m_Before.Variables)
					{
						DescribeVariable(variable);
					}
					DescribeEvents();
				}
				return std::move(m_Toasts);
			}

		private:
			VisualScriptReference MakeReference(VisualScriptReference::Kind kind, uint32_t id, std::string name = {}) const
			{
				VisualScriptReference reference;
				reference.ReferenceKind = kind;
				reference.ID = id;
				reference.Name = std::move(name);
				reference.Graph = m_Graph;
				return reference;
			}

			void Add(std::string text, std::optional<VisualScriptReference> find = std::nullopt, std::string label = {})
			{
				m_Toasts.push_back({std::move(text), std::move(find), std::move(label)});
			}

			VisualScriptNode const* FindReferrerNode(VisualScriptEditing::VisualScriptReferrer const& referrer) const
			{
				for (auto const& [handle, document] : m_Scripts)
				{
					if (handle == referrer.Graph)
					{
						return FindVisualScriptNode(*document, referrer.Node);
					}
				}
				return nullptr;
			}

			// The referrers of a problem whose node passes the filter, counted by node and script.
			template<typename TFilter>
			VisualScriptOtherUses CountReferrers(VisualScriptEditing::VisualScriptReferrerProblem problem, TFilter const& filter) const
			{
				VisualScriptOtherUses uses;
				std::optional<AssetHandle> last;
				for (VisualScriptEditing::VisualScriptReferrer const& referrer : m_Referrers)
				{
					VisualScriptNode const* const node = referrer.Problem == problem ? FindReferrerNode(referrer) : nullptr;
					if (node == nullptr || !filter(*node))
					{
						continue;
					}
					uses.Nodes++;
					// Sorted by graph: a new graph is a new script.
					if (last != referrer.Graph)
					{
						uses.Scripts++;
						last = referrer.Graph;
					}
				}
				return uses;
			}

			void DescribeFunction(VisualScriptFunction const& function)
			{
				VisualScriptOtherUses const calls =
					CountVisualScriptOtherUses(m_Graphs, m_Graph, VisualScriptElementKind::Function, function.ID);
				if (calls.Nodes == 0)
				{
					return;
				}
				VisualScriptReference const callers = MakeReference(VisualScriptReference::Kind::Function, function.ID);
				auto const after = std::find_if(m_After.Functions.begin(), m_After.Functions.end(),
				                                [&function](VisualScriptFunction const& candidate)
				                                {
													return candidate.ID == function.ID;
												});
				if (after == m_After.Functions.end())
				{
					Add(fmt::format("Deleted {}: {} show errors", function.Name, DescribeVisualScriptOtherUses(calls, "call", "calls")),
					    callers, "Find Callers");
					return;
				}
				auto const callsIt = [&function](VisualScriptNode const& node)
				{
					return node.Type == "Function.Call" && NamesID(node.Settings, "Function", function.ID);
				};
				VisualScriptOtherUses const changed = CountReferrers(VisualScriptEditing::VisualScriptReferrerProblem::Changed, callsIt);
				if (after->Pure != function.Pure)
				{
					if (changed.Nodes > 0)
					{
						Add(fmt::format("Made {} {}: {} lose links", after->Name, after->Pure ? "pure" : "impure",
						                DescribeVisualScriptOtherUses(changed, "call", "calls")),
						    callers, "Find Callers");
					}
					return;
				}
				DescribeParameters(function.Inputs, after->Inputs, calls, changed, callers);
				DescribeParameters(function.Outputs, after->Outputs, calls, changed, callers);
			}

			void DescribeParameters(std::vector<VisualScriptParameter> const& before, std::vector<VisualScriptParameter> const& after,
			                        VisualScriptOtherUses calls, VisualScriptOtherUses changed, VisualScriptReference const& callers)
			{
				for (VisualScriptParameter const& parameter : before)
				{
					auto const same = std::find_if(after.begin(), after.end(),
					                               [&parameter](VisualScriptParameter const& candidate)
					                               {
													   return candidate.Name == parameter.Name;
												   });
					if (same != after.end())
					{
						if (same->Type != parameter.Type && changed.Nodes > 0)
						{
							Add(fmt::format("Changed {} to {}: {} may not fit", parameter.Name, same->Type,
							                DescribeVisualScriptOtherUses(changed, "call", "calls")),
							    callers, "Find Callers");
						}
						continue;
					}
					auto const renamed = std::find_if(after.begin(), after.end(),
					                                  [&parameter](VisualScriptParameter const& candidate)
					                                  {
														  return std::find(candidate.FormerNames.begin(), candidate.FormerNames.end(),
						                                                   parameter.Name) != candidate.FormerNames.end();
													  });
					if (renamed != after.end())
					{
						Add(fmt::format("Renamed {} to {}: {} follow it", parameter.Name, renamed->Name,
						                DescribeVisualScriptOtherUses(calls, "call", "calls")));
					}
					else if (changed.Nodes > 0)
					{
						Add(fmt::format("Removed {}: {} lose links", parameter.Name,
						                DescribeVisualScriptOtherUses(changed, "call", "calls")),
						    callers, "Find Callers");
					}
				}
			}

			void DescribeVariable(VisualScriptVariable const& variable)
			{
				if (!variable.Public)
				{
					return;
				}
				auto const usesIt = [&variable](VisualScriptNode const& node)
				{
					return (node.Type == "Variable.Get" || node.Type == "Variable.Set") && NamesID(node.Settings, "Variable", variable.ID);
				};
				auto const after = std::find_if(m_After.Variables.begin(), m_After.Variables.end(),
				                                [&variable](VisualScriptVariable const& candidate)
				                                {
													return candidate.ID == variable.ID;
												});
				VisualScriptEditing::VisualScriptReferrerProblem const problem =
					after == m_After.Variables.end() ? VisualScriptEditing::VisualScriptReferrerProblem::Removed
					: !after->Public                 ? VisualScriptEditing::VisualScriptReferrerProblem::Private
													 : VisualScriptEditing::VisualScriptReferrerProblem::Changed;
				VisualScriptOtherUses const broken = CountReferrers(problem, usesIt);
				if (broken.Nodes == 0)
				{
					return;
				}
				std::string const uses = DescribeVisualScriptOtherUses(broken, "node", "nodes");
				VisualScriptReference const references = MakeReference(VisualScriptReference::Kind::Variable, variable.ID);
				switch (problem)
				{
					case VisualScriptEditing::VisualScriptReferrerProblem::Removed:
						Add(fmt::format("Deleted {}: {} show errors", variable.Name, uses), references, "Find References");
						break;
					case VisualScriptEditing::VisualScriptReferrerProblem::Private:
						Add(fmt::format("Made {} private: {} show errors", after->Name, uses), references, "Find References");
						break;
					case VisualScriptEditing::VisualScriptReferrerProblem::Changed:
						Add(fmt::format("Changed {} to {}: {} may not fit", after->Name, after->Type, uses), references, "Find References");
						break;
					case VisualScriptEditing::VisualScriptReferrerProblem::Stale:
						break;
				}
			}

			static std::optional<std::string> ReadName(VisualScriptNode const& node)
			{
				if (!node.Settings.is_object())
				{
					return std::nullopt;
				}
				auto const name = node.Settings.find("Name");
				return name != node.Settings.end() && name->is_string() ? std::optional<std::string>(name->get<std::string>())
				                                                        : std::nullopt;
			}

			static bool HasEvent(VisualScriptDocument const& document, std::string_view name)
			{
				return std::any_of(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
				                   [name](VisualScriptNode const& node)
				                   {
									   return node.Type == "Event.Custom" && ReadName(node) == std::optional<std::string>(name);
								   });
			}

			void DescribeEvents()
			{
				std::vector<std::string> told;
				for (VisualScriptNode const& event : m_Before.Graph.Nodes)
				{
					std::optional<std::string> const name = event.Type == "Event.Custom" ? ReadName(event) : std::nullopt;
					if (!name || HasEvent(m_After, *name) || std::find(told.begin(), told.end(), *name) != told.end())
					{
						continue;
					}
					told.push_back(*name);
					VisualScriptOtherUses const stale = CountReferrers(VisualScriptEditing::VisualScriptReferrerProblem::Stale,
					                                                   [&name](VisualScriptNode const& node)
					                                                   {
																		   return ReadName(node) == name;
																	   });
					if (stale.Nodes == 0)
					{
						continue;
					}
					std::string const senders = DescribeVisualScriptOtherUses(stale, "sender", "senders");
					VisualScriptReference const find = MakeReference(VisualScriptReference::Kind::CustomEvent, 0, *name);
					VisualScriptNode const* const renamed = FindVisualScriptNode(m_After, event.ID);
					std::optional<std::string> const newName =
						renamed != nullptr && renamed->Type == "Event.Custom" ? ReadName(*renamed) : std::nullopt;
					if (!newName)
					{
						Add(fmt::format("Deleted {}: {} still send it", *name, senders), find, "Find Senders");
						continue;
					}
					// This script's senders the rename took along (RenameCustomEvent).
					uint32_t followed = 0;
					auto const countFollowed = [&](VisualScriptGraph const& graph)
					{
						for (VisualScriptNode const& node : graph.Nodes)
						{
							VisualScriptNode const* const old = FindVisualScriptNode(m_Before, node.ID);
							if ((node.Type == "Event.Send" || node.Type == "Event.Broadcast") && ReadName(node) == newName &&
							    old != nullptr && old->Type == node.Type && ReadName(*old) == name)
							{
								followed++;
							}
						}
					};
					countFollowed(m_After.Graph);
					for (VisualScriptFunction const& function : m_After.Functions)
					{
						countFollowed(function.Graph);
					}
					std::string const along =
						followed > 0 ? fmt::format(" (and {} {})", followed, followed == 1 ? "sender" : "senders") : std::string();
					Add(fmt::format("Renamed {} to {}{}. {} still send {}", *name, *newName, along, senders, *name), find, "Find Senders");
				}
			}

			AssetHandle m_Graph;
			VisualScriptDocument const& m_Before;
			VisualScriptDocument const& m_After;
			std::span<VisualScriptProjectGraph const> m_Graphs;
			std::vector<std::pair<AssetHandle, VisualScriptDocument const*>> m_Scripts;
			std::vector<VisualScriptEditing::VisualScriptReferrer> m_Referrers;
			std::vector<VisualScriptOfferToast> m_Toasts;
		};
	}

	std::vector<VisualScriptOfferToast> DescribeVisualScriptOfferChange(AssetHandle graph, VisualScriptDocument const& before,
	                                                                    VisualScriptDocument const& after,
	                                                                    std::span<VisualScriptProjectGraph const> graphs)
	{
		if (graphs.empty() || VisualScriptEditing::HaveSameVisualScriptOffer(before, after))
		{
			return {};
		}
		return OfferChange(graph, before, after, graphs).Describe();
	}

	std::string DescribeVisualScriptLibraryUse(std::span<VisualScriptProjectGraph const> graphs, AssetHandle graph)
	{
		uint32_t nodes = 0;
		uint32_t scripts = 0;
		for (VisualScriptProjectGraph const& script : graphs)
		{
			if (script.Graph == graph)
			{
				continue;
			}
			for (VisualScriptGraphReference const& reference : script.References)
			{
				if (reference.Graph == graph && reference.Nodes > 0)
				{
					nodes += reference.Nodes;
					scripts++;
				}
			}
		}
		if (scripts == 0)
		{
			return "Function library \xC2\xB7 used by no script yet";
		}
		return fmt::format("Function library \xC2\xB7 used by {} {} in {} {}", nodes, nodes == 1 ? "node" : "nodes", scripts,
		                   scripts == 1 ? "script" : "scripts");
	}

	VisualScriptOtherUses CountVisualScriptOtherUses(std::span<VisualScriptProjectGraph const> graphs, AssetHandle graph,
	                                                 VisualScriptElementKind kind, uint32_t id)
	{
		VisualScriptOtherUses uses;
		bool const function = kind == VisualScriptElementKind::Function;
		auto const count = [&](VisualScriptGraph const& nodes)
		{
			uint32_t found = 0;
			for (VisualScriptNode const& node : nodes.Nodes)
			{
				bool const named =
					function ? node.Type == "Function.Call" && NamesID(node.Settings, "Function", id)
							 : (node.Type == "Variable.Get" || node.Type == "Variable.Set") && NamesID(node.Settings, "Variable", id);
				if (named && ReadVisualScriptGraphSetting(node) == std::optional<AssetHandle>(graph))
				{
					found++;
				}
			}
			return found;
		};
		for (VisualScriptProjectGraph const& script : graphs)
		{
			if (script.Graph == graph || script.Document == nullptr)
			{
				continue;
			}
			uint32_t nodes = count(script.Document->Graph);
			for (VisualScriptFunction const& owner : script.Document->Functions)
			{
				nodes += count(owner.Graph);
			}
			if (nodes > 0)
			{
				uses.Nodes += nodes;
				uses.Scripts++;
			}
		}
		return uses;
	}

	VisualScriptEnabledOffer GetVisualScriptEnabledOffer(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
	                                                     VisualScriptNode const& node)
	{
		if (node.Type == "Function.Entry" || node.Type == "Function.Return")
		{
			return VisualScriptEnabledOffer::FunctionEnd;
		}
		VisualScriptCanvasModel const& canvas = model.GetCanvas();
		if (std::optional<uint32_t> const index = canvas.FindNode(node.ID))
		{
			std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
			VisualScriptNodeVisual const& visual = nodes[*index];
			if (visual.Kind == VisualScriptNodeKind::Event && node.Type != "Event.Send")
			{
				return VisualScriptEnabledOffer::Offered;
			}
			// A type nothing describes (Inferred without a library, UnknownType when the library does not know it): disabling it
			// ends its chain, which is how its graph keeps running (spec §21).
			if (visual.HasFlag(VisualScriptNodeFlags::Inferred) || visual.HasFlag(VisualScriptNodeFlags::UnknownType))
			{
				return VisualScriptEnabledOffer::Offered;
			}
			if (visual.HasFlag(VisualScriptNodeFlags::Pure))
			{
				return VisualScriptEnabledOffer::PureNode;
			}
			bool execIn = false;
			bool execOut = false;
			for (VisualScriptPinVisual const& pin : canvas.GetPins(visual))
			{
				if (pin.Type == VisualScriptExecType && !pin.HasFlag(VisualScriptPinFlags::Orphan))
				{
					(pin.Direction == VisualScriptPinDirection::In ? execIn : execOut) = true;
				}
			}
			return execIn && execOut ? VisualScriptEnabledOffer::Offered : VisualScriptEnabledOffer::NoFlow;
		}
		VisualScriptNodeTemplate const* const kind = library != nullptr ? library->FindNode(node.Type) : nullptr;
		if (kind == nullptr || (kind->Kind == VisualScriptNodeKind::Event && node.Type != "Event.Send"))
		{
			return VisualScriptEnabledOffer::Offered;
		}
		if (kind->Pure)
		{
			return VisualScriptEnabledOffer::PureNode;
		}
		return HasExecPin(kind->Pins, VisualScriptPinDirection::In) && HasExecPin(kind->Pins, VisualScriptPinDirection::Out)
		           ? VisualScriptEnabledOffer::Offered
		           : VisualScriptEnabledOffer::NoFlow;
	}

	// --- VisualScriptWidgetCommits ---

	void VisualScriptWidgetCommits::BeginFrame(VisualScriptPaneContext& context, VisualScriptPaneResult& result)
	{
		m_Model = &context.Model;
		result.Notes.insert(result.Notes.end(), std::make_move_iterator(m_DroppedNotes.begin()),
		                    std::make_move_iterator(m_DroppedNotes.end()));
		m_DroppedNotes.clear();
		DropStalePending(context.Model, result.Notes);
		if (m_Pending != nullptr && m_HasCommitted && context.Session.GetMergeKey() == m_Key &&
		    context.Time - m_LastCommit >= CommitInterval)
		{
			// The widget is still in use but its value has not changed since: it reaches the document (and a running game) no
			// later than CommitInterval after the previous commit.
			CommitPending(context, result);
		}
		if (!m_Waiting)
		{
			return;
		}
		WaitingEdit waiting = std::move(*m_Waiting);
		m_Waiting.reset();
		if (context.Model.GetStoredDocument() == waiting.Base)
		{
			Commit(context, std::move(waiting.Document), std::move(waiting.Action), waiting.MergeKey, result, std::move(waiting.Select));
		}
	}

	VisualScriptDocument const& VisualScriptWidgetCommits::GetDocument(VisualScriptEditorModel const& model) const
	{
		if (m_Pending != nullptr && model.GetStoredDocument() == m_PendingBase)
		{
			return *m_Pending;
		}
		return model.GetDisplayDocument();
	}

	Ref<VisualScriptDocument const> VisualScriptWidgetCommits::GetPendingDocument(VisualScriptEditorModel const& model) const
	{
		return m_Pending != nullptr && model.GetStoredDocument() == m_PendingBase ? m_Pending : nullptr;
	}

	void VisualScriptWidgetCommits::DropStalePending(VisualScriptEditorModel& model, std::vector<std::string>& notes)
	{
		if (m_Pending == nullptr || model.GetStoredDocument() == m_PendingBase)
		{
			return;
		}
		// Something else changed the document (the model dropped the preview with it). A value of other elements than the
		// change touched carries over (spec-slice3 §10.11): the interaction goes on, and its next commit is a new step.
		Ref<VisualScriptDocument const> const current = model.GetStoredDocument();
		if (current != nullptr && m_PendingBase != nullptr)
		{
			if (std::optional<VisualScriptDocument> rebased =
			        VisualScriptEditing::RebaseVisualScriptEdit(*m_PendingBase, *m_Pending, *current))
			{
				model.SetWidgetPreview(*rebased);
				m_Pending = CreateRef<VisualScriptDocument const>(std::move(*rebased));
				m_PendingBase = current;
				return;
			}
		}
		// The value no longer applies.
		std::string const what = m_PendingBase != nullptr ? DescribeVisualScriptValueEdit(*m_PendingBase, *m_Pending) : "a value";
		notes.push_back(fmt::format("Your edit of {} was replaced by a change made elsewhere", what));
		m_Pending.reset();
		m_PendingBase.reset();
	}

	void VisualScriptWidgetCommits::Change(VisualScriptPaneContext& context, VisualScriptDocument edited, std::string action, bool active,
	                                       VisualScriptPaneResult& result)
	{
		VisualScriptEditorModel& model = context.Model;
		m_Model = &model;
		DropStalePending(model, result.Notes);
		result.WidgetActive = result.WidgetActive || active;
		if (model.GetStoredDocument() == nullptr || (m_Pending != nullptr && IsSameVisualScriptDocument(edited, *m_Pending)))
		{
			return;
		}
		if (IsSameVisualScriptDocument(edited, model.GetDisplayDocument()))
		{
			// Back to what is committed: the value pending is not the widget's any more.
			if (m_Pending != nullptr)
			{
				m_Pending.reset();
				m_PendingBase.reset();
				model.ClearWidgetPreview();
			}
			return;
		}
		uint64_t const key = context.Session.GetMergeKey();
		if (key != m_Key)
		{
			// A new interaction: its first change commits at once.
			m_Key = key;
			m_HasCommitted = false;
		}
		model.SetWidgetPreview(edited);
		m_Pending = CreateRef<VisualScriptDocument const>(std::move(edited));
		m_PendingAction = std::move(action);
		m_ScriptName = std::string(context.ScriptName);
		m_PendingBase = model.GetStoredDocument();
		if (!m_HasCommitted || !active || context.Time - m_LastCommit >= CommitInterval)
		{
			CommitPending(context, result);
		}
	}

	void VisualScriptWidgetCommits::End(VisualScriptPaneContext& context, VisualScriptPaneResult& result)
	{
		m_Model = &context.Model;
		DropStalePending(context.Model, result.Notes);
		CommitPending(context, result);
	}

	void VisualScriptWidgetCommits::Commit(VisualScriptPaneContext& context, VisualScriptDocument edited, std::string action,
	                                       uint64_t mergeKey, VisualScriptPaneResult& result, std::vector<uint32_t> select)
	{
		VisualScriptEditorModel& model = context.Model;
		m_Model = &model;
		DropStalePending(model, result.Notes);
		CommitPending(context, result);
		if (result.Commit)
		{
			m_Waiting = WaitingEdit{std::move(edited), std::move(action), mergeKey, std::move(select), result.Commit->Document};
			return;
		}
		if (model.GetStoredDocument() == nullptr || IsSameVisualScriptDocument(edited, model.GetDisplayDocument()))
		{
			return;
		}
		Ref<VisualScriptDocument const> const stored = GetStored(model, mergeKey);
		Result<VisualScriptCommitRequest> request =
			MakeVisualScriptCommitRequest(*stored, std::move(edited), model.GetAnalysis(), action, context.ScriptName, mergeKey);
		if (!request)
		{
			result.Notes.push_back(fmt::format("{} failed: {}", action, request.GetError()));
			return;
		}
		request.GetValue().Select = std::move(select);
		m_RunWritten = request.GetValue().Document;
		result.Commit = request.TakeValue();
	}

	std::optional<VisualScriptCommitRequest> VisualScriptWidgetCommits::TakePendingEdit()
	{
		if (m_Pending == nullptr || m_Model == nullptr)
		{
			return std::nullopt;
		}
		VisualScriptEditorModel& model = *m_Model;
		// A flush has no pane result to tell a dropped value in: the pane's next Draw tells it.
		DropStalePending(model, m_DroppedNotes);
		if (m_Pending == nullptr)
		{
			return std::nullopt;
		}
		VisualScriptDocument pending = *m_Pending;
		m_Pending.reset();
		m_PendingBase.reset();
		model.ClearWidgetPreview();
		Ref<VisualScriptDocument const> const stored = GetStored(model, m_Key);
		Result<VisualScriptCommitRequest> request =
			MakeVisualScriptCommitRequest(*stored, std::move(pending), model.GetAnalysis(), m_PendingAction, m_ScriptName, m_Key);
		if (!request)
		{
			ST_ERROR("{} failed: {}", m_PendingAction, request.GetError());
			return std::nullopt;
		}
		m_RunWritten = request.GetValue().Document;
		return request.TakeValue();
	}

	Ref<VisualScriptDocument const> VisualScriptWidgetCommits::GetStored(VisualScriptEditorModel const& model, uint64_t key)
	{
		Ref<VisualScriptDocument const> const current = model.GetStoredDocument();
		if (key != 0 && key == m_RunKey && m_RunStored != nullptr && current == m_RunWritten)
		{
			return m_RunStored;
		}
		m_RunKey = key;
		m_RunStored = current;
		m_RunWritten.reset();
		return current;
	}

	bool VisualScriptWidgetCommits::CommitPending(VisualScriptPaneContext& context, VisualScriptPaneResult& result)
	{
		if (m_Pending == nullptr || result.Commit)
		{
			return false;
		}
		VisualScriptEditorModel& model = context.Model;
		VisualScriptDocument pending = *m_Pending;
		m_Pending.reset();
		m_PendingBase.reset();
		model.ClearWidgetPreview();
		Ref<VisualScriptDocument const> const stored = GetStored(model, m_Key);
		Result<VisualScriptCommitRequest> request =
			MakeVisualScriptCommitRequest(*stored, std::move(pending), model.GetAnalysis(), m_PendingAction, context.ScriptName, m_Key);
		if (!request)
		{
			result.Notes.push_back(fmt::format("{} failed: {}", m_PendingAction, request.GetError()));
			return false;
		}
		m_RunWritten = request.GetValue().Document;
		m_LastCommit = context.Time;
		m_HasCommitted = true;
		result.Commit = request.TakeValue();
		return true;
	}

	// --- VisualScriptNameField ---

	namespace
	{
		// Puts the cursor after the text when the field gets the keyboard (ImGui selects all of a field it gives the keyboard
		// to, and the next character would replace it).
		int PlaceNameCursor(ImGuiInputTextCallbackData* data)
		{
			bool* const place = static_cast<bool*>(data->UserData);
			if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && *place)
			{
				data->CursorPos = data->BufTextLen;
				data->SelectionStart = data->BufTextLen;
				data->SelectionEnd = data->BufTextLen;
				*place = false;
			}
			return 0;
		}
	}

	VisualScriptNameField::Outcome VisualScriptNameField::Draw(char const* id, std::string const& current, Validator const& validate,
	                                                           bool focus)
	{
		Outcome outcome;
		ImGuiID const itemId = ImGui::GetID(id);
		int const frame = ImGui::GetFrameCount();
		ImGuiInputTextFlags const inputFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways;
		if (m_Left == itemId)
		{
			// Another field of this object took the keyboard from this one, in this frame or the last: the name typed here is
			// settled now, as if this field had been left by a click anywhere else.
			m_Left = 0;
			std::string text = std::move(m_LeftText);
			if (m_LeftFrame + 1 >= frame)
			{
				// No callback here: the cursor is placed only for a field opened for typing.
				ImGui::InputText(id, &text, ImGuiInputTextFlags_EnterReturnsTrue);
				outcome.Active = ImGui::IsItemActive();
				if (text != current)
				{
					std::optional<std::string> const error = validate(text);
					if (error)
					{
						outcome.Refused = *error;
					}
					else
					{
						outcome.Committed = std::move(text);
					}
				}
				return outcome;
			}
		}
		if (m_Editing == itemId && m_DrawnFrame + 1 < frame)
		{
			// The field was not drawn in the last frame (its pane was hidden, its row scrolled away): ImGui let its keyboard go
			// with nobody to say so, and the name stays.
			m_Editing = 0;
			m_Error.clear();
			m_Refocus = false;
		}
		if (focus && m_Editing != itemId)
		{
			Leave(frame);
			m_Editing = itemId;
			m_Text = current;
			m_Error.clear();
			m_Checked = false;
			m_Refocus = true;
		}
		bool const editing = m_Editing == itemId;
		if (editing)
		{
			m_DrawnFrame = frame;
		}
		if (editing && m_Refocus)
		{
			ImGui::SetKeyboardFocusHere();
			m_Refocus = false;
			m_PlaceCursor = m_CursorAtEnd;
			m_CursorAtEnd = false;
		}
		// While the name is edited the field shows the text typed, whichever item has the keyboard now: a field left for a
		// widget drawn before it in this frame reports that it was left only once it is drawn, with the text typed.
		std::string shown = current;
		std::string& buffer = editing ? m_Text : shown;
		bool const invalid = editing && !m_Error.empty();
		if (invalid)
		{
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(VisualScriptErrorColor));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(ImGui::GetStyle().FrameBorderSize, 1.0f));
		}
		bool const entered = ImGui::InputText(id, &buffer, inputFlags, PlaceNameCursor, &m_PlaceCursor);
		if (invalid)
		{
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}
		outcome.Active = ImGui::IsItemActive();
		if (!editing)
		{
			if (ImGui::IsItemActivated())
			{
				Leave(frame);
				m_Editing = itemId;
				m_DrawnFrame = frame;
				// What a click put in the field in the frame it took the keyboard is kept.
				m_Text = std::move(shown);
				m_Error.clear();
				m_Checked = false;
			}
			return outcome;
		}

		if (!m_Checked || m_Text != m_CheckedText)
		{
			// Checked when the text changes: a check may try the edit on a copy of the document.
			m_Error = m_Text == current ? std::string() : validate(m_Text).value_or(std::string());
			m_CheckedText = m_Text;
			m_Checked = true;
		}
		if (!m_Error.empty() && (outcome.Active || entered))
		{
			// Below the field while it has the keyboard: why the name cannot be given.
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			ImGui::SetNextWindowPos(ImVec2(min.x, max.y + ImGui::GetStyle().ItemSpacing.y));
			if (ImGui::BeginTooltip())
			{
				ImGui::TextUnformatted(m_Error.c_str());
				ImGui::EndTooltip();
			}
		}
		bool const deactivated = ImGui::IsItemDeactivated();
		if (deactivated && !entered && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			m_Editing = 0;
			m_Error.clear();
			return outcome;
		}
		if (entered && !m_Error.empty())
		{
			// Enter is refused: the field keeps the text and gets the keyboard back, the cursor after the text. Until it has it
			// again, the field stays the one edited.
			m_Refocus = true;
			m_CursorAtEnd = true;
			return outcome;
		}
		if (entered || deactivated)
		{
			if (!m_Error.empty())
			{
				outcome.Refused = m_Error;
			}
			else if (m_Text != current)
			{
				outcome.Committed = m_Text;
			}
			m_Editing = 0;
			m_Error.clear();
		}
		return outcome;
	}

	void VisualScriptNameField::Leave(int frame)
	{
		if (m_Editing == 0)
		{
			return;
		}
		m_Left = m_Editing;
		m_LeftText = std::move(m_Text);
		m_LeftFrame = frame;
		m_Editing = 0;
		m_Error.clear();
		m_Refocus = false;
	}
}
