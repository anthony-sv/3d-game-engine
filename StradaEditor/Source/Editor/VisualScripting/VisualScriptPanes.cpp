#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace Strada
{
	namespace
	{
		// The shown sub-graph's node of an ID; null when it has none.
		VisualScriptNode const* FindShownNode(VisualScriptEditorModel const& model, uint32_t id)
		{
			VisualScriptDocument const& document = model.GetDisplayDocument();
			VisualScriptGraph const* graph = &document.Graph;
			if (model.GetFunction() != 0)
			{
				auto const function = std::find_if(document.Functions.begin(), document.Functions.end(),
				                                   [&model](VisualScriptFunction const& candidate)
				                                   {
													   return candidate.ID == model.GetFunction();
												   });
				if (function == document.Functions.end())
				{
					return nullptr;
				}
				graph = &function->Graph;
			}
			auto const node = std::find_if(graph->Nodes.begin(), graph->Nodes.end(),
			                               [id](VisualScriptNode const& candidate)
			                               {
											   return candidate.ID == id;
										   });
			return node != graph->Nodes.end() ? &*node : nullptr;
		}

		// The library pin behind a canvas pin: the analysis' while the canvas draws the node from it, else the template's.
		VisualScriptPin const* FindLibraryPin(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
		                                      VisualScriptNodeVisual const& visual, std::string_view pin,
		                                      VisualScriptPinDirection direction)
		{
			VisualScriptAnalysis const* const analysis = model.GetAnalysis();
			if (analysis != nullptr && visual.HasFlag(VisualScriptNodeFlags::Analyzed))
			{
				if (VisualScriptAnalyzedNode const* const analyzed = analysis->FindNode(visual.ID))
				{
					return analyzed->FindPin(pin, direction);
				}
			}
			if (library == nullptr)
			{
				return nullptr;
			}
			VisualScriptNode const* const node = FindShownNode(model, visual.ID);
			VisualScriptNodeTemplate const* const kind = node != nullptr ? library->FindNode(node->Type) : nullptr;
			return kind != nullptr ? kind->FindPin(pin, direction) : nullptr;
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
		return [&model, library, newNode](uint32_t id)
		{
			std::vector<VisualScriptNodePin> rows;
			if (newNode && newNode->Node == id)
			{
				rows.reserve(newNode->Pins.size());
				for (VisualScriptPin const& pin : newNode->Pins)
				{
					rows.push_back({pin.Name, pin.Direction, pin.Type, glm::vec2(0.0f), pin.Default, pin.DefaultsToSelf});
				}
				return rows;
			}
			VisualScriptCanvasModel const& canvas = model.GetCanvas();
			std::optional<uint32_t> const index = canvas.FindNode(id);
			if (!index)
			{
				return rows;
			}
			std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
			VisualScriptNodeVisual const& visual = nodes[*index];
			VisualScriptAnalysis const* const analysis = model.GetAnalysis();
			VisualScriptAnalyzedNode const* const analyzed = analysis != nullptr ? analysis->FindNode(id) : nullptr;
			std::span<VisualScriptPinVisual const> const pins = canvas.GetPins(visual);
			rows.reserve(pins.size());
			for (VisualScriptPinVisual const& pin : pins)
			{
				if (pin.HasFlag(VisualScriptPinFlags::Orphan))
				{
					continue;
				}
				std::string_view const name = canvas.GetText(pin.Name);
				VisualScriptNodePin row;
				row.Name = VisualScriptEditing::ToStoredVisualScriptPin(analyzed, pin.Direction, name);
				row.Direction = pin.Direction;
				row.Type = pin.Type;
				row.Anchor = pin.Anchor;
				if (VisualScriptPin const* const libraryPin = FindLibraryPin(model, library, visual, name, pin.Direction))
				{
					row.Default = libraryPin->Default;
					row.DefaultsToSelf = libraryPin->DefaultsToSelf;
				}
				rows.push_back(std::move(row));
			}
			return rows;
		};
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
	                                     VisualScriptNode const& node)
	{
		if (analysis != nullptr)
		{
			VisualScriptAnalyzedNode const* const analyzed = analysis->FindNode(node.ID);
			if (analyzed != nullptr && analyzed->Type == node.Type && !analyzed->Title.empty())
			{
				return analyzed->Title;
			}
		}
		return DeriveVisualScriptNodeTitle(node, document);
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
			bool const uses = kind == VisualScriptElementKind::Function
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
			if (visual.HasFlag(VisualScriptNodeFlags::Pure))
			{
				return VisualScriptEnabledOffer::PureNode;
			}
			if (visual.HasFlag(VisualScriptNodeFlags::Inferred))
			{
				// A type nothing describes: disabling it ends its chain.
				return VisualScriptEnabledOffer::Offered;
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
		DropStalePending(context.Model);
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

	void VisualScriptWidgetCommits::DropStalePending(VisualScriptEditorModel& model)
	{
		if (m_Pending != nullptr && model.GetStoredDocument() != m_PendingBase)
		{
			// Something else changed the document (the model dropped the preview with it): the value no longer applies.
			m_Pending.reset();
			m_PendingBase.reset();
		}
	}

	void VisualScriptWidgetCommits::Change(VisualScriptPaneContext& context, VisualScriptDocument edited, std::string action, bool active,
	                                       VisualScriptPaneResult& result)
	{
		VisualScriptEditorModel& model = context.Model;
		m_Model = &model;
		DropStalePending(model);
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
		DropStalePending(context.Model);
		CommitPending(context, result);
	}

	void VisualScriptWidgetCommits::Commit(VisualScriptPaneContext& context, VisualScriptDocument edited, std::string action,
	                                       uint64_t mergeKey, VisualScriptPaneResult& result, std::vector<uint32_t> select)
	{
		VisualScriptEditorModel& model = context.Model;
		m_Model = &model;
		DropStalePending(model);
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
		DropStalePending(model);
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
				ImGui::InputText(id, &text, inputFlags);
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
