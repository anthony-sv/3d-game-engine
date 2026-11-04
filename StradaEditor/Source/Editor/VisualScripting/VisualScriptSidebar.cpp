#include "Editor/VisualScripting/VisualScriptSidebar.h"

#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptDrawer.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Log.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>

namespace Strada
{
	namespace
	{
		constexpr char const* DeleteModalId = "###VisualScriptDelete";
		constexpr char const* ProblemsPopupId = "##problems";

		std::string CountOf(uint32_t count, std::string_view singular, std::string_view plural)
		{
			return fmt::format("{} {}", count, count == 1 ? singular : plural);
		}

		// The pins of every node of the document for the operations that remove nodes in any graph: the canvas's rows for the
		// shown sub-graph's nodes, the analysis' pins under their stored names for the others, else the library template's.
		VisualScriptNodePins MakeDocumentPins(VisualScriptEditorModel const& model, VisualScriptLibrary const& library,
		                                      VisualScriptDocument const& document)
		{
			VisualScriptNodePins canvas = MakeVisualScriptNodePins(model, &library);
			VisualScriptAnalysis const* const analysis = model.GetAnalysis();
			return [canvas = std::move(canvas), analysis, &model, &library, &document](uint32_t id)
			{
				if (model.GetCanvas().FindNode(id))
				{
					return canvas(id);
				}
				std::vector<VisualScriptNodePin> rows;
				auto const add = [&rows](VisualScriptPin const& pin, std::string name)
				{
					rows.push_back({std::move(name), pin.Direction, pin.Type, glm::vec2(0.0f), pin.Default, pin.DefaultsToSelf});
				};
				if (VisualScriptAnalyzedNode const* const analyzed = analysis != nullptr ? analysis->FindNode(id) : nullptr)
				{
					for (VisualScriptPin const& pin : analyzed->Pins)
					{
						add(pin, VisualScriptEditing::ToStoredVisualScriptPin(analyzed, pin.Direction, pin.Name));
					}
					return rows;
				}
				VisualScriptNode const* const node = FindVisualScriptNode(document, id);
				if (VisualScriptNodeTemplate const* const kind = node != nullptr ? library.FindNode(node->Type) : nullptr)
				{
					for (VisualScriptPin const& pin : kind->Pins)
					{
						add(pin, pin.Name);
					}
				}
				return rows;
			};
		}

		std::string GetCustomEventName(VisualScriptNode const& node)
		{
			auto const found = node.Settings.is_object() ? node.Settings.find("Name") : node.Settings.end();
			return node.Settings.is_object() && found != node.Settings.end() && found->is_string() ? found->get<std::string>()
			                                                                                       : std::string();
		}

	}

	// What a Draw works with.
	struct VisualScriptSidebar::Frame
	{
		VisualScriptPaneContext& Context;
		VisualScriptPaneResult& Result;
		Ref<VisualScriptDocument const> Hold;
		VisualScriptDocument const& Document;
		Ref<VisualScriptLibrary const> LibraryHold;
		VisualScriptLibrary const* Library = nullptr;
		VisualScriptAnalysis const* Analysis = nullptr;
		bool ReadOnly = false;
	};

