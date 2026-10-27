#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

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

	void SelectVisualScriptElement(VisualScriptPaneContext& context, VisualScriptElement element)
	{
		context.Shared.Element = element;
		context.Model.Select({}, VisualScriptSelectMode::Replace);
	}

	// --- VisualScriptWidgetCommits ---

	void VisualScriptWidgetCommits::BeginFrame(VisualScriptPaneContext& context, VisualScriptPaneResult& result)
	{
		m_Model = &context.Model;
		DropStalePending(context.Model);
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
			MakeVisualScriptCommitRequest(*stored, std::move(pending), nullptr, m_PendingAction, m_ScriptName, m_Key);
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
		if (m_Editing == itemId && !m_Refocus && ImGui::GetActiveID() != itemId)
		{
			// The field lost the keyboard while it was not drawn (its pane was hidden): the name stays.
			m_Editing = 0;
			m_Error.clear();
		}
		if (focus && m_Editing != itemId)
		{
			m_Editing = itemId;
			m_Text = current;
			m_Error.clear();
			m_Checked = false;
			m_Refocus = true;
		}
		bool const editing = m_Editing == itemId;
		if (editing && m_Refocus)
		{
			ImGui::SetKeyboardFocusHere();
			m_Refocus = false;
			m_PlaceCursor = m_CursorAtEnd;
			m_CursorAtEnd = false;
		}
		std::string shown = editing ? std::string() : current;
		std::string& buffer = editing ? m_Text : shown;
		bool const invalid = editing && !m_Error.empty();
		if (invalid)
		{
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(VisualScriptErrorColor));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(ImGui::GetStyle().FrameBorderSize, 1.0f));
		}
		bool const entered = ImGui::InputText(id, &buffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
		                                      PlaceNameCursor, &m_PlaceCursor);
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
				m_Editing = itemId;
				m_Text = current;
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
			// Enter is refused: the field keeps the keyboard and the text.
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
}
