#include "Editor/VisualScripting/VisualScriptFindBar.h"

#include "Strada/Core/Log.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <vector>

namespace Strada
{
	float VisualScriptFindBar::GetWidth(float canvasWidth, float fontSize)
	{
		return std::max(0.0f, std::min(WidthInFontSizes * fontSize, canvasWidth - 2.0f * fontSize));
	}

	void VisualScriptFindBar::Open(std::string text)
	{
		m_Open = true;
		m_Focus = true;
		m_FocusWindow = true;
		if (!text.empty())
		{
			m_Text = std::move(text);
		}
	}

	void VisualScriptFindBar::Close()
	{
		m_Open = false;
		m_Focus = false;
		m_Index = -1;
		m_Matches.clear();
		m_Outlined.clear();
		m_Found = false;
	}

	void VisualScriptFindBar::UpdateMatches(VisualScriptPaneContext& context)
	{
		VisualScriptEditorModel const& model = context.Model;
		uint32_t const function = model.GetFunction();
		VisualScriptAnalysis const* const analysis = model.GetAnalysis();
		if (m_Found && m_FoundText == m_Text && m_FoundAllGraphs == m_AllGraphs && m_FoundFunction == function &&
		    m_FoundDocument == model.GetStoredDocument() && m_FoundAnalysis == analysis)
		{
			return;
		}
		std::optional<VisualScriptFindResult> const current = m_Index >= 0 && static_cast<size_t>(m_Index) < m_Matches.size()
		                                                          ? std::optional(m_Matches[static_cast<size_t>(m_Index)])
		                                                          : std::nullopt;
		m_Found = true;
		m_FoundText = m_Text;
		m_FoundAllGraphs = m_AllGraphs;
		m_FoundFunction = function;
		m_FoundDocument = model.GetStoredDocument();
		m_FoundAnalysis = analysis;

		VisualScriptFindQuery query;
		query.Text = m_Text;
		m_Matches = FindInVisualScript(model.GetDisplayDocument(), analysis, query);
		if (!m_AllGraphs)
		{
			std::erase_if(m_Matches,
			              [function](VisualScriptFindResult const& match)
			              {
							  return match.Function != function;
						  });
		}
		// The match gone to last stays the current one while it still matches.
		m_Index = -1;
		if (current)
		{
			auto const found = std::find(m_Matches.begin(), m_Matches.end(), *current);
			m_Index = found != m_Matches.end() ? static_cast<int>(found - m_Matches.begin()) : -1;
		}
		m_Outlined.clear();
		for (VisualScriptFindResult const& match : m_Matches)
		{
			uint32_t const element = match.Node != 0 ? match.Node : match.Comment;
			if (match.Function == function && element != 0 && std::find(m_Outlined.begin(), m_Outlined.end(), element) == m_Outlined.end())
			{
				m_Outlined.push_back(element);
			}
		}
	}

	void VisualScriptFindBar::GoTo(VisualScriptPaneContext& context, int step, VisualScriptPaneResult& result)
	{
		if (m_Matches.empty())
		{
			return;
		}
		auto const count = static_cast<int>(m_Matches.size());
		m_Index = m_Index < 0 ? (step > 0 ? 0 : count - 1) : ((m_Index + step) % count + count) % count;
		VisualScriptFindResult const& match = m_Matches[static_cast<size_t>(m_Index)];
		uint32_t const element = match.Node != 0 ? match.Node : match.Comment;
		if (element != 0)
		{
			// The match becomes the selection, shown in Details.
			context.Shared.Element.reset();
			result.Open = MakeVisualScriptJump(context.Graph, match.Function, {element});
			return;
		}
		if (match.Variable != 0)
		{
			if (match.Function != 0)
			{
				result.ShowFunction = match.Function;
			}
			SelectVisualScriptElement(
				context, {match.Function != 0 ? VisualScriptElementKind::Local : VisualScriptElementKind::Variable, match.Variable});
			return;
		}
		SelectVisualScriptElement(context, {VisualScriptElementKind::Function, match.Function});
	}

	VisualScriptPaneResult VisualScriptFindBar::Draw(VisualScriptPaneContext& context)
	{
		VisualScriptPaneResult result;
		if (!m_Open)
		{
			return result;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		UpdateMatches(context);
		std::string const position = m_Matches.empty() ? std::string("No matches") : fmt::format("{} of {}", GetIndex(), GetCount());
		float const controls = ImGui::CalcTextSize("No matches").x + ImGui::GetFrameHeight() * 3.0f + ImGui::CalcTextSize("All graphs").x +
		                       ImGui::CalcTextSize("x").x + style.FramePadding.x * 4.0f + style.ItemInnerSpacing.x +
		                       style.ItemSpacing.x * 5.0f;
		if (m_FocusWindow)
		{
			// Opening the bar gives it the keyboard: its window takes the focus, as a window's Ctrl+F gives it.
			ImGui::SetWindowFocus();
			m_FocusWindow = false;
		}
		if (m_Focus)
		{
			ImGui::SetKeyboardFocusHere();
			m_Focus = false;
		}
		ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - controls, ImGui::GetFontSize() * 4.0f));
		int step = 0;
		if (ImGui::InputTextWithHint("##find", "Find", &m_Text, ImGuiInputTextFlags_EnterReturnsTrue))
		{
			// Enter goes on; the field keeps the keyboard.
			step = ImGui::GetIO().KeyShift ? -1 : 1;
			m_Focus = true;
		}
		else if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			Close();
			return result;
		}
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", position.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(m_Matches.empty());
		if (ImGui::ArrowButton("##previous", ImGuiDir_Up))
		{
			step = -1;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Previous (Shift+Enter)");
		}
		ImGui::SameLine();
		if (ImGui::ArrowButton("##next", ImGuiDir_Down))
		{
			step = 1;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Next (Enter)");
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Checkbox("All graphs", &m_AllGraphs);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Find in every graph of the script, else in the one shown");
		}
		ImGui::SameLine();
		bool const close = ImGui::SmallButton("x");
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Close (Escape)");
		}
		// Escape closes the bar while its own window has the keyboard; on the canvas beside it, the window decides (§8.3).
		if (close || (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsAnyItemActive() &&
		              ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
		{
			Close();
			return result;
		}
		if (step != 0)
		{
			UpdateMatches(context);
			GoTo(context, step, result);
		}
		return result;
	}
}