	bool VisualScriptSidebar::HandleAction(VisualScriptEditorAction action, VisualScriptPaneShared const& shared)
	{
		if (!shared.Element)
		{
			return false;
		}
		bool const eventGraph = shared.Element->Kind == VisualScriptElementKind::Function && shared.Element->ID == 0;
		switch (action)
		{
			case VisualScriptEditorAction::Delete:
			case VisualScriptEditorAction::DeleteWithoutRelink:
			case VisualScriptEditorAction::Rename:
			case VisualScriptEditorAction::Duplicate:
				if (eventGraph || m_ReadOnly)
				{
					return false;
				}
				m_Action = RoutedAction{action, *shared.Element, ImGui::GetFrameCount()};
				return true;
			case VisualScriptEditorAction::OpenNode:
				m_Action = RoutedAction{action, *shared.Element, ImGui::GetFrameCount()};
				return true;
			default:
				return false;
		}
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptSidebar::FindRow(VisualScriptElement element) const
	{
		auto const found = std::find_if(m_Rows.begin(), m_Rows.end(),
		                                [&element](RowRect const& row)
		                                {
											return row.Element == element;
										});
		return found != m_Rows.end() ? std::optional(std::pair(found->Min, found->Max)) : std::nullopt;
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptSidebar::FindAddButton(VisualScriptElementKind section) const
	{
		auto const found = std::find_if(m_AddButtons.begin(), m_AddButtons.end(),
		                                [section](auto const& button)
		                                {
											return button.first == section;
										});
		return found != m_AddButtons.end() ? std::optional(found->second) : std::nullopt;
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptSidebar::FindEye(uint32_t variable) const
	{
		auto const found = std::find_if(m_Eyes.begin(), m_Eyes.end(),
		                                [variable](auto const& eye)
		                                {
											return eye.first == variable;
										});
		return found != m_Eyes.end() ? std::optional(found->second) : std::nullopt;
	}

	std::optional<VisualScriptElement> VisualScriptSidebar::GetPendingDelete() const
	{
		return m_Delete;
	}

	bool VisualScriptSidebar::Matches(std::string_view name) const
	{
		return m_Search.empty() || ContainsVisualScriptTextIgnoringCase(name, m_Search);
	}

	VisualScriptPaneResult VisualScriptSidebar::Draw(VisualScriptPaneContext& context)
	{
		VisualScriptPaneResult result;
		m_Rows.clear();
		m_AddButtons.clear();
		m_Eyes.clear();
		m_Commits.BeginFrame(context, result);
		VisualScriptEditorModel& model = context.Model;
		if (model.GetStoredDocument() == nullptr)
		{
			ImGui::TextDisabled("No script is shown.");
			return result;
		}
		Ref<VisualScriptDocument const> const hold = m_Commits.GetPendingDocument(model);
		Ref<VisualScriptLibrary const> const library = context.Semantics.GetLibrary();
		Frame frame{context,
		            result,
		            hold,
		            hold != nullptr ? *hold : model.GetDisplayDocument(),
		            library,
		            library.get(),
		            model.GetAnalysis(),
		            model.IsReadOnly()};
		// The canvas's selection and a row's exclude each other.
		if (!model.GetSelection().empty())
		{
			context.Shared.Element.reset();
		}
		m_ReadOnly = frame.ReadOnly;
		m_LiveCellRects.clear();
		m_TypeRects.clear();
		m_LiveCellLabels.clear();
		m_LiveTooltip.clear();
		if (context.Shared.Rename)
		{
			// A quick fix that renames; nothing is renamed in a document that cannot be edited.
			if (!frame.ReadOnly)
			{
				m_Renaming = context.Shared.Rename;
				m_RenameFocus = true;
				m_NameKey = 0;
			}
			context.Shared.Rename.reset();
		}
		if (m_RenameCreated)
		{
			// The element the last commit created is a row now: its name is typed in the same step.
			m_Renaming = m_RenameCreated;
			m_RenameFocus = true;
			m_RenameCreated.reset();
		}
		UpdateLiveValues(frame);

		// Keys: those the window routed, and Enter on the selected row while My Script has the keyboard's focus.
		std::optional<VisualScriptEditorAction> action;
		if (std::optional<RoutedAction> const routed = std::exchange(m_Action, std::nullopt);
		    routed && routed->Frame + 1 >= ImGui::GetFrameCount() && context.Shared.Element == routed->Element)
		{
			action = routed->Action;
		}
		// My Script's own window or its children: not the window it is drawn in (the canvas beside it).
		bool const enter = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
		                   ImGui::IsKeyPressed(ImGuiKey_Enter, false);
		if (std::optional<VisualScriptElement> const element = context.Shared.Element)
		{
			if (enter || action == VisualScriptEditorAction::OpenNode)
			{
				Open(frame, *element);
			}
			else if (action == VisualScriptEditorAction::Delete || action == VisualScriptEditorAction::DeleteWithoutRelink)
			{
				Delete(frame, *element, false);
			}
			else if (action == VisualScriptEditorAction::Rename && !frame.ReadOnly)
			{
				m_Renaming = element;
				m_RenameFocus = true;
				m_NameKey = 0;
			}
			else if (action == VisualScriptEditorAction::Duplicate)
			{
				Duplicate(frame, *element);
			}
		}
		// A library's elements are its functions: variables and custom events show only while it still has some, so that they
		// can be deleted (spec-slice3 §10.9).
		bool const isLibrary = frame.Document.Kind == VisualScriptDocumentKind::Library;
		if (isLibrary)
		{
			m_LibraryLine = DescribeVisualScriptLibraryUse(model.GetProjectGraphs(), context.Graph);
			// Wrapped: in a narrow pane the line goes on below rather than past the pane's edge.
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%s", m_LibraryLine.c_str());
			ImGui::PopTextWrapPos();
			m_LibraryLineRect.emplace(glm::vec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y),
			                          glm::vec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y));
		}
		else
		{
			m_LibraryLine.clear();
			m_LibraryLineRect.reset();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##search", "Search", &m_Search);

		DrawGraphs(frame);
		if (!isLibrary || !frame.Document.Variables.empty())
		{
			DrawVariables(frame);
		}
		if (model.GetFunction() != 0)
		{
			DrawLocals(frame);
		}
		bool const events = std::any_of(frame.Document.Graph.Nodes.begin(), frame.Document.Graph.Nodes.end(),
		                                [](VisualScriptNode const& node)
		                                {
											return node.Type == "Event.Custom";
										});
		if (!isLibrary || events)
		{
			DrawCustomEvents(frame);
		}

		DrawDeleteModal(frame);
		return result;
	}

	bool VisualScriptSidebar::BeginSection(Frame& frame, char const* label, VisualScriptElementKind kind, bool canAdd)
	{
		// The section of a row renamed from elsewhere (F2, a quick fix) opens.
		if (m_Renaming && m_RenameFocus &&
		    (m_Renaming->Kind == kind ||
		     (kind == VisualScriptElementKind::Function && m_Renaming->Kind == VisualScriptElementKind::Function)))
		{
			ImGui::SetNextItemOpen(true);
		}
		float const right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		bool const open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (canAdd)
		{
			ImGui::PushID(label);
			float const width = ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2.0f;
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(right - width - ImGui::GetStyle().FramePadding.x, ImGui::GetCursorScreenPos().y));
			ImGui::BeginDisabled(frame.ReadOnly);
			if (ImGui::SmallButton("+"))
			{
				Add(frame, kind);
			}
			ImGui::EndDisabled();
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			m_AddButtons.emplace_back(kind, std::pair(glm::vec2(min.x, min.y), glm::vec2(max.x, max.y)));
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
			{
				ImGui::SetTooltip("Add %s", kind == VisualScriptElementKind::Variable   ? "a variable"
				                            : kind == VisualScriptElementKind::Local    ? "a local"
				                            : kind == VisualScriptElementKind::Function ? "a function"
				                                                                        : "a custom event");
			}
			ImGui::PopID();
		}
		return open;
	}

	bool VisualScriptSidebar::DrawRow(Frame& frame, VisualScriptElement element, std::string const& name, bool canRename,
	                                  std::optional<uint32_t>* dropped)
	{
		VisualScriptPaneContext& context = frame.Context;
		if (m_Renaming == element && frame.ReadOnly)
		{
			// The document became read-only while the name was typed (the file was made read-only): nothing can be renamed.
			m_Renaming.reset();
			m_NameKey = 0;
		}
		if (canRename && m_Renaming == element)
		{
			Rename(frame, element, name);
			if (m_Renaming == element)
			{
				return false;
			}
		}
		bool const selected = context.Shared.Element == element;
		// Where the row's contents start: Selectable widens its box by half the item spacing on each side
		// (imgui_widgets.cpp:7434-7440), so its rectangle starts left of the content region, where the pane clips.
		float const contentX = ImGui::GetCursorScreenPos().x;
		ImGui::SetNextItemAllowOverlap();
		bool const clicked =
			ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, ImGui::GetFrameHeight()));
		ImVec2 const min = ImGui::GetItemRectMin();
		ImVec2 const max = ImGui::GetItemRectMax();
		m_Rows.push_back({element, glm::vec2(min.x, min.y), glm::vec2(max.x, max.y)});
		if (clicked)
		{
			SelectVisualScriptElement(context, element);
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				Open(frame, element);
			}
		}
		// A row of the same kind of this graph dropped here.
		if (dropped != nullptr && !frame.ReadOnly && ImGui::BeginDragDropTarget())
		{
			if (ImGuiPayload const* const payload =
			        ImGui::AcceptDragDropPayload(DragDropPayload::VisualScriptElement, ImGuiDragDropFlags_None))
			{
				VisualScriptElementPayload source;
				if (payload->DataSize == sizeof(source))
				{
					std::memcpy(&source, payload->Data, sizeof(source));
					if (source.Graph == context.Graph.GetUUID().GetValue() && source.Kind == static_cast<uint32_t>(element.Kind) &&
					    source.ID != element.ID)
					{
						*dropped = source.ID;
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
		bool const eventGraph = element.Kind == VisualScriptElementKind::Function && element.ID == 0;
		if (!eventGraph && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
		{
			VisualScriptElementPayload const payload{context.Graph.GetUUID().GetValue(), static_cast<uint32_t>(element.Kind), element.ID};
			ImGui::SetDragDropPayload(DragDropPayload::VisualScriptElement, &payload, sizeof(payload));
			ImGui::TextUnformatted(name.c_str());
			ImGui::EndDragDropSource();
		}
		if (!eventGraph && ImGui::BeginPopupContextItem("##menu"))
		{
			SelectVisualScriptElement(context, element);
			bool const editable = !frame.ReadOnly;
			if (ImGui::MenuItem("Open", "Enter"))
			{
				Open(frame, element);
			}
			if (ImGui::MenuItem("Rename", "F2", false, editable))
			{
				m_Renaming = element;
				m_RenameFocus = true;
				m_NameKey = 0;
			}
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editable))
			{
				Duplicate(frame, element);
			}
			if (ImGui::MenuItem("Delete", "Delete", false, editable))
			{
				Delete(frame, element, false);
			}
			ImGui::Separator();
			char const* const find = element.Kind == VisualScriptElementKind::Function      ? "Find Callers"
			                         : element.Kind == VisualScriptElementKind::CustomEvent ? "Find Senders"
			                                                                                : "Find References";
			if (ImGui::MenuItem(find))
			{
				if (element.Kind == VisualScriptElementKind::CustomEvent)
				{
					VisualScriptNode const* const node = FindVisualScriptNode(frame.Document, element.ID);
					frame.Result.FindReferences = VisualScriptReference{VisualScriptReference::Kind::CustomEvent, 0,
					                                                    node != nullptr ? GetCustomEventName(*node) : name};
				}
				else
				{
					frame.Result.FindReferences =
						VisualScriptReference{element.Kind == VisualScriptElementKind::Function ? VisualScriptReference::Kind::Function
					                                                                            : VisualScriptReference::Kind::Variable,
					                          element.ID,
					                          {}};
				}
			}
			ImGui::EndPopup();
		}
		// The row's contents start a frame padding inside the content region (as its right end does), on the selectable's top,
		// so that glyphs reaching left of their origin (the function glyph) are not clipped either.
		ImGui::SetCursorScreenPos(ImVec2(contentX + ImGui::GetStyle().FramePadding.x, min.y));
		return true;
	}

	void VisualScriptSidebar::DrawProblemMarker(Frame& frame, std::vector<VisualScriptDiagnostic> const& problems, bool badge, float right)
	{
		if (problems.empty())
		{
			return;
		}
		size_t const errors = static_cast<size_t>(std::count_if(problems.begin(), problems.end(),
		                                                        [](VisualScriptDiagnostic const& diagnostic)
		                                                        {
																	return diagnostic.Severity == VisualScriptSeverity::Error;
																}));
		uint32_t const color = errors > 0 ? VisualScriptErrorColor : VisualScriptWarningColor;
		std::string const count = std::to_string(problems.size());
		float const height = ImGui::GetFrameHeight();
		float const radius = ImGui::GetFontSize() * 0.25f;
		float const width = badge ? ImGui::CalcTextSize(count.c_str()).x + radius * 4.0f : radius * 2.0f;
		ImVec2 const cursor = ImGui::GetCursorScreenPos();
		ImVec2 const min(right - width, cursor.y + (height - (badge ? ImGui::GetFontSize() : radius * 2.0f)) * 0.5f);
		ImVec2 const max(right, min.y + (badge ? ImGui::GetFontSize() : radius * 2.0f));
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		if (badge)
		{
			drawList->AddRectFilled(min, max, color, ImGui::GetFontSize() * 0.5f);
			drawList->AddText(ImVec2(min.x + radius * 2.0f, min.y), IM_COL32(255, 255, 255, 255), count.c_str());
		}
		else
		{
			drawList->AddCircleFilled(ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), radius, color);
		}
		ImGui::SetCursorScreenPos(ImVec2(min.x, cursor.y));
		ImGui::PushID("problems");
		if (ImGui::InvisibleButton("##marker", ImVec2(std::max(width, 1.0f), height)))
		{
			ImGui::OpenPopup(ProblemsPopupId);
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip())
		{
			for (VisualScriptDiagnostic const& diagnostic : problems)
			{
				ImGui::TextUnformatted(diagnostic.Message.c_str());
			}
			ImGui::EndTooltip();
		}
		if (ImGui::BeginPopup(ProblemsPopupId, ImGuiWindowFlags_NoSavedSettings))
		{
			DrawVisualScriptProblems(frame.Context, problems, m_Commits, frame.Result);
			ImGui::EndPopup();
		}
		ImGui::PopID();
		ImGui::SetCursorScreenPos(cursor);
	}

	void VisualScriptSidebar::DrawEye(Frame& frame, VisualScriptVariable const& variable, float x)
	{
		float const height = ImGui::GetFrameHeight();
		float const width = ImGui::GetFontSize() * 1.2f;
		ImVec2 const cursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(x, cursor.y));
		ImGui::PushID("eye");
		ImGui::BeginDisabled(frame.ReadOnly);
		bool const clicked = ImGui::InvisibleButton("##eye", ImVec2(width, height));
		ImGui::EndDisabled();
		bool const hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		ImVec2 const min = ImGui::GetItemRectMin();
		ImVec2 const max = ImGui::GetItemRectMax();
		m_Eyes.emplace_back(variable.ID, std::pair(glm::vec2(min.x, min.y), glm::vec2(max.x, max.y)));
		bool const exposable = IsVisualScriptPublicType(variable.Type, frame.Library);
		// Why the variable cannot be made public: arrays have a reason of their own (spec-slice3 §3.4.6).
		std::string const refusal = variable.Type.ends_with("[]")
		                                ? std::string(VisualScriptArrayPublicRefusal)
		                                : fmt::format("{} cannot be public: components cannot store it", variable.Type);
		// An eye: shown in the text colour when public, faint on hover otherwise (a click makes it public).
		if (variable.Public || hovered)
		{
			uint32_t const color = ImGui::GetColorU32(variable.Public ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			ImVec2 const center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
			float const radius = ImGui::GetFontSize() * 0.4f;
			ImGui::GetWindowDrawList()->AddEllipse(center, ImVec2(radius, radius * 0.6f), color, 0.0f, 0, 1.0f);
			ImGui::GetWindowDrawList()->AddCircleFilled(center, radius * 0.3f, color);
		}
		if (hovered && ImGui::BeginTooltip())
		{
			if (!exposable && !variable.Public)
			{
				ImGui::TextUnformatted(refusal.c_str());
			}
			else
			{
				ImGui::TextUnformatted(variable.Public ? "Public: set per entity in the inspector (click to make private)"
				                                       : "Private (click to make public)");
			}
			ImGui::EndTooltip();
		}
		ImGui::PopID();
		ImGui::SetCursorScreenPos(cursor);
		if (!clicked)
		{
			return;
		}
		if (!exposable && !variable.Public)
		{
			frame.Result.Notes.push_back(refusal);
			return;
		}
		VisualScriptDocument edited = frame.Document;
		VisualScriptVariable changed = variable;
		changed.Public = !variable.Public;
		Result<bool> const set = VisualScriptEditing::SetVariable(edited, variable.ID, changed, MakeVisualScriptEnumLookup(frame.Library));
		if (!set)
		{
			frame.Result.Notes.push_back(fmt::format("Could not change {}: {}", variable.Name, set.GetError()));
			return;
		}
		m_Commits.Commit(frame.Context, std::move(edited), fmt::format("Make {} {}", variable.Name, changed.Public ? "Public" : "Private"),
		                 0, frame.Result);
	}

	void VisualScriptSidebar::DrawGraphs(Frame& frame)
	{
		VisualScriptDocument const& document = frame.Document;
		bool const library = document.Kind == VisualScriptDocumentKind::Library;
		if (!BeginSection(frame, library ? "Functions" : "Graphs", VisualScriptElementKind::Function, true))
		{
			return;
		}
		ImGui::PushID("graphs");
		float const right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x;
		// The event graph: its problems are those of no function. A library has none, unless nodes or comment boxes are still
		// there to delete.
		std::string const eventGraph = library ? "Event Graph (not used in a library)" : "Event Graph";
		if (OffersVisualScriptEventGraph(document) && Matches(eventGraph))
		{
			ImGui::PushID(0);
			if (DrawRow(frame, {VisualScriptElementKind::Function, 0}, eventGraph, false))
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(eventGraph.c_str());
				ImGui::SameLine();
				std::vector<VisualScriptDiagnostic> problems;
				if (frame.Analysis != nullptr)
				{
					std::copy_if(frame.Analysis->Diagnostics.begin(), frame.Analysis->Diagnostics.end(), std::back_inserter(problems),
					             [](VisualScriptDiagnostic const& diagnostic)
					             {
									 return diagnostic.Function == 0 && (diagnostic.Node || diagnostic.Link);
								 });
				}
				DrawProblemMarker(frame, problems, true, right);
				ImGui::NewLine();
			}
			ImGui::PopID();
		}
		for (VisualScriptFunction const& function : document.Functions)
		{
			if (!Matches(function.Name))
			{
				continue;
			}
			ImGui::PushID(static_cast<int>(function.ID));
			if (DrawRow(frame, {VisualScriptElementKind::Function, function.ID}, function.Name, true))
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("\xC6\x92");
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
				ImGui::TextUnformatted(function.Name.c_str());
				ImGui::SameLine();
				std::vector<VisualScriptDiagnostic> problems;
				if (frame.Analysis != nullptr)
				{
					std::copy_if(frame.Analysis->Diagnostics.begin(), frame.Analysis->Diagnostics.end(), std::back_inserter(problems),
					             [&function](VisualScriptDiagnostic const& diagnostic)
					             {
									 return diagnostic.Function == function.ID;
								 });
				}
				DrawProblemMarker(frame, problems, true, right);
				ImGui::NewLine();
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	void VisualScriptSidebar::DrawVariables(Frame& frame)
	{
		VisualScriptDocument const& document = frame.Document;
		if (!BeginSection(frame, "Variables", VisualScriptElementKind::Variable, document.Kind != VisualScriptDocumentKind::Library))
		{
			return;
		}
		if (document.Variables.empty())
		{
			ImGui::TextDisabled("No variables yet: + adds one");
			return;
		}
		ImGui::PushID("variables");
		float const right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x;
		std::optional<std::pair<uint32_t, uint32_t>> moved;
		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (!Matches(variable.Name))
			{
				continue;
			}
			ImGui::PushID(static_cast<int>(variable.ID));
			VisualScriptElement const element{VisualScriptElementKind::Variable, variable.ID};
			std::optional<uint32_t> dropped;
			bool const drawn = DrawRow(frame, element, variable.Name, true, &dropped);
			if (dropped)
			{
				// The dropped variable goes before this one.
				moved.emplace(*dropped, variable.ID);
			}
			if (drawn)
			{
				ImVec2 const start = ImGui::GetCursorScreenPos();
				float const pill = ImGui::GetFontSize() * 0.9f;
				float const pillHeight = ImGui::GetFontSize() * 0.5f;
				float const top = start.y + (ImGui::GetFrameHeight() - pillHeight) * 0.5f;
				ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x, top), ImVec2(start.x + pill, top + pillHeight),
				                                          GetVisualScriptTypeColor(variable.Type, frame.Library), pillHeight * 0.5f);
				ImGui::SetCursorScreenPos(ImVec2(start.x + ImGui::GetStyle().FramePadding.x + pill, start.y));
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(variable.Name.c_str());
				float const nameEnd = ImGui::GetItemRectMax().x;
				ImGui::SameLine();
				// Left to right: the type pill, the name, the live value's cell, the eye, the type and the problem marker. The
				// cell never reaches the eye or the type column (spec-slice4 §15.5).
				std::vector<VisualScriptDiagnostic> const problems = GetVisualScriptVariableProblems(frame.Analysis, variable.ID);
				float const marker = problems.empty() ? 0.0f : ImGui::GetFontSize() * 0.75f;
				std::string const type = DescribeVisualScriptType(frame.Library, variable.Type);
				float const typeWidth = ImGui::CalcTextSize(type.c_str()).x;
				float const typeX = right - marker - typeWidth;
				float const spacing = ImGui::GetStyle().ItemSpacing.x;
				float const eyeX = typeX - ImGui::GetFontSize() * 1.2f - spacing;
				ImGui::GetWindowDrawList()->AddText(ImVec2(typeX, start.y + ImGui::GetStyle().FramePadding.y),
				                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), type.c_str());
				m_TypeRects.emplace_back(
					variable.ID, std::pair(glm::vec2(typeX, start.y), glm::vec2(typeX + typeWidth, start.y + ImGui::GetFrameHeight())));
				if (m_LiveCells.contains(variable.ID))
				{
					DrawLiveCell(frame, variable, std::max(nameEnd + spacing, start.x + LiveCellIndent * ImGui::GetFontSize()),
					             eyeX - spacing, start.y);
				}
				DrawEye(frame, variable, eyeX);
				DrawProblemMarker(frame, problems, false, right);
				ImGui::NewLine();
			}
			ImGui::PopID();
		}
		ImGui::PopID();
		if (moved)
		{
			std::vector<uint32_t> order;
			for (VisualScriptVariable const& variable : document.Variables)
			{
				if (variable.ID == moved->first)
				{
					continue;
				}
				if (variable.ID == moved->second)
				{
					order.push_back(moved->first);
				}
				order.push_back(variable.ID);
			}
			VisualScriptDocument edited = document;
			if (VisualScriptEditing::ReorderVariables(edited, order))
			{
				m_Commits.Commit(frame.Context, std::move(edited), "Reorder Variables", 0, frame.Result);
			}
		}
	}

	void VisualScriptSidebar::DrawLocals(Frame& frame)
	{
		VisualScriptDocument const& document = frame.Document;
		uint32_t const shown = frame.Context.Model.GetFunction();
		auto const function = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                   [shown](VisualScriptFunction const& candidate)
		                                   {
											   return candidate.ID == shown;
										   });
		if (function == document.Functions.end() || !BeginSection(frame, "Locals", VisualScriptElementKind::Local, true))
		{
			return;
		}
		if (function->Locals.empty())
		{
			ImGui::TextDisabled("No locals yet: + adds one");
			return;
		}
		ImGui::PushID("locals");
		float const right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x;
		for (VisualScriptLocal const& local : function->Locals)
		{
			if (!Matches(local.Name))
			{
				continue;
			}
			ImGui::PushID(static_cast<int>(local.ID));
			if (DrawRow(frame, {VisualScriptElementKind::Local, local.ID}, local.Name, true))
			{
				ImVec2 const start = ImGui::GetCursorScreenPos();
				float const pill = ImGui::GetFontSize() * 0.9f;
				float const pillHeight = ImGui::GetFontSize() * 0.5f;
				float const top = start.y + (ImGui::GetFrameHeight() - pillHeight) * 0.5f;
				ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x, top), ImVec2(start.x + pill, top + pillHeight),
				                                          GetVisualScriptTypeColor(local.Type, frame.Library), pillHeight * 0.5f);
				ImGui::SetCursorScreenPos(ImVec2(start.x + ImGui::GetStyle().FramePadding.x + pill, start.y));
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(local.Name.c_str());
				ImGui::SameLine();
				std::vector<VisualScriptDiagnostic> const problems = GetVisualScriptVariableProblems(frame.Analysis, local.ID);
				float const marker = problems.empty() ? 0.0f : ImGui::GetFontSize() * 0.75f;
				std::string const type = DescribeVisualScriptType(frame.Library, local.Type);
				ImGui::GetWindowDrawList()->AddText(
					ImVec2(right - marker - ImGui::CalcTextSize(type.c_str()).x, start.y + ImGui::GetStyle().FramePadding.y),
					ImGui::GetColorU32(ImGuiCol_TextDisabled), type.c_str());
				DrawProblemMarker(frame, problems, false, right);
				ImGui::NewLine();
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	void VisualScriptSidebar::DrawCustomEvents(Frame& frame)
	{
		VisualScriptDocument const& document = frame.Document;
		if (!BeginSection(frame, "Custom Events", VisualScriptElementKind::CustomEvent, document.Kind != VisualScriptDocumentKind::Library))
		{
			return;
		}
		ImGui::PushID("events");
		float const right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x;
		bool any = false;
		for (VisualScriptNode const& node : document.Graph.Nodes)
		{
			if (node.Type != "Event.Custom")
			{
				continue;
			}
			any = true;
			std::string const name = GetCustomEventName(node);
			if (!Matches(name))
			{
				continue;
			}
			ImGui::PushID(static_cast<int>(node.ID));
			if (DrawRow(frame, {VisualScriptElementKind::CustomEvent, node.ID}, name, true))
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(name.empty() ? "(unnamed)" : name.c_str());
				// The parameters' types.
				std::string parameters;
				auto const list = node.Settings.is_object() ? node.Settings.find("Parameters") : node.Settings.end();
				if (node.Settings.is_object() && list != node.Settings.end() && list->is_array())
				{
					for (Json const& parameter : *list)
					{
						auto const type = parameter.is_object() ? parameter.find("Type") : parameter.end();
						if (parameter.is_object() && type != parameter.end() && type->is_string())
						{
							parameters +=
								(parameters.empty() ? "" : ", ") + DescribeVisualScriptType(frame.Library, type->get<std::string>());
						}
					}
				}
				ImGui::SameLine();
				ImGui::TextDisabled("(%s)", parameters.c_str());
				ImGui::SameLine();
				DrawProblemMarker(frame, GetVisualScriptNodeProblems(frame.Analysis, node.ID), false, right);
				ImGui::NewLine();
			}
			ImGui::PopID();
		}
		if (!any)
		{
			ImGui::TextDisabled("No custom events yet: + adds one");
		}
		ImGui::PopID();
	}

	void VisualScriptSidebar::Rename(Frame& frame, VisualScriptElement element, std::string const& name)
	{
		VisualScriptDocument const& document = frame.Document;
		VisualScriptEnumLookup const enums = MakeVisualScriptEnumLookup(frame.Library);
		// The document with the element renamed, as its operation writes it; the operation's error when it refuses.
		auto const renamed = [&](std::string const& newName, VisualScriptDocument& edited) -> Result<uint32_t>
		{
			if (!VisualScriptSerializer::IsValidName(newName))
			{
				return Error{GetVisualScriptNameRuleMessage()};
			}
			switch (element.Kind)
			{
				case VisualScriptElementKind::Variable:
				{
					auto const variable = std::find_if(edited.Variables.begin(), edited.Variables.end(),
					                                   [&element](VisualScriptVariable const& candidate)
					                                   {
														   return candidate.ID == element.ID;
													   });
					if (variable == edited.Variables.end())
					{
						return Error{std::string("the variable is gone")};
					}
					VisualScriptVariable changed = *variable;
					changed.Name = newName;
					Result<bool> const set = VisualScriptEditing::SetVariable(edited, element.ID, changed, enums);
					return set ? Result<uint32_t>(0u) : Result<uint32_t>(Error{set.GetError()});
				}
				case VisualScriptElementKind::Local:
					for (VisualScriptFunction const& function : edited.Functions)
					{
						auto const local = std::find_if(function.Locals.begin(), function.Locals.end(),
						                                [&element](VisualScriptLocal const& candidate)
						                                {
															return candidate.ID == element.ID;
														});
						if (local != function.Locals.end())
						{
							VisualScriptLocal changed = *local;
							changed.Name = newName;
							Result<bool> const set = VisualScriptEditing::SetLocal(edited, function.ID, element.ID, changed, enums);
							return set ? Result<uint32_t>(0u) : Result<uint32_t>(Error{set.GetError()});
						}
					}
					return Error{std::string("the local is gone")};
				case VisualScriptElementKind::Function:
				{
					Result<void> const set = VisualScriptEditing::RenameFunction(edited, element.ID, newName);
					return set ? Result<uint32_t>(0u) : Result<uint32_t>(Error{set.GetError()});
				}
				case VisualScriptElementKind::CustomEvent:
					if (VisualScriptEditing::IsCustomEventNameTaken(edited, newName))
					{
						return Error{fmt::format("a custom event is named {} already", newName)};
					}
					return VisualScriptEditing::RenameCustomEvent(edited, element.ID, newName, frame.Context.Graph);
			}
			return Error{std::string("nothing to rename")};
		};

		ImGui::SetNextItemWidth(-FLT_MIN);
		VisualScriptNameField::Outcome const outcome = m_Names.Draw(
			"##rename", name,
			[&](std::string_view candidate) -> std::optional<std::string>
			{
				VisualScriptDocument copy = document;
				Result<uint32_t> const done = renamed(std::string(candidate), copy);
				return done ? std::nullopt : std::optional<std::string>(done.GetError());
			},
			std::exchange(m_RenameFocus, false));
		ImVec2 const min = ImGui::GetItemRectMin();
		ImVec2 const max = ImGui::GetItemRectMax();
		m_Rows.push_back({element, glm::vec2(min.x, min.y), glm::vec2(max.x, max.y)});
		if (outcome.Committed)
		{
			VisualScriptDocument edited = document;
			Result<uint32_t> const done = renamed(*outcome.Committed, edited);
			if (done)
			{
				std::string action = element.Kind == VisualScriptElementKind::CustomEvent
				                         ? fmt::format("Rename Event {} to {}", name, *outcome.Committed)
				                         : fmt::format("Rename {} to {}", name, *outcome.Committed);
				if (element.Kind == VisualScriptElementKind::CustomEvent && done.GetValue() > 0)
				{
					action += fmt::format(" (and {})", CountOf(done.GetValue(), "sender", "senders"));
				}
				m_Commits.Commit(frame.Context, std::move(edited), std::move(action), m_NameKey, frame.Result);
				if (element.Kind == VisualScriptElementKind::CustomEvent)
				{
					frame.Result.Notes.push_back(
						fmt::format("Senders of {} in other scripts still send {}: Find Senders lists them", name, name));
				}
			}
		}
		else if (outcome.Refused)
		{
			frame.Result.Notes.push_back(fmt::format("{} keeps its name: {}", name, *outcome.Refused));
		}
		if (!m_Names.IsEditing())
		{
			// The field is left: the step of the element's creation and first name ends with it.
			m_Renaming.reset();
			m_NameKey = 0;
		}
	}

	void VisualScriptSidebar::Add(Frame& frame, VisualScriptElementKind kind)
	{
		VisualScriptPaneContext& context = frame.Context;
		VisualScriptEditorModel const& model = context.Model;
		VisualScriptDocument edited = frame.Document;
		uint64_t const key = MakeVisualScriptPaneNameMergeKey(context.Shared);
		switch (kind)
		{
			case VisualScriptElementKind::Variable:
			{
				std::string const name =
					VisualScriptEditing::MakeUniqueVisualScriptName(NewVariableName,
				                                                    [&edited](std::string_view candidate)
				                                                    {
																		return VisualScriptEditing::IsVariableNameTaken(edited, candidate);
																	});
				Result<uint32_t> const added = VisualScriptEditing::AddVariable(edited, name, context.Settings.GetLastVariableType());
				if (!added)
				{
					frame.Result.Notes.push_back(fmt::format("Could not add a variable: {}", added.GetError()));
					return;
				}
				m_Commits.Commit(context, std::move(edited), "Add Variable", key, frame.Result);
				SelectVisualScriptElement(context, {kind, added.GetValue()});
				m_RenameCreated = VisualScriptElement{kind, added.GetValue()};
				m_NameKey = key;
				return;
			}
			case VisualScriptElementKind::Local:
			{
				uint32_t const function = model.GetFunction();
				std::string const name = VisualScriptEditing::MakeUniqueVisualScriptName(NewLocalName,
				                                                                         [&edited, function](std::string_view candidate)
				                                                                         {
																							 return VisualScriptEditing::IsLocalNameTaken(
																								 edited, function, candidate);
																						 });
				Result<uint32_t> const added =
					VisualScriptEditing::AddLocal(edited, function, name, context.Settings.GetLastVariableType());
				if (!added)
				{
					frame.Result.Notes.push_back(fmt::format("Could not add a local: {}", added.GetError()));
					return;
				}
				m_Commits.Commit(context, std::move(edited), "Add Local", key, frame.Result);
				SelectVisualScriptElement(context, {kind, added.GetValue()});
				m_RenameCreated = VisualScriptElement{kind, added.GetValue()};
				m_NameKey = key;
				return;
			}
			case VisualScriptElementKind::Function:
			{
				std::string const name =
					VisualScriptEditing::MakeUniqueVisualScriptName(NewFunctionName,
				                                                    [&edited](std::string_view candidate)
				                                                    {
																		return VisualScriptEditing::IsFunctionNameTaken(edited, candidate);
																	});
				Result<uint32_t> const added = VisualScriptEditing::AddFunction(edited, name);
				if (!added)
				{
					frame.Result.Notes.push_back(fmt::format("Could not add a function: {}", added.GetError()));
					return;
				}
				m_Commits.Commit(context, std::move(edited), "Add Function", key, frame.Result);
				// The window shows it once the commit made it.
				frame.Result.ShowFunction = added.GetValue();
				SelectVisualScriptElement(context, {kind, added.GetValue()});
				m_RenameCreated = VisualScriptElement{kind, added.GetValue()};
				m_NameKey = key;
				return;
			}
			case VisualScriptElementKind::CustomEvent:
			{
				std::string const name = VisualScriptEditing::MakeUniqueVisualScriptName(
					NewEventName,
					[&edited](std::string_view candidate)
					{
						return VisualScriptEditing::IsCustomEventNameTaken(edited, candidate);
					});
				// At the centre of the event graph's view.
				glm::vec2 center = model.GetView().GetState().Center;
				if (model.GetFunction() != 0)
				{
					VisualScriptGraphViews const* const views = context.Settings.FindGraph(context.Graph);
					VisualScriptViewState const* const view = views != nullptr ? views->FindView(0) : nullptr;
					center = view != nullptr ? view->Center : glm::vec2(0.0f);
				}
				Result<uint32_t> const added =
					VisualScriptEditing::AddNode(edited, 0, "Event.Custom", Json::object({{"Name", name}}), SnapVisualScriptToGrid(center));
				if (!added)
				{
					frame.Result.Notes.push_back(fmt::format("Could not add a custom event: {}", added.GetError()));
					return;
				}
				m_Commits.Commit(context, std::move(edited), "Add Custom Event", 0, frame.Result);
				// The event graph shows it selected, and Details gives its name the keyboard.
				frame.Result.Open = MakeVisualScriptJump(context.Graph, 0, {added.GetValue()});
				frame.Result.FocusSetting = VisualScriptSettingFocus{added.GetValue(), "Name", std::nullopt};
				return;
			}
		}
	}

	void VisualScriptSidebar::Open(Frame& frame, VisualScriptElement element)
	{
		VisualScriptPaneContext& context = frame.Context;
		switch (element.Kind)
		{
			case VisualScriptElementKind::Function:
				frame.Result.ShowFunction = element.ID;
				return;
			case VisualScriptElementKind::CustomEvent:
				frame.Result.Open = MakeVisualScriptJump(context.Graph, 0, {element.ID});
				return;
			case VisualScriptElementKind::Variable:
			case VisualScriptElementKind::Local:
			{
				uint32_t const shown = context.Model.GetFunction();
				VisualScriptGraph const* const graph = FindVisualScriptGraph(frame.Document, shown);
				std::vector<uint32_t> const uses =
					graph != nullptr ? FindVisualScriptUses(*graph, element.Kind, element.ID) : std::vector<uint32_t>();
				if (uses.empty())
				{
					frame.Result.Notes.push_back(fmt::format("No node of {} uses it", GetVisualScriptGraphName(frame.Document, shown)));
					return;
				}
				frame.Result.SelectAndFrame = uses;
				return;
			}
		}
	}

	void VisualScriptSidebar::Delete(Frame& frame, VisualScriptElement element, bool confirmed)
	{
		VisualScriptPaneContext& context = frame.Context;
		VisualScriptDocument const& document = frame.Document;
		if (frame.ReadOnly || (element.Kind == VisualScriptElementKind::Function && element.ID == 0))
		{
			return;
		}
		std::string name;
		uint32_t uses = 0;
		switch (element.Kind)
		{
			case VisualScriptElementKind::Variable:
			{
				auto const variable = std::find_if(document.Variables.begin(), document.Variables.end(),
				                                   [&element](VisualScriptVariable const& candidate)
				                                   {
													   return candidate.ID == element.ID;
												   });
				if (variable == document.Variables.end())
				{
					return;
				}
				name = variable->Name;
				uses = CountVisualScriptUses(document, element.Kind, element.ID);
				break;
			}
			case VisualScriptElementKind::Local:
				for (VisualScriptFunction const& function : document.Functions)
				{
					for (VisualScriptLocal const& local : function.Locals)
					{
						if (local.ID == element.ID)
						{
							name = local.Name;
							uses = static_cast<uint32_t>(FindVisualScriptUses(function.Graph, element.Kind, element.ID).size());
						}
					}
				}
				if (name.empty())
				{
					return;
				}
				break;
			case VisualScriptElementKind::Function:
			{
				auto const function = std::find_if(document.Functions.begin(), document.Functions.end(),
				                                   [&element](VisualScriptFunction const& candidate)
				                                   {
													   return candidate.ID == element.ID;
												   });
				if (function == document.Functions.end())
				{
					return;
				}
				name = function->Name;
				uses = CountVisualScriptUses(document, element.Kind, element.ID);
				break;
			}
			case VisualScriptElementKind::CustomEvent:
			{
				VisualScriptNode const* const node = FindVisualScriptNode(document, element.ID);
				if (node == nullptr)
				{
					return;
				}
				name = GetCustomEventName(*node);
				break;
			}
		}
		// What other scripts lose (spec-slice3 §10.6): nodes naming the variable or the library function through their Graph
		// setting, this script's own nodes that read the variable on other entities included. They are not deleted.
		VisualScriptOtherUses others;
		if (element.Kind == VisualScriptElementKind::Variable || element.Kind == VisualScriptElementKind::Function)
		{
			others = CountVisualScriptOtherUses(context.Model.GetProjectGraphs(), context.Graph, element.Kind, element.ID);
		}
		uint32_t const selfUses =
			element.Kind == VisualScriptElementKind::Variable ? CountVisualScriptSelfUses(document, context.Graph, element.ID) : 0;
		if (selfUses > 0)
		{
			others.Nodes += selfUses;
			others.Scripts++;
		}
		if ((uses > 0 || others.Nodes > 0) && !confirmed)
		{
			// Asks first (the editor's modal shape).
			m_Delete = element;
			m_DeleteTitle = fmt::format("Delete {}?", name);
			bool const function = element.Kind == VisualScriptElementKind::Function;
			if (others.Nodes == 0)
			{
				m_DeleteMessage =
					function ? fmt::format("{} is called by {}; they are deleted too.", name, CountOf(uses, "node", "nodes"))
							 : fmt::format("{} {} it; they are deleted too.", CountOf(uses, "node", "nodes"), uses == 1 ? "uses" : "use");
			}
			else
			{
				std::string const here = uses > 0 ? fmt::format("{} here {} it (they are deleted too). ", CountOf(uses, "node", "nodes"),
				                                                uses == 1 ? "uses" : "use")
				                                  : std::string();
				std::string const elsewhere = fmt::format(
					"{} in {} {} it", CountOf(others.Nodes, "node", "nodes"),
					CountOf(others.Scripts, selfUses > 0 ? "script" : "other script", selfUses > 0 ? "scripts" : "other scripts"),
					others.Nodes == 1 ? "uses" : "use");
				m_DeleteMessage = fmt::format("{}{}: they will show errors until fixed.", here, elsewhere);
			}
			if (others.Nodes > 0)
			{
				VisualScriptReference reference;
				reference.ReferenceKind = function ? VisualScriptReference::Kind::Function : VisualScriptReference::Kind::Variable;
				reference.ID = element.ID;
				reference.Graph = context.Graph;
				m_DeleteReferences = std::move(reference);
			}
			else
			{
				m_DeleteReferences.reset();
			}
			m_OpenDelete = true;
			return;
		}
		VisualScriptDocument edited = document;
		// Without the runtime only reroutes bridge (RemoveElements).
		std::optional<VisualScriptNodePins> pins;
		if (frame.Library != nullptr)
		{
			pins = MakeDocumentPins(context.Model, *frame.Library, document);
		}
		VisualScriptNodePins const* const rows = pins ? &*pins : nullptr;
		std::string action;
		Result<uint32_t> removed = 0u;
		switch (element.Kind)
		{
			case VisualScriptElementKind::Variable:
				removed = VisualScriptEditing::RemoveVariable(edited, element.ID, true, rows);
				action = fmt::format("Delete Variable {}", name);
				break;
			case VisualScriptElementKind::Local:
			{
				uint32_t owner = 0;
				for (VisualScriptFunction const& function : document.Functions)
				{
					if (std::any_of(function.Locals.begin(), function.Locals.end(),
					                [&element](VisualScriptLocal const& local)
					                {
										return local.ID == element.ID;
									}))
					{
						owner = function.ID;
					}
				}
				removed = VisualScriptEditing::RemoveLocal(edited, owner, element.ID, true, rows);
				action = fmt::format("Delete Local {}", name);
				break;
			}
			case VisualScriptElementKind::Function:
				removed = VisualScriptEditing::RemoveFunction(edited, element.ID, true, rows);
				action = fmt::format("Delete Function {}", name);
				break;
			case VisualScriptElementKind::CustomEvent:
			{
				uint32_t const node = element.ID;
				Result<VisualScriptRemoval> const removal = VisualScriptEditing::RemoveElements(edited, 0, std::span(&node, 1), rows, true);
				removed = removal ? Result<uint32_t>(0u) : Result<uint32_t>(Error{removal.GetError()});
				action = fmt::format("Delete Custom Event {}", name);
				break;
			}
		}
		if (!removed)
		{
			frame.Result.Notes.push_back(fmt::format("Could not delete {}: {}", name, removed.GetError()));
			return;
		}
		if (removed.GetValue() > 0)
		{
			action += element.Kind == VisualScriptElementKind::Function
			              ? fmt::format(" (and {})", CountOf(removed.GetValue(), "call", "calls"))
			              : fmt::format(" (and {})", CountOf(removed.GetValue(), "node", "nodes"));
		}
		context.Shared.Element.reset();
		m_Commits.Commit(context, std::move(edited), std::move(action), 0, frame.Result);
	}

	void VisualScriptSidebar::Duplicate(Frame& frame, VisualScriptElement element)
	{
		VisualScriptPaneContext& context = frame.Context;
		if (frame.ReadOnly)
		{
			return;
		}
		VisualScriptDocument edited = frame.Document;
		switch (element.Kind)
		{
			case VisualScriptElementKind::Variable:
			{
				Result<uint32_t> const copy = VisualScriptEditing::DuplicateVariable(edited, element.ID);
				if (!copy)
				{
					frame.Result.Notes.push_back(fmt::format("Could not duplicate: {}", copy.GetError()));
					return;
				}
				std::string const name = std::find_if(frame.Document.Variables.begin(), frame.Document.Variables.end(),
				                                      [&element](VisualScriptVariable const& variable)
				                                      {
														  return variable.ID == element.ID;
													  })
				                             ->Name;
				m_Commits.Commit(context, std::move(edited), fmt::format("Duplicate {}", name), 0, frame.Result);
				SelectVisualScriptElement(context, {element.Kind, copy.GetValue()});
				return;
			}
			case VisualScriptElementKind::Local:
				for (VisualScriptFunction const& function : frame.Document.Functions)
				{
					for (VisualScriptLocal const& local : function.Locals)
					{
						if (local.ID != element.ID)
						{
							continue;
						}
						std::string const name = VisualScriptEditing::MakeUniqueVisualScriptName(
							local.Name,
							[&edited, &function](std::string_view candidate)
							{
								return VisualScriptEditing::IsLocalNameTaken(edited, function.ID, candidate);
							});
						Result<uint32_t> const copy = VisualScriptEditing::AddLocal(edited, function.ID, name, local.Type);
						if (!copy)
						{
							frame.Result.Notes.push_back(fmt::format("Could not duplicate: {}", copy.GetError()));
							return;
						}
						VisualScriptLocal changed = local;
						changed.ID = copy.GetValue();
						changed.Name = name;
						if (Result<bool> const set = VisualScriptEditing::SetLocal(edited, function.ID, copy.GetValue(), changed,
						                                                           MakeVisualScriptEnumLookup(frame.Library));
						    !set)
						{
							frame.Result.Notes.push_back(fmt::format("Could not duplicate: {}", set.GetError()));
							return;
						}
						m_Commits.Commit(context, std::move(edited), fmt::format("Duplicate {}", local.Name), 0, frame.Result);
						SelectVisualScriptElement(context, {element.Kind, copy.GetValue()});
						return;
					}
				}
				return;
			case VisualScriptElementKind::Function:
			{
				Result<uint32_t> const copy = VisualScriptEditing::DuplicateFunction(edited, element.ID);
				if (!copy)
				{
					frame.Result.Notes.push_back(fmt::format("Could not duplicate: {}", copy.GetError()));
					return;
				}
				m_Commits.Commit(context, std::move(edited),
				                 fmt::format("Duplicate {}", GetVisualScriptGraphName(frame.Document, element.ID)), 0, frame.Result);
				SelectVisualScriptElement(context, {element.Kind, copy.GetValue()});
				return;
			}
			case VisualScriptElementKind::CustomEvent:
			{
				VisualScriptNode const* const node = FindVisualScriptNode(frame.Document, element.ID);
				if (node == nullptr)
				{
					return;
				}
				std::string const name = VisualScriptEditing::MakeUniqueVisualScriptName(
					GetCustomEventName(*node),
					[&edited](std::string_view candidate)
					{
						return VisualScriptEditing::IsCustomEventNameTaken(edited, candidate);
					});
				Json settings = node->Settings;
				settings["Name"] = name;
				glm::vec2 const position = node->Position.value_or(glm::vec2(0.0f)) + glm::vec2(VisualScriptCanvasMetrics::GridSize * 2.0f);
				Result<uint32_t> const copy = VisualScriptEditing::AddNode(edited, 0, node->Type, std::move(settings), position);
				if (!copy)
				{
					frame.Result.Notes.push_back(fmt::format("Could not duplicate: {}", copy.GetError()));
					return;
				}
				m_Commits.Commit(context, std::move(edited), fmt::format("Duplicate Custom Event {}", GetCustomEventName(*node)), 0,
				                 frame.Result);
				SelectVisualScriptElement(context, {element.Kind, copy.GetValue()});
				return;
			}
		}
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptSidebar::FindDeleteButton(std::string_view label) const
	{
		for (auto const& [name, rect] : m_DeleteButtons)
		{
			if (name == label)
			{
				return rect;
			}
		}
		return std::nullopt;
	}

	void VisualScriptSidebar::DrawDeleteModal(Frame& frame)
	{
		if (m_OpenDelete)
		{
			ImGui::OpenPopup(DeleteModalId);
			m_OpenDelete = false;
		}
		std::string const title = m_DeleteTitle + DeleteModalId;
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			// Closed without an answer (the window was hidden): nothing is deleted.
			m_Delete.reset();
			m_DeleteButtons.clear();
			return;
		}
		ImGui::TextUnformatted(m_DeleteMessage.c_str());
		ImGui::Spacing();
		m_DeleteButtons.clear();
		auto const button = [this](char const* label)
		{
			bool const pressed = ImGui::Button(label);
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			m_DeleteButtons.emplace_back(label, std::pair(glm::vec2(min.x, min.y), glm::vec2(max.x, max.y)));
			return pressed;
		};
		bool const confirm =
			button("Delete") || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		// Other scripts' nodes are listed in the find drawer instead (nothing is deleted).
		bool find = false;
		if (m_DeleteReferences)
		{
			ImGui::SameLine();
			find = button("Find References");
		}
		ImGui::SameLine();
		bool const cancel = button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		if (confirm || cancel || find)
		{
			ImGui::CloseCurrentPopup();
			std::optional<VisualScriptElement> const element = std::exchange(m_Delete, std::nullopt);
			if (confirm && element)
			{
				Delete(frame, *element, true);
			}
			else if (find && m_DeleteReferences)
			{
				frame.Result.FindReferences = m_DeleteReferences;
			}
			m_DeleteReferences.reset();
		}
		ImGui::EndPopup();
	}

	void VisualScriptSidebar::UpdateLiveValues(Frame& frame)
	{
		VisualScriptPaneContext& context = frame.Context;
		if (!context.Playing || !context.ReadLiveValues)
		{
			m_LiveCells.clear();
			m_LiveEntity = UUID::Invalid();
			m_LiveTime = -1.0e30;
			return;
		}
		if (context.Time - m_LiveTime < LiveValueInterval)
		{
			return;
		}
		m_LiveTime = context.Time;
		Result<VisualScriptLiveValues> const values = context.ReadLiveValues(context.ContextEntity);
		if (!values)
		{
			m_LiveCells.clear();
			m_LiveEntity = UUID::Invalid();
			return;
		}
		// Every variable the document has (private ones too: spec-slice4 §15.5), formatted as the debugger shows values; one
		// that differs from the previous read of the same entity flashes.
		VisualScriptEnumLookup const enums = MakeVisualScriptEnumLookup(frame.Library);
		std::function<std::string(UUID)> const describe = context.DescribeEntity ? context.DescribeEntity
		                                                                         : std::function<std::string(UUID)>(
																					   [](UUID entity)
																					   {
																						   return entity.ToString();
																					   });
		VisualScriptAssetNameLookup const assetName = [](AssetHandle handle)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::IsInitialized() ? AssetManager::GetMetadata(handle) : std::nullopt;
			return metadata ? (metadata->Path.empty() ? metadata->Name : metadata->Path) : handle.ToString();
		};
		bool const sameEntity = values.GetValue().Entity == m_LiveEntity;
		std::map<uint32_t, LiveCell> cells;
		for (VisualScriptDebugValue const& value : values.GetValue().Variables)
		{
			auto const declared = std::find_if(frame.Document.Variables.begin(), frame.Document.Variables.end(),
			                                   [&value](VisualScriptVariable const& variable)
			                                   {
												   return variable.ID == value.ID && variable.Name == value.Name;
											   });
			if (declared == frame.Document.Variables.end())
			{
				continue;
			}
			LiveCell cell;
			cell.Value = value;
			cell.Cell = FormatVisualScriptDebugValue(value, frame.Library, enums, describe, assetName);
			if (auto const previous = m_LiveCells.find(value.ID); previous != m_LiveCells.end() && sameEntity)
			{
				cell.Changed = previous->second.Value != value ? context.Time : previous->second.Changed;
			}
			cells.emplace(value.ID, std::move(cell));
		}
		m_LiveCells = std::move(cells);
		m_LiveEntity = values.GetValue().Entity;
	}

	void VisualScriptSidebar::DrawLiveCell(Frame& frame, VisualScriptVariable const& variable, float x, float right, float top)
	{
		LiveCell const& live = m_LiveCells.at(variable.ID);
		VisualScriptValueCell const& cell = live.Cell;
		VisualScriptPaneContext const& context = frame.Context;
		ImGuiStyle const& style = ImGui::GetStyle();
		float const font = ImGui::GetFontSize();
		float const height = ImGui::GetFrameHeight();
		float const radius = font * 0.18f;
		std::string_view const ellipsis = VisualScriptDebugValueTexts::Ellipsis;
		if (right - x <
		    radius * 2.0f + style.ItemInnerSpacing.x + ImGui::CalcTextSize(ellipsis.data(), ellipsis.data() + ellipsis.size()).x)
		{
			// No room left between the name and the eye even for the dot and an ellipsis.
			return;
		}
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		ImVec2 const cursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(x, top));
		ImGui::PushID("liveCell");
		// A colour's swatch is drawn over the cell later: it takes the hover there, and shows ImGui's colour tooltip
		// (ItemHoverable refuses a later item over one that does not allow overlap, imgui.cpp:5155).
		ImGui::SetNextItemAllowOverlap();
		bool const clicked = ImGui::InvisibleButton("##cell", ImVec2(right - x, height));
		bool const hovered = ImGui::IsItemHovered();
		m_LiveCellRects.emplace_back(variable.ID, std::pair(glm::vec2(x, top), glm::vec2(right, top + height)));
		// A value that changed flashes in the play colour (held while Reduce Motion is on).
		double const since = context.Time - live.Changed;
		if (since >= 0.0 && since < LiveFlashSeconds)
		{
			float const fade = context.Settings.IsReduceMotion() ? 1.0f : 1.0f - static_cast<float>(since / LiveFlashSeconds);
			ImVec4 flash = ImGui::ColorConvertU32ToFloat4(VisualScriptPlayColor);
			flash.w *= 0.3f * fade;
			drawList->AddRectFilled(ImVec2(x, top), ImVec2(right, top + height), ImGui::ColorConvertFloat4ToU32(flash),
			                        style.FrameRounding);
		}
		// The play-coloured dot that opens the cell.
		drawList->AddCircleFilled(ImVec2(x + radius, top + height * 0.5f), radius, VisualScriptPlayColor);
		float left = x + radius * 2.0f + style.ItemInnerSpacing.x;
		float const textY = top + style.FramePadding.y;
		ImU32 const dimmed = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		ImU32 const normal = cell.Dimmed || cell.Kind == VisualScriptValueCellKind::None ? dimmed : ImGui::GetColorU32(ImGuiCol_Text);
		// For a few seconds after a receipt: what it did to the variable, before the value.
		bool const reset =
			std::find(context.ResetVariables.begin(), context.ResetVariables.end(), variable.Name) != context.ResetVariables.end();
		bool const added = std::find(context.NewVariables.begin(), context.NewVariables.end(), variable.Name) != context.NewVariables.end();
		if (reset || added)
		{
			std::string_view const label = reset ? VisualScriptLiveTexts::Reset : VisualScriptLiveTexts::New;
			drawList->AddText(ImVec2(left, textY), dimmed, label.data(), label.data() + label.size());
			left += ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + style.ItemInnerSpacing.x;
			m_LiveCellLabels.emplace_back(variable.ID, label);
		}
		bool cut = false;
		if (cell.Kind == VisualScriptValueCellKind::Swatch && cell.Color)
		{
			// A colour is its swatch alone, with ImGui's colour tooltip.
			float const size = std::max(height - 4.0f, 1.0f);
			ImGui::SetCursorScreenPos(ImVec2(left, top + (height - size) * 0.5f));
			ImGui::ColorButton("##swatch", ImGui::ColorConvertU32ToFloat4(*cell.Color), ImGuiColorEditFlags_AlphaPreviewHalf,
			                   ImVec2(size, size));
		}
		else
		{
			if (cell.Kind == VisualScriptValueCellKind::Entity)
			{
				// An entity's glyph (a small box), then its name.
				float const box = font * 0.55f;
				ImVec2 const lower(left, top + (height - box) * 0.5f);
				drawList->AddRect(lower, ImVec2(lower.x + box, lower.y + box), normal, 1.0f, std::max(font * 0.08f, 1.0f));
				left += box + style.ItemInnerSpacing.x;
			}
			std::string_view const text = cell.Text;
			float const room = std::max(right - left, 0.0f);
			ImVec2 const size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
			cut = size.x > room;
			drawList->PushClipRect(ImVec2(left, top), ImVec2(right, top + height), true);
			if (cell.Escapes.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, normal);
				ImGui::RenderTextEllipsis(drawList, ImVec2(left, textY), ImVec2(right, top + height), right, text.data(),
				                          text.data() + text.size(), &size);
				ImGui::PopStyleColor();
			}
			else
			{
				// The string's \n escapes dimmed: its parts drawn in turn, cut before the ellipsis as RenderTextEllipsis cuts.
				std::string_view shown = text;
				if (cut)
				{
					float const ellipsisWidth = ImGui::CalcTextSize(ellipsis.data(), ellipsis.data() + ellipsis.size()).x;
					char const* end = nullptr;
					ImGui::GetFont()->CalcTextSizeA(font, std::max(room - ellipsisWidth, 1.0f), 0.0f, text.data(),
					                                text.data() + text.size(), &end);
					shown = text.substr(0, static_cast<size_t>(end - text.data()));
				}
				float at = left;
				size_t start = 0;
				auto const part = [&](size_t from, size_t to, ImU32 color)
				{
					if (to <= from || from >= shown.size())
					{
						return;
					}
					std::string_view const piece = shown.substr(from, std::min(to, shown.size()) - from);
					drawList->AddText(ImVec2(at, textY), color, piece.data(), piece.data() + piece.size());
					at += ImGui::CalcTextSize(piece.data(), piece.data() + piece.size()).x;
				};
				for (uint32_t const offset : cell.Escapes)
				{
					if (offset < start || offset + 2 > text.size())
					{
						continue;
					}
					part(start, offset, normal);
					part(offset, offset + 2, dimmed);
					start = offset + 2;
				}
				part(start, shown.size(), normal);
				if (cut)
				{
					drawList->AddText(ImVec2(at, textY), normal, ellipsis.data(), ellipsis.data() + ellipsis.size());
				}
			}
			drawList->PopClipRect();
		}
		if (hovered)
		{
			// The whole value when it was cut (an entity's with its ID), else where the value comes from.
			std::string const entity = context.DescribeEntity ? context.DescribeEntity(m_LiveEntity) : m_LiveEntity.ToString();
			std::string tooltip = (cut || cell.Kind == VisualScriptValueCellKind::Entity) && !cell.Tooltip.empty()
			                          ? cell.Tooltip
			                          : fmt::format("{}{}", VisualScriptLiveTexts::LiveValueOn, entity);
			if (!cell.Note.empty())
			{
				tooltip += "\n" + cell.Note;
			}
			if (cell.Kind != VisualScriptValueCellKind::Swatch)
			{
				ImGui::SetTooltip("%s", tooltip.c_str());
				m_LiveTooltip = std::move(tooltip);
			}
		}
		if (clicked && cell.Kind == VisualScriptValueCellKind::Entity && cell.Entity.IsValid())
		{
			// A click selects the entity in the running scene.
			frame.Result.SelectEntity = cell.Entity;
		}
		ImGui::PopID();
		ImGui::SetCursorScreenPos(cursor);
	}

	std::map<std::string, VisualScriptValueCell> VisualScriptSidebar::GetLiveValues() const
	{
		std::map<std::string, VisualScriptValueCell> cells;
		for (auto const& [variable, cell] : m_LiveCells)
		{
			cells.emplace(cell.Value.Name, cell.Cell);
		}
		return cells;
	}

	VisualScriptValueCell const* VisualScriptSidebar::FindLiveCell(uint32_t variable) const
	{
		auto const found = m_LiveCells.find(variable);
		return found != m_LiveCells.end() ? &found->second.Cell : nullptr;
	}

	bool VisualScriptSidebar::IsLiveCellFlashing(uint32_t variable, double time) const
	{
		auto const found = m_LiveCells.find(variable);
		return found != m_LiveCells.end() && time - found->second.Changed >= 0.0 && time - found->second.Changed < LiveFlashSeconds;
	}

	std::string_view VisualScriptSidebar::GetLiveCellLabel(uint32_t variable) const
	{
		auto const found = std::find_if(m_LiveCellLabels.begin(), m_LiveCellLabels.end(),
		                                [variable](std::pair<uint32_t, std::string_view> const& label)
		                                {
											return label.first == variable;
										});
		return found != m_LiveCellLabels.end() ? found->second : std::string_view();
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptSidebar::FindLiveCellRect(uint32_t variable) const
	{
		auto const found = std::find_if(m_LiveCellRects.begin(), m_LiveCellRects.end(),
		                                [variable](auto const& rect)
		                                {
											return rect.first == variable;
										});
		return found != m_LiveCellRects.end() ? std::optional(found->second) : std::nullopt;
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptSidebar::FindTypeRect(uint32_t variable) const
	{
		auto const found = std::find_if(m_TypeRects.begin(), m_TypeRects.end(),
		                                [variable](auto const& rect)
		                                {
											return rect.first == variable;
										});
		return found != m_TypeRects.end() ? std::optional(found->second) : std::nullopt;
	}
}
