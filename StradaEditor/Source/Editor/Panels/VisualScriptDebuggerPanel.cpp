#include "Editor/Panels/VisualScriptDebuggerPanel.h"

#include "Editor/EditorContext.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"
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
#include <cmath>
#include <span>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
		                                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
		// The Evaluate row's list of names shows at most this many rows before it scrolls.
		constexpr float EvaluateNameRows = 6.0f;

		using Texts = VisualScriptDebuggerPanel::Texts;

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
				return fmt::format("{}{})", Texts::Missing, entity);
			}
			return fmt::format("{} ({})", scene.GetEntityByUUID(entity).GetName(), entity);
		}

		// A cell's text on one line, the \n escapes of its strings dimmed (the formatter's Escapes).
		void DrawCellText(VisualScriptValueCell const& cell, bool dimmed)
		{
			ImU32 const normal = ImGui::GetColorU32(dimmed ? ImGuiCol_TextDisabled : ImGuiCol_Text);
			ImU32 const escape = ImGui::GetColorU32(ImGuiCol_TextDisabled);
			std::string_view const text = cell.Text;
			if (cell.Escapes.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, normal);
				ImGui::TextUnformatted(text.data(), text.data() + text.size());
				ImGui::PopStyleColor();
				return;
			}
			// Parts of the text and its escapes, two bytes each, side by side.
			size_t start = 0;
			bool first = true;
			auto const part = [&first](std::string_view piece, ImU32 color)
			{
				if (piece.empty())
				{
					return;
				}
				if (!first)
				{
					ImGui::SameLine(0.0f, 0.0f);
				}
				first = false;
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextUnformatted(piece.data(), piece.data() + piece.size());
				ImGui::PopStyleColor();
			};
			for (uint32_t const offset : cell.Escapes)
			{
				if (offset < start || offset + 2 > text.size())
				{
					continue;
				}
				part(text.substr(start, offset - start), normal);
				part(text.substr(offset, 2), escape);
				start = offset + 2;
			}
			part(text.substr(start), normal);
		}

		// A value cell: its swatch or text, dimmed when computed earlier or unknown, its note, and the full value as its tooltip.
		void DrawValueCell(VisualScriptValueCell const& cell, EditorContext& context, std::string const& id)
		{
			if (cell.Kind == VisualScriptValueCellKind::Swatch && cell.Color)
			{
				ImVec2 const at = ImGui::GetCursorScreenPos();
				float const size = ImGui::GetTextLineHeight();
				ImU32 const color = cell.Dimmed ? (*cell.Color & 0x00FFFFFFu) | (0x80u << IM_COL32_A_SHIFT) : *cell.Color;
				ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + size * 2.0f, at.y + size), color, 2.0f);
				ImGui::Dummy(ImVec2(size * 2.0f, size));
			}
			else
			{
				ImGui::BeginGroup();
				DrawCellText(cell, cell.Dimmed || cell.Kind == VisualScriptValueCellKind::None);
				ImGui::EndGroup();
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
				if (ImGui::SmallButton(fmt::format("{}##{}", Texts::Select, id).c_str()))
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
					return Texts::StartEvent;
				case VisualScriptStartKind::Tick:
					return Texts::StartTick;
				case VisualScriptStartKind::Timeline:
					return Texts::StartTimeline;
				case VisualScriptStartKind::Resume:
					return {};
				case VisualScriptStartKind::SendEvent:
					return Texts::StartMessage;
				case VisualScriptStartKind::Delivery:
					return Texts::StartDelivery;
			}
			return {};
		}

		// A node's title as the canvas shows it, from the stored document holding it; "node 12" alone when it has none.
		std::string GetNodeTitle(AssetHandle document, uint32_t node)
		{
			Ref<VisualScriptAsset> const asset =
				AssetManager::IsInitialized() ? AssetManager::GetAsset<VisualScriptAsset>(document) : Ref<VisualScriptAsset>();
			if (asset == nullptr)
			{
				return {};
			}
			VisualScriptNode const* found = FindVisualScriptNode(asset->GetDocument(), node);
			return found != nullptr ? DeriveVisualScriptNodeTitle(*found, asset->GetDocument()) : std::string();
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

	std::optional<VisualScriptDebuggerPanel::ItemRect> VisualScriptDebuggerPanel::GetItemRect(VisualScriptDebuggerItem item) const
	{
		size_t const index = static_cast<size_t>(item);
		return index < m_ItemRects.size() ? m_ItemRects[index] : std::nullopt;
	}

	void VisualScriptDebuggerPanel::RecordItem(VisualScriptDebuggerItem item)
	{
		std::optional<ItemRect>& rect = m_ItemRects[static_cast<size_t>(item)];
		if (!rect)
		{
			rect = ItemRect{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
		}
	}

	VisualScriptStop const* VisualScriptDebuggerPanel::GetSelectedStop() const
	{
		std::optional<uint64_t> const selected = m_Session.GetSelectedStop();
		return selected ? m_Session.FindStop(*selected) : nullptr;
	}

	VisualScriptValueCell VisualScriptDebuggerPanel::FormatValue(VisualScriptDebugValue const& value) const
	{
		Scene& scene = m_Context.GetScene();
		auto const describe = [&scene](UUID entity)
		{
			return scene.HasEntity(entity) ? scene.GetEntityByUUID(entity).GetName() : fmt::format("{}{})", Texts::Missing, entity);
		};
		VisualScriptAssetNameLookup const assetName = [](AssetHandle handle)
		{
			return GetDocumentName(handle, true);
		};
		// The library the canvas's pills are formatted with, so a value reads the same on a wire and here.
		Ref<VisualScriptLibrary const> const library = m_Semantics != nullptr ? m_Semantics->GetLibrary() : nullptr;
		return FormatVisualScriptDebugValue(value, library.get(), MakeVisualScriptEnumLookup(library.get()), describe, assetName);
	}

	void VisualScriptDebuggerPanel::ForgetEvaluation()
	{
		m_EvaluatedIn.reset();
		m_EvaluateResult.clear();
		m_EvaluateCell.reset();
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
				fmt::format("#{} {}{}{}) \xC2\xB7 {} \xE2\x80\xBA {}", index, frame.Title, Texts::Node, frame.Node,
			                GetDocumentName(frame.Document, true), function.empty() ? std::string(Texts::EventGraph) : function);
			// The instance is named where it changes: the innermost frame, and a frame of another entity (a sender).
			if (index == 0 || frame.Entity != previousEntity)
			{
				row += fmt::format(" \xC2\xB7{}{}", Texts::On, DescribeEntity(scene, frame.Entity));
			}
			previousEntity = frame.Entity;
			rows.push_back(std::move(row));
		}
		VisualScriptStopStart const& start = stop.Start;
		if (start.Kind == VisualScriptStartKind::Resume)
		{
			// A resumed fiber: where it waited, and where the suspended execution started (S4-R6.1).
			std::string row = fmt::format("{}{}{})", start.Title, Texts::Node, start.Node);
			if (start.Origin)
			{
				row += fmt::format("{}{}{}{})", Texts::Resumed,
				                   start.Origin->Title.empty() ? std::string(Texts::NodeRemoved) : start.Origin->Title, Texts::Node,
				                   start.Origin->Node);
			}
			rows.push_back(std::move(row));
		}
		else
		{
			rows.push_back(fmt::format("{}{}{}{}{})", Texts::StartedBy, GetStartWords(start.Kind), start.Title, Texts::Node, start.Node));
		}
		return rows;
	}

	void VisualScriptDebuggerPanel::Evaluate()
	{
		m_EvaluateResult.clear();
		m_EvaluateCell.reset();
		VisualScriptStop const* stop = GetSelectedStop();
		if (stop == nullptr || m_EvaluateText.empty() || !ScriptEngine::IsInitialized())
		{
			return;
		}
		uint32_t const frame = m_Session.GetSelectedFrame();
		// The result belongs to this stop and frame: another selection clears it.
		m_EvaluatedIn = std::make_pair(stop->ID, frame);
		std::array<std::string, 1> const formulas = {m_EvaluateText};
		Result<VisualScriptStopValues> const described = VisualScriptDebugger::DescribeStop(stop->ID, frame, formulas);
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
		if (!evaluated.Value)
		{
			// Its error, with the suggestion the runtime makes ("unknown name angle: did you mean Angle?").
			m_EvaluateResult = evaluated.Error;
			return;
		}
		m_EvaluateCell = FormatValue(*evaluated.Value);
		m_EvaluateResult = m_EvaluateCell->Text;
	}

	void VisualScriptDebuggerPanel::OnImGuiRender(std::optional<ImGuiID> dockTarget)
	{
		if (!m_Session.IsAttached())
		{
			m_StoppedThisSession = false;
		}
		// The Evaluate row's result belongs to the stop and frame it was made in.
		if (m_EvaluatedIn)
		{
			VisualScriptStop const* stop = GetSelectedStop();
			if (stop == nullptr || m_EvaluatedIn != std::make_pair(stop->ID, m_Session.GetSelectedFrame()))
			{
				ForgetEvaluation();
			}
		}
		m_ItemRects.fill(std::nullopt);
		m_ValueRows.clear();
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
					if (ImGui::Selectable(
							fmt::format("{}{}: {}{}{}", Texts::StopPrefix, stop.ID, title, Texts::On, DescribeEntity(scene, stop.Entity))
								.c_str(),
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
		uint32_t const frameIndex = m_Session.GetSelectedFrame();
		// At an Enter position the stop node's plan has not run: its inputs are their sources' values (spec-slice4 §5.3).
		bool const atEnter = frameIndex == 0 && stop->Place == VisualScriptStopPlace::Enter && !stop->Frames.empty() &&
		                     stop->Frames.front().Kind != VisualScriptStopFrameKind::Native;
		VisualScriptStopValues const* values = m_Session.GetStopValues(stopId, frameIndex);
		if (values == nullptr)
		{
			ImGui::TextDisabled("%s", Texts::NoValues.data());
			return;
		}
		ImFont* const monospace = m_MonospaceFont;
		// Evaluate's row and the names it can read go below the values; in a short tab the names yield to the values, taking
		// at most a third of the height (and one row at least).
		float const nameLine = ImGui::GetTextLineHeightWithSpacing();
		float const available = ImGui::GetContentRegionAvail().y;
		float const nameRows = std::clamp(std::floor(available / 3.0f / nameLine), 1.0f, EvaluateNameRows);
		float const evaluateHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f + nameLine * nameRows;
		if (ImGui::BeginTable("##values", 3, TableFlags, ImVec2(0.0f, std::max(available - evaluateHeight, 0.0f))))
		{
			ImGui::TableSetupColumn(Texts::Name.data(), ImGuiTableColumnFlags_WidthStretch, 0.3f);
			ImGui::TableSetupColumn(Texts::Type.data(), ImGuiTableColumnFlags_WidthStretch, 0.2f);
			ImGui::TableSetupColumn(Texts::Value.data(), ImGuiTableColumnFlags_WidthStretch, 0.5f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			uint32_t rowId = 0;
			auto const section = [this](std::string title, std::string_view note)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", title.c_str());
				if (!note.empty())
				{
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", std::string(note).c_str());
				}
				m_ValueRows.push_back({std::move(title), {}, std::nullopt, std::string(note)});
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
				VisualScriptValueCell cell = FormatValue(value);
				DrawValueCell(cell, m_Context, std::to_string(rowId++));
				m_ValueRows.push_back({value.Name, value.Type, std::move(cell), {}});
			};
			auto const rows = [&](std::string_view title, std::span<VisualScriptDebugValue const> list, std::string_view note)
			{
				if (list.empty())
				{
					return;
				}
				section(std::string(title), note);
				for (VisualScriptDebugValue const& value : list)
				{
					row(value);
				}
			};
			rows(Texts::Inputs, values->Node.Inputs, atEnter ? Texts::EvaluatedWhenItRuns : std::string_view());
			// At a function's Entry the node's outputs are the function's parameters: they are listed once, as Parameters.
			bool const outputsAreParameters =
				!values->Parameters.empty() &&
				std::equal(values->Node.Outputs.begin(), values->Node.Outputs.end(), values->Parameters.begin(), values->Parameters.end(),
			               [](VisualScriptDebugValue const& output, VisualScriptDebugValue const& parameter)
			               {
							   return output.Name == parameter.Name && output.Type == parameter.Type && output.Value == parameter.Value;
						   });
			rows(Texts::Outputs, outputsAreParameters ? std::span<VisualScriptDebugValue const>() : values->Node.Outputs, {});
			rows(Texts::Event, values->Event, {});
			rows(Texts::Parameters, values->Parameters, {});
			rows(Texts::Locals, values->Locals, {});
			rows(Texts::Variables, values->Variables, {});
			if (!values->Loops.empty())
			{
				section(std::string(Texts::Loops), {});
				for (VisualScriptLoopState const& loop : values->Loops)
				{
					// "For Loop (node 5): Index 3 of 0 … 9", "For Each (node 8): Element 2 of 5 = (1, 0, 0)", "Sequence (node 4):
					// next: Then1".
					std::string const title = GetNodeTitle(values->Document, loop.Node);
					std::string const name = fmt::format("{}{}{})", title.empty() ? loop.Kind : title, Texts::Node, loop.Node);
					std::string position;
					std::optional<VisualScriptValueCell> element;
					if (loop.Kind == "ForLoop")
					{
						position = fmt::format("{}{}{}{}{}{}", Texts::LoopIndex, loop.Index, Texts::LoopOf, loop.First, Texts::LoopRange,
						                       loop.Last);
					}
					else if (loop.Kind == "ForEach")
					{
						position = fmt::format("{}{}{}{}", Texts::LoopElement, loop.Index, Texts::LoopOf, loop.Length);
						if (loop.Element)
						{
							element = FormatValue(*loop.Element);
						}
					}
					else if (!loop.Next.empty())
					{
						position = fmt::format("{}{}", Texts::LoopNext, loop.Next);
					}
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(name.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(position.c_str());
					ImGui::TableNextColumn();
					if (element)
					{
						DrawValueCell(*element, m_Context, std::to_string(rowId++));
					}
					m_ValueRows.push_back({name, {}, element, element ? position + std::string(Texts::LoopIs) + element->Text : position});
				}
			}
			for (VisualScriptNodeValues const& computed : values->Computed)
			{
				// Each node the execution computed, by its title.
				std::string const title = GetNodeTitle(values->Document, computed.Node);
				section(fmt::format("{}: {}{}{})", Texts::Computed, title, Texts::Node, computed.Node), {});
				for (VisualScriptDebugValue const& output : computed.Outputs)
				{
					row(output);
				}
			}
			ImGui::EndTable();
			// EndTable submits the table (its child window) as the last item.
			RecordItem(VisualScriptDebuggerItem::ValuesTable);
		}

		// Evaluate: a formula read against the frame's snapshot, once (nothing is watched), with the names it can read there (read
		// once per stop and frame: an empty condition checked at the frame's node lists its scope).
		if (m_ScopeReadIn != std::make_pair(stopId, frameIndex))
		{
			m_ScopeReadIn = std::make_pair(stopId, frameIndex);
			m_EvaluateScope.clear();
			if (ScriptEngine::IsInitialized() && values->Node.Node != 0)
			{
				Result<VisualScriptConditionCheck> const checked =
					VisualScriptDebugger::CheckCondition(values->Document, values->Node.Node, std::string_view());
				if (checked)
				{
					m_EvaluateScope = checked.GetValue().Scope;
				}
			}
		}
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		bool const entered =
			ImGui::InputTextWithHint("##evaluate", Texts::EvaluateHint.data(), &m_EvaluateText, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		bool const pressed = ImGui::Button(Texts::Evaluate.data());
		RecordItem(VisualScriptDebuggerItem::EvaluateButton);
		if (pressed || entered)
		{
			Evaluate();
		}
		if (m_EvaluateCell)
		{
			ImGui::SameLine();
			DrawValueCell(*m_EvaluateCell, m_Context, "evaluate");
		}
		else if (!m_EvaluateResult.empty())
		{
			ImGui::SameLine();
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(VisualScriptErrorColor), "%s", m_EvaluateResult.c_str());
			ImGui::PopTextWrapPos();
		}
		if (!m_EvaluateScope.empty())
		{
			ImGui::TextDisabled("%s", Texts::EvaluateNames.data());
			float const listHeight = std::min(static_cast<float>(m_EvaluateScope.size()), nameRows) * nameLine;
			std::optional<std::string> inserted;
			if (ImGui::BeginChild("##evaluateNames", ImVec2(0.0f, listHeight), ImGuiChildFlags_None))
			{
				for (size_t index = 0; index < m_EvaluateScope.size(); index++)
				{
					VisualScriptConditionName const& name = m_EvaluateScope[index];
					if (ImGui::Selectable(fmt::format("{}   {} \xC2\xB7 {}##{}", name.Form, name.Type, name.Source, index).c_str()))
					{
						inserted = name.Form;
					}
					if (index == 0)
					{
						RecordItem(VisualScriptDebuggerItem::EvaluateName);
					}
				}
			}
			ImGui::EndChild();
			if (inserted)
			{
				m_EvaluateText += m_EvaluateText.empty() || m_EvaluateText.back() == ' ' ? *inserted : " " + *inserted;
			}
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
		RecordItem(VisualScriptDebuggerItem::DisableAll);
		ImGui::SameLine();
		ImGui::BeginDisabled(graphs.empty());
		if (ImGui::Button(Texts::RemoveAll.data()))
		{
			m_OpenRemoveAll = true;
		}
		RecordItem(VisualScriptDebuggerItem::RemoveAll);
		ImGui::EndDisabled();
		ImGui::SameLine();
		bool breakOnErrors = m_Session.IsBreakOnErrors();
		if (ImGui::Checkbox(Texts::BreakOnErrors.data(), &breakOnErrors))
		{
			m_Session.SetBreakOnErrors(breakOnErrors);
		}
		RecordItem(VisualScriptDebuggerItem::BreakOnErrors);
		ImGui::SetItemTooltip("%s", Texts::BreakOnErrorsTooltip.data());

		std::string const removeAllId = fmt::format("{}##removeAll", Texts::RemoveAllTitle);
		if (std::exchange(m_OpenRemoveAll, false) && !graphs.empty())
		{
			ImGui::OpenPopup(removeAllId.c_str());
		}
		m_RemoveAllOpen =
			ImGui::BeginPopupModal(removeAllId.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
		if (m_RemoveAllOpen)
		{
			ImGui::TextUnformatted(Texts::RemoveAllQuestion.data());
			bool const remove = ImGui::Button(Texts::Remove.data());
			RecordItem(VisualScriptDebuggerItem::RemoveAllConfirm);
			if (remove)
			{
				m_Session.ClearBreakpoints(std::nullopt);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			bool const cancel = ImGui::Button(Texts::Cancel.data());
			RecordItem(VisualScriptDebuggerItem::RemoveAllCancel);
			if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (graphs.empty())
		{
			std::string const hint =
				fmt::format("{}{}{}", Texts::NoBreakpoints, FormatVisualScriptChord(ImGuiKey_F9, ImGui::GetIO().ConfigMacOSXBehaviors),
			                Texts::NoBreakpointsEnd);
			ImGui::TextDisabled("%s", hint.c_str());
			return;
		}
		VisualScriptDebugState const& state = m_Session.GetState();
		if (!ImGui::BeginTable("##breakpoints", 8, TableFlags))
		{
			return;
		}
		ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn(Texts::ColumnBreakpoint.data(), ImGuiTableColumnFlags_WidthStretch, 0.35f);
		ImGui::TableSetupColumn(Texts::ColumnCondition.data(), ImGuiTableColumnFlags_WidthStretch, 0.25f);
		ImGui::TableSetupColumn(Texts::ColumnHitCount.data(), ImGuiTableColumnFlags_WidthStretch, 0.08f);
		ImGui::TableSetupColumn(Texts::Reached.data(), ImGuiTableColumnFlags_WidthStretch, 0.07f);
		ImGui::TableSetupColumn(Texts::Hits.data(), ImGuiTableColumnFlags_WidthStretch, 0.07f);
		ImGui::TableSetupColumn(Texts::ColumnNotes.data(), ImGuiTableColumnFlags_WidthStretch, 0.18f);
		ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		// Changes are made after the table: they change the store the rows are drawn from.
		std::optional<std::pair<AssetHandle, VisualScriptBreakpoint>> changed;
		std::optional<std::pair<AssetHandle, uint32_t>> removed;
		std::optional<std::pair<AssetHandle, uint32_t>> opened;
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
				RecordItem(VisualScriptDebuggerItem::BreakpointEnabled);
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
				std::string const name = fmt::format("{} \xE2\x80\xBA {} \xE2\x80\xBA {}{}{})", graphName, function, breakpoint.Type,
				                                     Texts::Node, breakpoint.Node);
				ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);
				RecordItem(VisualScriptDebuggerItem::BreakpointRow);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					opened = std::make_pair(graph, breakpoint.Node);
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
					                              ? Texts::NotInProgram.data()
					                              : Texts::Inactive.data());
				}
				ImGui::TableNextColumn();
				// A drawn cross: the editor's fonts have no multiplication sign at button size worth relying on.
				ImVec2 const at = ImGui::GetCursorScreenPos();
				float const size = ImGui::GetFrameHeight();
				if (ImGui::InvisibleButton("##remove", ImVec2(size, size)))
				{
					removed = std::make_pair(graph, breakpoint.Node);
				}
				RecordItem(VisualScriptDebuggerItem::BreakpointRemove);
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
		// A double-click opens the graph's window on the node.
		if (opened && m_OpenNode)
		{
			m_OpenNode(opened->first, opened->second);
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
		ImGui::TextDisabled("%s", fmt::format("{}-{}{}{}", total == 0 ? 0u : m_InstanceOffset + 1,
		                                      std::min(m_InstanceOffset + InstancesPage, total), Texts::LoopOf, total)
		                              .c_str());
		if (!m_Instances || !ImGui::BeginTable("##instances", 5, TableFlags))
		{
			return;
		}
		ImGui::TableSetupColumn(Texts::ColumnEntity.data(), ImGuiTableColumnFlags_WidthStretch, 0.22f);
		ImGui::TableSetupColumn(Texts::ColumnGraph.data(), ImGuiTableColumnFlags_WidthStretch, 0.2f);
		ImGui::TableSetupColumn(Texts::ColumnState.data(), ImGuiTableColumnFlags_WidthStretch, 0.1f);
		ImGui::TableSetupColumn(Texts::ColumnWaitingIn.data(), ImGuiTableColumnFlags_WidthStretch, 0.38f);
		ImGui::TableSetupColumn(Texts::ColumnStops.data(), ImGuiTableColumnFlags_WidthStretch, 0.1f);
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
			RecordItem(VisualScriptDebuggerItem::InstanceRow);
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
				std::string part = fmt::format("{}{}{})", wait.Title, Texts::Node, wait.Node);
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
