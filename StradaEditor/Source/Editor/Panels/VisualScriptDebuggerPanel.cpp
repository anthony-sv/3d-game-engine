#include "Editor/Panels/VisualScriptDebuggerPanel.h"

#include "Editor/EditorContext.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptDebugValues.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <span>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
		                                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

		// A document's display name: its file stem, a memory asset's name, or its handle.
		std::string GetDocumentName(AssetHandle document, bool withExtension)
		{
			std::optional<AssetMetadata> const metadata =
				AssetManager::IsInitialized() ? AssetManager::GetMetadata(document) : std::nullopt;
			if (!metadata)
			{
				return document.ToString();
			}
			if (metadata->Path.empty())
			{
				return metadata->Name;
			}
			std::filesystem::path const path = FileSystem::PathFromUtf8(metadata->Path);
			return FileSystem::PathToUtf8(withExtension ? path.filename() : path.stem());
		}

		std::string DescribeEntity(Scene& scene, UUID entity)
		{
			if (!scene.HasEntity(entity))
			{
				return fmt::format("Missing ({})", entity);
			}
			return fmt::format("{} ({})", scene.GetEntityByUUID(entity).GetName(), entity);
		}

		// What the panel formats values with: names of the running scene's entities and of the project's assets.
		VisualScriptValueCell FormatValue(VisualScriptDebugValue const& value, Scene& scene)
		{
			auto const describe = [&scene](UUID entity)
			{
				return scene.HasEntity(entity) ? scene.GetEntityByUUID(entity).GetName() : fmt::format("Missing ({})", entity);
			};
			VisualScriptAssetNameLookup const assetName = [](AssetHandle handle)
			{
				return GetDocumentName(handle, true);
			};
			return FormatVisualScriptDebugValue(value, nullptr, {}, describe, assetName);
		}

		// A value cell: its swatch or text, dimmed when computed earlier or unknown, its note, and the full value as its tooltip.
		void DrawValueCell(VisualScriptValueCell const& cell, EditorContext& context, std::string const& id)
		{
			if (cell.Kind == VisualScriptValueCellKind::Swatch && cell.Color)
			{
				ImVec2 const at = ImGui::GetCursorScreenPos();
				float const size = ImGui::GetTextLineHeight();
				ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + size * 2.0f, at.y + size), *cell.Color, 2.0f);
				ImGui::Dummy(ImVec2(size * 2.0f, size));
			}
			else if (cell.Dimmed || cell.Kind == VisualScriptValueCellKind::None)
			{
				ImGui::TextDisabled("%s", cell.Text.c_str());
			}
			else
			{
				ImGui::TextUnformatted(cell.Text.c_str());
			}
			if (!cell.Tooltip.empty())
			{
				ImGui::SetItemTooltip("%s", cell.Tooltip.c_str());
			}
			if (!cell.Note.empty())
			{
				ImGui::SameLine();
				ImVec2 const at = ImGui::GetCursorScreenPos();
				UI::DrawArrowText(ImGui::GetWindowDrawList(), at, ImGui::GetColorU32(ImGuiCol_TextDisabled), cell.Note);
				ImGui::Dummy(UI::CalcArrowTextSize(cell.Note));
			}
			if (cell.Kind == VisualScriptValueCellKind::Entity && cell.Entity.IsValid() && context.GetScene().HasEntity(cell.Entity))
			{
				ImGui::SameLine();
				if (ImGui::SmallButton(fmt::format("{}##{}", VisualScriptDebuggerPanel::Texts::Select, id).c_str()))
				{
					std::array<UUID, 1> const entities = {cell.Entity};
					context.Select(entities);
				}
			}
		}

		std::string_view GetStartWords(VisualScriptStartKind kind)
		{
			switch (kind)
			{
				case VisualScriptStartKind::Event:
					return "Event ";
				case VisualScriptStartKind::Tick:
					return "the tick of ";
				case VisualScriptStartKind::Timeline:
					return "the timeline of ";
				case VisualScriptStartKind::Resume:
					return "";
				case VisualScriptStartKind::SendEvent:
					return "a message to ";
				case VisualScriptStartKind::Delivery:
					return "the delivery of ";
			}
			return "";
		}
	}

	std::string_view GetVisualScriptDebuggerTabName(VisualScriptDebuggerTab tab)
	{
		switch (tab)
		{
			case VisualScriptDebuggerTab::CallStack:
				return "CallStack";
			case VisualScriptDebuggerTab::Values:
				return "Values";
			case VisualScriptDebuggerTab::Breakpoints:
				return "Breakpoints";
			case VisualScriptDebuggerTab::Instances:
				return "Instances";
		}
		return "CallStack";
	}

	std::optional<VisualScriptDebuggerTab> ParseVisualScriptDebuggerTab(std::string_view name)
	{
		for (VisualScriptDebuggerTab const tab : {VisualScriptDebuggerTab::CallStack, VisualScriptDebuggerTab::Values,
		                                          VisualScriptDebuggerTab::Breakpoints, VisualScriptDebuggerTab::Instances})
		{
			if (GetVisualScriptDebuggerTabName(tab) == name)
			{
				return tab;
			}
		}
		return std::nullopt;
	}

	VisualScriptDebuggerPanel::VisualScriptDebuggerPanel(VisualScriptDebugSession& session, EditorContext& context)
		: m_Session(session),
		  m_Context(context)
	{
	}

	void VisualScriptDebuggerPanel::Show(std::optional<VisualScriptDebuggerTab> tab)
	{
		m_Open = true;
		m_FocusRequested = true;
		if (tab)
		{
			SetTab(*tab);
		}
	}

	void VisualScriptDebuggerPanel::SetTab(VisualScriptDebuggerTab tab)
	{
		m_Tab = tab;
		m_TabRequest = tab;
	}

	void VisualScriptDebuggerPanel::SelectFrame(uint32_t frame)
	{
		m_Session.SelectFrame(frame);
		if (VisualScriptStop const* stop = GetSelectedStop(); stop != nullptr && m_FollowFrame)
		{
			// Copied: opening a window refreshes nothing of the session, but the stop belongs to its state.
			VisualScriptStop const shown = *stop;
			m_FollowFrame(shown, frame);
		}
	}

	void VisualScriptDebuggerPanel::OnStop(VisualScriptStop const&)
	{
		if (!m_StoppedThisSession)
		{
			m_StoppedThisSession = true;
			Show(VisualScriptDebuggerTab::CallStack);
		}
	}

	VisualScriptStop const* VisualScriptDebuggerPanel::GetSelectedStop() const
	{
		std::optional<uint64_t> const selected = m_Session.GetSelectedStop();
		return selected ? m_Session.FindStop(*selected) : nullptr;
	}

	std::vector<std::string> VisualScriptDebuggerPanel::DescribeCallStack(VisualScriptStop const& stop, EditorContext& context)
	{
		Scene& scene = context.GetScene();
		std::vector<std::string> rows;
		UUID previousEntity = UUID::Invalid();
		for (size_t index = 0; index < stop.Frames.size(); index++)
		{
			VisualScriptStopFrame const& frame = stop.Frames[index];
			if (frame.Kind == VisualScriptStopFrameKind::Native)
			{
				rows.push_back(fmt::format("#{} {}{}{}", index, Texts::EngineCode, frame.Title, Texts::EngineCodeEnd));
				previousEntity = UUID::Invalid();
				continue;
			}
			std::string const function =
				frame.Function == 0 && frame.FunctionName.empty() ? std::string(Texts::EventGraph) : frame.FunctionName;
			std::string row =
				fmt::format("#{} {} (node {}) \xC2\xB7 {} \xE2\x80\xBA {}", index, frame.Title, frame.Node,
			                GetDocumentName(frame.Document, true), function.empty() ? std::string(Texts::EventGraph) : function);
			// The instance is named where it changes: the innermost frame, and a frame of another entity (a sender).
			if (index == 0 || frame.Entity != previousEntity)
			{
				row += " \xC2\xB7 on " + DescribeEntity(scene, frame.Entity);
			}
			previousEntity = frame.Entity;
			rows.push_back(std::move(row));
		}
		VisualScriptStopStart const& start = stop.Start;
		if (start.Kind == VisualScriptStartKind::Resume)
		{
			// A resumed fiber: where it waited, and where the suspended execution started (S4-R6.1).
			std::string row = fmt::format("{} (node {})", start.Title, start.Node);
			if (start.Origin)
			{
				row += fmt::format("{}{} (node {})", Texts::Resumed,
				                   start.Origin->Title.empty() ? std::string("a node removed since") : start.Origin->Title,
				                   start.Origin->Node);
			}
			rows.push_back(std::move(row));
		}
		else
		{
			rows.push_back(fmt::format("{}{}{} (node {})", Texts::StartedBy, GetStartWords(start.Kind), start.Title, start.Node));
		}
		return rows;
	}

	void VisualScriptDebuggerPanel::Evaluate()
	{
		m_EvaluateResult.clear();
		VisualScriptStop const* stop = GetSelectedStop();
		if (stop == nullptr || m_EvaluateText.empty() || !ScriptEngine::IsInitialized())
		{
			return;
		}
		std::array<std::string, 1> const formulas = {m_EvaluateText};
		Result<VisualScriptStopValues> const described =
			VisualScriptDebugger::DescribeStop(stop->ID, m_Session.GetSelectedFrame(), formulas);
		if (!described)
		{
			m_EvaluateResult = described.GetError();
			return;
		}
		if (described.GetValue().Evaluated.empty())
		{
			return;
		}
		VisualScriptEvaluated const& evaluated = described.GetValue().Evaluated.front();
		m_EvaluateResult = evaluated.Value ? FormatValue(*evaluated.Value, m_Context.GetScene()).Text : evaluated.Error;
	}

	void VisualScriptDebuggerPanel::OnImGuiRender(std::optional<ImGuiID> dockTarget)
	{
		if (!m_Session.IsAttached())
		{
			m_StoppedThisSession = false;
		}
		if (!m_Open)
		{
			m_Visible = false;
			return;
		}
		if (dockTarget && ImGui::FindWindowByName(WindowName) == nullptr)
		{
			ImGui::SetNextWindowDockID(*dockTarget, ImGuiCond_FirstUseEver);
		}
		if (m_FocusRequested && ImGui::FindWindowByName(WindowName) != nullptr)
		{
			ImGui::SetNextWindowFocus();
			m_FocusRequested = false;
		}
		m_Visible = ImGui::Begin(WindowName, &m_Open);
		if (!m_Visible)
		{
			ImGui::End();
			return;
		}
		if (ImGui::BeginTabBar("##debuggerTabs"))
		{
			std::array<std::pair<VisualScriptDebuggerTab, std::string_view>, 4> const tabs = {{
				{VisualScriptDebuggerTab::CallStack, Texts::CallStack},
				{VisualScriptDebuggerTab::Values, Texts::Values},
				{VisualScriptDebuggerTab::Breakpoints, Texts::Breakpoints},
				{VisualScriptDebuggerTab::Instances, Texts::Instances},
			}};
			for (auto const& [tab, label] : tabs)
			{
				ImGuiTabItemFlags const flags = m_TabRequest == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
				if (!ImGui::BeginTabItem(label.data(), nullptr, flags))
				{
					continue;
				}
				if (!m_TabRequest)
				{
					m_Tab = tab;
				}
				switch (tab)
				{
					case VisualScriptDebuggerTab::CallStack:
						DrawCallStack();
						break;
					case VisualScriptDebuggerTab::Values:
						DrawValues();
						break;
					case VisualScriptDebuggerTab::Breakpoints:
						DrawBreakpoints();
						break;
					case VisualScriptDebuggerTab::Instances:
						DrawInstances();
						break;
				}
				ImGui::EndTabItem();
			}
			m_TabRequest.reset();
			ImGui::EndTabBar();
		}
		ImGui::End();
	}

	void VisualScriptDebuggerPanel::DrawCallStack()
	{
		m_CallStackRows.clear();
		if (!m_Session.IsAttached())
		{
			ImGui::TextDisabled("%s", Texts::NotPlaying.data());
			return;
		}
		VisualScriptDebugState const& state = m_Session.GetState();
		if (state.Stops.empty())
		{
			ImGui::TextDisabled("%s", Texts::NothingStopped.data());
			return;
		}
		VisualScriptStop const* selected = GetSelectedStop();
		Scene& scene = m_Context.GetScene();
		// The stop picker, when several stops exist: each with its Continue (that stop only).
		if (state.Stops.size() > 1)
		{
			std::string const label = state.Passed > 0 ? fmt::format("{}{} \xC2\xB7 {}{}", Texts::StoppedExecutions, state.Stops.size(),
			                                                         state.Passed, Texts::Passed)
			                                           : fmt::format("{}{}", Texts::StoppedExecutions, state.Stops.size());
			if (ImGui::BeginCombo("##stops", label.c_str()))
			{
				for (VisualScriptStop const& stop : state.Stops)
				{
					ImGui::PushID(static_cast<int>(stop.ID));
					std::string const title = stop.Frames.empty() ? std::string() : stop.Frames.front().Title;
					if (ImGui::Selectable(fmt::format("Stop {}: {} on {}", stop.ID, title, DescribeEntity(scene, stop.Entity)).c_str(),
					                      selected != nullptr && selected->ID == stop.ID, ImGuiSelectableFlags_AllowOverlap))
					{
						m_Session.SelectStop(stop.ID);
					}
					if (stop.Resumable)
					{
						ImGui::SameLine();
						if (ImGui::SmallButton(Texts::Continue.data()))
						{
							m_Session.RequestResume(stop.ID, VisualScriptDebugAction::Continue);
						}
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
		}
		if (selected == nullptr)
		{
			return;
		}
		// Copied: a click selects a frame, which reads the session again.
		VisualScriptStop const stop = *selected;
		if (!stop.Message.empty())
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(stop.Message.c_str());
			ImGui::PopTextWrapPos();
		}
		std::vector<std::string> const rows = DescribeCallStack(stop, m_Context);
		uint32_t const selectedFrame = m_Session.GetSelectedFrame();
		std::optional<uint32_t> clicked;
		float const marker = ImGui::GetTextLineHeight();
		for (size_t index = 0; index < rows.size(); index++)
		{
			bool const isFrame = index < stop.Frames.size();
			bool const isSelected = isFrame && index == selectedFrame;
			m_CallStackRows.push_back({static_cast<uint32_t>(index), rows[index], isSelected});
			ImGui::PushID(static_cast<int>(index));
			// The selected frame is marked with a drawn triangle (the arrows' shape).
			ImVec2 const at = ImGui::GetCursorScreenPos();
			if (isSelected)
			{
				float const half = marker * 0.3f;
				ImVec2 const center(at.x + marker * 0.5f, at.y + marker * 0.5f);
				ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(center.x - half * 0.7f, center.y - half),
				                                              ImVec2(center.x - half * 0.7f, center.y + half),
				                                              ImVec2(center.x + half, center.y), VisualScriptCurrentInstructionColor);
			}
			ImGui::SetCursorScreenPos(ImVec2(at.x + marker * 1.2f, at.y));
			bool const native = isFrame && stop.Frames[index].Kind == VisualScriptStopFrameKind::Native;
			if (!isFrame || native)
			{
				ImGui::TextDisabled("%s", rows[index].c_str());
			}
			else if (ImGui::Selectable(rows[index].c_str(), isSelected))
			{
				clicked = static_cast<uint32_t>(index);
			}
			ImGui::PopID();
		}
		if (clicked)
		{
			SelectFrame(*clicked);
		}
	}

	void VisualScriptDebuggerPanel::DrawValues()
	{
		VisualScriptStop const* stop = GetSelectedStop();
		if (stop == nullptr)
		{
			ImGui::TextDisabled("%s", m_Session.IsAttached() ? Texts::NothingStopped.data() : Texts::NotPlaying.data());
			return;
		}
		uint64_t const stopId = stop->ID;
		VisualScriptStopValues const* values = m_Session.GetStopValues(stopId, m_Session.GetSelectedFrame());
		if (values == nullptr)
		{
			ImGui::TextDisabled("%s", Texts::NoValues.data());
			return;
		}
		Scene& scene = m_Context.GetScene();
		ImFont* const monospace = m_MonospaceFont;
		float const evaluateHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
		if (ImGui::BeginTable("##values", 3, TableFlags, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y - evaluateHeight, 0.0f))))
		{
			ImGui::TableSetupColumn(Texts::Name.data(), ImGuiTableColumnFlags_WidthStretch, 0.3f);
			ImGui::TableSetupColumn(Texts::Type.data(), ImGuiTableColumnFlags_WidthStretch, 0.2f);
			ImGui::TableSetupColumn(Texts::Value.data(), ImGuiTableColumnFlags_WidthStretch, 0.5f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			uint32_t rowId = 0;
			auto const section = [](std::string_view title)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", std::string(title).c_str());
			};
			auto const row = [&](VisualScriptDebugValue const& value)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(value.Name.c_str());
				ImGui::TableNextColumn();
				if (monospace != nullptr)
				{
					ImGui::PushFont(monospace, 0.0f);
				}
				ImGui::TextUnformatted(value.Type.c_str());
				if (monospace != nullptr)
				{
					ImGui::PopFont();
				}
				ImGui::TableNextColumn();
				DrawValueCell(FormatValue(value, scene), m_Context, std::to_string(rowId++));
			};
			auto const rows = [&](std::string_view title, std::span<VisualScriptDebugValue const> list)
			{
				if (list.empty())
				{
					return;
				}
				section(title);
				for (VisualScriptDebugValue const& value : list)
				{
					row(value);
				}
			};
			rows(Texts::Inputs, values->Node.Inputs);
			rows(Texts::Outputs, values->Node.Outputs);
			rows(Texts::Event, values->Event);
			rows(Texts::Parameters, values->Parameters);
			rows(Texts::Locals, values->Locals);
			rows(Texts::Variables, values->Variables);
			if (!values->Loops.empty())
			{
				section(Texts::Loops);
				for (VisualScriptLoopState const& loop : values->Loops)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("%s (node %u)", loop.Kind.c_str(), loop.Node);
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					if (loop.Kind == "Sequence")
					{
						ImGui::Text("next: %s", loop.Next.c_str());
					}
					else
					{
						ImGui::Text("Index %lld", static_cast<long long>(loop.Index));
					}
				}
			}
			for (VisualScriptNodeValues const& computed : values->Computed)
			{
				section(fmt::format("{}: node {}", Texts::Computed, computed.Node));
				for (VisualScriptDebugValue const& output : computed.Outputs)
				{
					row(output);
				}
			}
			ImGui::EndTable();
		}
		// Evaluate: a formula read against the frame's snapshot, once (nothing is watched).
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		bool const entered =
			ImGui::InputTextWithHint("##evaluate", Texts::EvaluateHint.data(), &m_EvaluateText, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button(Texts::Evaluate.data()) || entered)
		{
			Evaluate();
		}
		if (!m_EvaluateResult.empty())
		{
			ImGui::SameLine();
			ImGui::TextUnformatted(m_EvaluateResult.c_str());
		}
	}

	void VisualScriptDebuggerPanel::DrawBreakpoints()
	{
		VisualScriptBreakpointStore const& store = m_Session.GetBreakpoints();
		std::vector<AssetHandle> const graphs = store.GetGraphs();
		if (ImGui::Button(Texts::DisableAll.data()))
		{
			for (AssetHandle const graph : graphs)
			{
				std::vector<uint32_t> nodes;
				for (VisualScriptBreakpoint const& breakpoint : store.Get(graph))
				{
					nodes.push_back(breakpoint.Node);
				}
				m_Session.SetBreakpointsEnabled(graph, nodes, false);
			}
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(graphs.empty());
		if (ImGui::Button(Texts::RemoveAll.data()))
		{
			m_OpenRemoveAll = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		bool breakOnErrors = m_Session.IsBreakOnErrors();
		if (ImGui::Checkbox(Texts::BreakOnErrors.data(), &breakOnErrors))
		{
			m_Session.SetBreakOnErrors(breakOnErrors);
		}
		ImGui::SetItemTooltip("An aborting error stops the game where it happened");

		constexpr char const* RemoveAllId = "Remove All Breakpoints##removeAll";
		if (std::exchange(m_OpenRemoveAll, false))
		{
			ImGui::OpenPopup(RemoveAllId);
		}
		m_RemoveAllOpen =
			ImGui::BeginPopupModal(RemoveAllId, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
		if (m_RemoveAllOpen)
		{
			ImGui::TextUnformatted(Texts::RemoveAllQuestion.data());
			if (ImGui::Button(Texts::Remove.data()))
			{
				m_Session.ClearBreakpoints(std::nullopt);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(Texts::Cancel.data()) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (graphs.empty())
		{
			ImGui::TextDisabled("%s", Texts::NoBreakpoints.data());
			return;
		}
		VisualScriptDebugState const& state = m_Session.GetState();
		if (!ImGui::BeginTable("##breakpoints", 8, TableFlags))
		{
			return;
		}
		ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn("Breakpoint", ImGuiTableColumnFlags_WidthStretch, 0.35f);
		ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthStretch, 0.25f);
		ImGui::TableSetupColumn("Hit", ImGuiTableColumnFlags_WidthStretch, 0.08f);
		ImGui::TableSetupColumn(Texts::Reached.data(), ImGuiTableColumnFlags_WidthStretch, 0.07f);
		ImGui::TableSetupColumn(Texts::Hits.data(), ImGuiTableColumnFlags_WidthStretch, 0.07f);
		ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch, 0.18f);
		ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		// Changes are made after the table: they change the store the rows are drawn from.
		std::optional<std::pair<AssetHandle, VisualScriptBreakpoint>> changed;
		std::optional<std::pair<AssetHandle, uint32_t>> removed;
		for (AssetHandle const graph : graphs)
		{
			Ref<VisualScriptAsset> const asset =
				AssetManager::IsInitialized() ? AssetManager::GetAsset<VisualScriptAsset>(graph) : Ref<VisualScriptAsset>();
			std::string const graphName = GetDocumentName(graph, true);
			for (VisualScriptBreakpoint const& breakpoint : store.Get(graph))
			{
				ImGui::PushID(fmt::format("{}:{}", graph.ToString(), breakpoint.Node).c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				bool enabled = breakpoint.Enabled;
				if (ImGui::Checkbox("##enabled", &enabled))
				{
					VisualScriptBreakpoint toggled = breakpoint;
					toggled.Enabled = enabled;
					changed = std::make_pair(graph, std::move(toggled));
				}
				ImGui::TableNextColumn();
				// "Player.sgraph › TakeHit › Health.Set (node 12)": the node's sub-graph from the stored document.
				std::string function(Texts::EventGraph);
				if (asset != nullptr)
				{
					for (VisualScriptFunction const& candidate : asset->GetDocument().Functions)
					{
						auto const found = std::find_if(candidate.Graph.Nodes.begin(), candidate.Graph.Nodes.end(),
						                                [&breakpoint](VisualScriptNode const& node)
						                                {
															return node.ID == breakpoint.Node;
														});
						if (found != candidate.Graph.Nodes.end())
						{
							function = candidate.Name;
							break;
						}
					}
				}
				std::string const name =
					fmt::format("{} \xE2\x80\xBA {} \xE2\x80\xBA {} (node {})", graphName, function, breakpoint.Type, breakpoint.Node);
				ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m_OpenNode)
				{
					m_OpenNode(graph, breakpoint.Node);
				}
				ImGui::TableNextColumn();
				std::string condition = breakpoint.Condition;
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputText("##condition", &condition);
				if (ImGui::IsItemDeactivatedAfterEdit())
				{
					VisualScriptBreakpoint edited = breakpoint;
					edited.Condition = condition;
					changed = std::make_pair(graph, std::move(edited));
				}
				ImGui::TableNextColumn();
				int hitCount = static_cast<int>(std::min<uint32_t>(breakpoint.HitCount, static_cast<uint32_t>(INT32_MAX)));
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputInt("##hitCount", &hitCount, 0, 0);
				if (ImGui::IsItemDeactivatedAfterEdit())
				{
					VisualScriptBreakpoint edited = breakpoint;
					edited.HitCount =
						static_cast<uint32_t>(std::clamp(hitCount, 0, static_cast<int>(VisualScriptBreakpointStore::MaxHitCount)));
					changed = std::make_pair(graph, std::move(edited));
				}
				auto const counted = std::find_if(state.Breakpoints.begin(), state.Breakpoints.end(),
				                                  [graph, &breakpoint](VisualScriptBreakpointState const& candidate)
				                                  {
													  return candidate.Document == graph && candidate.Node == breakpoint.Node;
												  });
				bool const reported = counted != state.Breakpoints.end();
				ImGui::TableNextColumn();
				ImGui::Text("%u", reported ? counted->Reached : 0u);
				ImGui::TableNextColumn();
				ImGui::Text("%u", reported ? counted->Hits : 0u);
				ImGui::TableNextColumn();
				if (reported && !counted->Error.empty())
				{
					ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(VisualScriptErrorColor), "%s", counted->Error.c_str());
				}
				else if (m_Session.IsAttached() && (!reported || !counted->Active))
				{
					ImGui::TextDisabled("%s", reported && counted->Reason == VisualScriptBreakpointInactivity::NotInProgram
					                              ? "not in the running program"
					                              : "inactive");
				}
				ImGui::TableNextColumn();
				// A drawn cross: the editor's fonts have no multiplication sign at button size worth relying on.
				ImVec2 const at = ImGui::GetCursorScreenPos();
				float const size = ImGui::GetFrameHeight();
				if (ImGui::InvisibleButton("##remove", ImVec2(size, size)))
				{
					removed = std::make_pair(graph, breakpoint.Node);
				}
				ImGui::SetItemTooltip("%s", Texts::Remove.data());
				ImU32 const cross = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
				float const inset = size * 0.3f;
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddLine(ImVec2(at.x + inset, at.y + inset), ImVec2(at.x + size - inset, at.y + size - inset), cross, 1.5f);
				drawList->AddLine(ImVec2(at.x + size - inset, at.y + inset), ImVec2(at.x + inset, at.y + size - inset), cross, 1.5f);
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
		if (changed)
		{
			if (Result<void> const set = m_Session.SetBreakpoint(changed->first, changed->second); !set)
			{
				UI::ReportFailure(set, "Changing the breakpoint");
			}
		}
		if (removed)
		{
			std::array<uint32_t, 1> const nodes = {removed->second};
			m_Session.RemoveBreakpoints(removed->first, nodes);
		}
	}

	void VisualScriptDebuggerPanel::DrawInstances()
	{
		if (!m_Session.IsAttached() || !ScriptEngine::IsInitialized())
		{
			ImGui::TextDisabled("%s", Texts::NotPlaying.data());
			m_Instances.reset();
			return;
		}
		double const now = ImGui::GetTime();
		if (now - m_InstancesRead >= InstancesInterval)
		{
			m_InstancesRead = now;
			VisualScriptInstanceQuery query;
			query.Offset = m_InstanceOffset;
			query.Limit = InstancesPage;
			Result<VisualScriptInstanceList> listed = VisualScriptDebugger::DescribeInstances(query);
			m_Instances = listed ? std::optional<VisualScriptInstanceList>(listed.TakeValue()) : std::nullopt;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
		ImGui::InputTextWithHint("##filter", Texts::Filter.data(), &m_InstanceFilter);
		uint32_t const total = m_Instances ? m_Instances->Total : 0;
		ImGui::SameLine();
		ImGui::BeginDisabled(m_InstanceOffset == 0);
		if (ImGui::Button(Texts::Previous.data()))
		{
			m_InstanceOffset = m_InstanceOffset > InstancesPage ? m_InstanceOffset - InstancesPage : 0;
			m_InstancesRead = -1.0e30;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(m_InstanceOffset + InstancesPage >= total);
		if (ImGui::Button(Texts::Next.data()))
		{
			m_InstanceOffset += InstancesPage;
			m_InstancesRead = -1.0e30;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("%u-%u of %u", total == 0 ? 0u : m_InstanceOffset + 1, std::min(m_InstanceOffset + InstancesPage, total),
		                    total);
		if (!m_Instances || !ImGui::BeginTable("##instances", 5, TableFlags))
		{
			return;
		}
		ImGui::TableSetupColumn("Entity", ImGuiTableColumnFlags_WidthStretch, 0.22f);
		ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthStretch, 0.2f);
		ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.1f);
		ImGui::TableSetupColumn("Waiting in", ImGuiTableColumnFlags_WidthStretch, 0.38f);
		ImGui::TableSetupColumn("Stops", ImGuiTableColumnFlags_WidthStretch, 0.1f);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		Scene& scene = m_Context.GetScene();
		std::optional<std::pair<AssetHandle, UUID>> picked;
		for (VisualScriptInstanceInfo const& instance : m_Instances->Instances)
		{
			std::string const entity = DescribeEntity(scene, instance.Entity);
			std::string const graph = GetDocumentName(instance.Graph, true);
			if (!m_InstanceFilter.empty() && entity.find(m_InstanceFilter) == std::string::npos &&
			    graph.find(m_InstanceFilter) == std::string::npos)
			{
				continue;
			}
			ImGui::PushID(fmt::format("{}", instance.Entity.GetValue()).c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(entity.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
			{
				picked = std::make_pair(instance.Graph, instance.Entity);
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(graph.c_str());
			ImGui::TableNextColumn();
			std::string_view const stateText = instance.State == VisualScriptInstanceState::Stopped   ? Texts::Stopped
			                                   : instance.State == VisualScriptInstanceState::Waiting ? Texts::Waiting
			                                                                                          : Texts::Idle;
			ImGui::TextUnformatted(stateText.data(), stateText.data() + stateText.size());
			ImGui::TableNextColumn();
			std::string waits;
			for (VisualScriptInstanceWait const& wait : instance.Waits)
			{
				std::string part = fmt::format("{} (node {})", wait.Title, wait.Node);
				if (wait.Remaining)
				{
					part += fmt::format(": {:.2f}{}", *wait.Remaining, Texts::SecondsLeft);
				}
				else if (wait.Frames)
				{
					part += fmt::format(": {}{}", *wait.Frames, Texts::FramesLeft);
				}
				waits += waits.empty() ? part : ", " + part;
			}
			ImGui::TextUnformatted(waits.c_str());
			ImGui::TableNextColumn();
			std::string stops;
			for (uint64_t const stop : instance.Stops)
			{
				stops += stops.empty() ? std::to_string(stop) : ", " + std::to_string(stop);
			}
			ImGui::TextUnformatted(stops.c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
		// A click makes the instance its graph's target and opens the graph's window on it.
		if (picked)
		{
			m_Session.SetTarget(picked->first, picked->second);
			if (m_OpenGraph)
			{
				m_OpenGraph(picked->first, picked->second);
			}
		}
	}
}
