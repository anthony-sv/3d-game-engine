#pragma once

#include "Editor/UI/CurveEditor.h"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <utility>

// The definitions of CurveEditor's members (CurveEditor.h). Included only by the translation unit that instantiates the
// widget for a document type (template class CurveEditor<TDocument>;), after the document's CurveDocumentTraits and the
// view's instantiation (CurveView<TDocument>).
namespace Strada
{
	namespace CurveEditorDetail
	{
		// The ruler's ticks are at least this many font sizes apart; the value lines at least this many.
		inline constexpr float TickSpacing = 4.0f;
		inline constexpr float ValueLineSpacing = 2.5f;

		inline ImVec2 ToImVec(glm::vec2 value)
		{
			return ImVec2(value.x, value.y);
		}

		inline glm::vec2 ToVec(ImVec2 value)
		{
			return glm::vec2(value.x, value.y);
		}

		inline uint32_t WithAlpha(uint32_t color, float alpha)
		{
			auto const current = static_cast<float>((color >> 24) & 0xFF);
			return (color & 0x00FFFFFFu) | (static_cast<uint32_t>(std::clamp(current * alpha, 0.0f, 255.0f)) << 24);
		}
	}

	template<typename TDocument>
	CurveEditor<TDocument>::CurveEditor(uint64_t nudgeMergeKey)
		: m_NudgeBase(nudgeMergeKey)
	{
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::BeginFrame()
	{
		m_Rects.clear();
		m_Record = {};
		m_InteractionActive = false;
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::Reset()
	{
		m_View = {};
		m_HeaderStart.reset();
		m_Actions.clear();
		m_CancelGesture = false;
		m_OpenKeyMenu = false;
	}

	template<typename TDocument>
	std::optional<std::pair<glm::vec2, glm::vec2>> CurveEditor<TDocument>::FindRect(std::string_view part) const
	{
		auto const found = std::find_if(m_Rects.begin(), m_Rects.end(),
		                                [part](auto const& rect)
		                                {
											return rect.first == part;
										});
		return found != m_Rects.end() ? std::optional(found->second) : std::nullopt;
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::AddRect(std::string label)
	{
		m_Rects.emplace_back(std::move(label), std::pair(CurveEditorDetail::ToVec(ImGui::GetItemRectMin()),
		                                                 CurveEditorDetail::ToVec(ImGui::GetItemRectMax())));
	}

	template<typename TDocument>
	uint64_t CurveEditor<TDocument>::GetNudgeKey(double time)
	{
		// As the canvas's nudges: a run of presses less than NudgeRunSeconds apart is one step.
		if (m_NudgeKey == 0 || time - m_LastNudge >= NudgeRunSeconds)
		{
			m_NudgeKey = m_NudgeBase + ++m_NudgeRuns;
		}
		m_LastNudge = time;
		return m_NudgeKey;
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::Add(Frame const& frame, std::vector<Change>& changes, typename View::Intent intent)
	{
		using Traits = typename View::Traits;
		switch (intent.Kind)
		{
			case CurveIntentKind::Edit:
				if (!frame.ReadOnly)
				{
					changes.push_back(
						Change{CurveEditorChangeKind::Drag, std::move(Traits::GetDocument(intent)), std::move(intent.Action), true, 0});
				}
				break;
			case CurveIntentKind::EndEdit:
				changes.push_back(Change{CurveEditorChangeKind::EndDrag, {}, {}, false, 0});
				break;
			case CurveIntentKind::CancelEdit:
				changes.push_back(Change{CurveEditorChangeKind::CancelDrag, std::move(Traits::GetDocument(intent)), {}, false, 0});
				break;
			case CurveIntentKind::Commit:
				if (!frame.ReadOnly)
				{
					changes.push_back(
						Change{CurveEditorChangeKind::Commit, std::move(Traits::GetDocument(intent)), std::move(intent.Action), false, 0});
				}
				break;
			case CurveIntentKind::OpenKeyMenu:
				m_OpenKeyMenu = true;
				break;
		}
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::DrawHeader(Frame const& frame, std::vector<Change>& changes)
	{
		using Traits = typename View::Traits;
		TDocument const& document = frame.Document;
		// The selection follows the document shown before the header reads its first key (an undo may have removed keys).
		m_View.Sync(document);
		std::span<CurveKeyRef const> const selection = m_View.GetSelection();
		bool const any = !selection.empty();
		bool const curves = std::any_of(selection.begin(), selection.end(),
		                                [&document](CurveKeyRef const& key)
		                                {
											return Traits::GetComponents(document, key.first) > 0;
										});
		float const fontSize = ImGui::GetFontSize();
		CurveDomain const domain = Traits::GetDomain(document);
		ImGui::BeginDisabled(frame.ReadOnly);

		// The first selected key's time and value.
		ImGui::BeginDisabled(!any);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Time");
		ImGui::SameLine();
		CurveKeyRef const first = any ? selection.front() : CurveKeyRef(0, 0);
		float time = any ? Traits::GetKeyTime(document, first.first, first.second) : 0.0f;
		ImGui::SetNextItemWidth(6.0f * fontSize);
		// A bounded domain clamps the field to itself. An unbounded one has no bounds (ImGui: equal bounds), and keeps the
		// digits its format does not show: a drag rounds to the format otherwise (imgui_widgets.cpp, DragBehaviorT).
		float const low = domain.Bounded ? static_cast<float>(domain.Start) : 0.0f;
		float const high = domain.Bounded ? static_cast<float>(domain.End) : 0.0f;
		bool const timeChanged = ImGui::DragFloat("##time", &time, static_cast<float>(m_View.GetTimeStep()), low, high, Traits::TimeFormat,
		                                          domain.Bounded ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_NoRoundToFormat);
		AddRect("Time");
		if (ImGui::IsItemActivated())
		{
			m_HeaderStart = document;
		}
		bool const timeActive = ImGui::IsItemActive();
		if (timeChanged && any && m_HeaderStart && first.first < Traits::GetTrackCount(*m_HeaderStart) &&
		    first.second < Traits::GetKeyCount(*m_HeaderStart, first.first))
		{
			// From the interaction's start: the selection moves as a block by the total change of the first key's time.
			TDocument moved = *m_HeaderStart;
			std::vector<CurveKeyRef> const keys(selection.begin(), selection.end());
			float const offset = time - Traits::GetKeyTime(*m_HeaderStart, first.first, first.second);
			if (Traits::MoveKeys(moved, keys, offset, glm::vec4(0.0f)) && !frame.ReadOnly)
			{
				std::string action = View::DescribeKeys(*m_HeaderStart, keys, "Move", frame.Title);
				changes.push_back(Change{CurveEditorChangeKind::Change, std::move(moved), std::move(action), timeActive, 0});
			}
		}
		m_InteractionActive = m_InteractionActive || timeActive;
		if (ImGui::IsItemDeactivated())
		{
			m_HeaderStart.reset();
			changes.push_back(Change{CurveEditorChangeKind::End, {}, {}, false, 0});
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		bool const valueKey = any && Traits::GetComponents(document, first.first) > 0;
		ImGui::BeginDisabled(!valueKey);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Value");
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::PushItemWidth(std::max(14.0f * fontSize, 1.0f));
		std::optional<glm::vec4> value;
		if (valueKey && frame.ValueField)
		{
			value = frame.ValueField(first.first, first.second);
		}
		else
		{
			ImGui::TextDisabled("-");
		}
		ImGui::PopItemWidth();
		ImGui::EndGroup();
		AddRect("Value");
		bool const valueActive = ImGui::IsItemActive();
		bool const valueDeactivated = ImGui::IsItemDeactivated();
		if (value && valueKey)
		{
			TDocument edited = document;
			if (Traits::SetKeyValue(edited, first, *value) && !frame.ReadOnly)
			{
				std::array<CurveKeyRef, 1> const keys = {first};
				std::string action = View::DescribeKeys(document, keys, "Set Value of", frame.Title);
				changes.push_back(Change{CurveEditorChangeKind::Change, std::move(edited), std::move(action), valueActive, 0});
			}
		}
		m_InteractionActive = m_InteractionActive || valueActive;
		if (valueDeactivated)
		{
			changes.push_back(Change{CurveEditorChangeKind::End, {}, {}, false, 0});
		}
		ImGui::EndDisabled();

		// Interpolation, tangents and Frame.
		ImGui::SameLine();
		ImGui::BeginDisabled(!curves);
		for (auto const& [interpolation, name] :
		     {std::pair(CurveInterpolation::Cubic, "Cubic"), std::pair(CurveInterpolation::Linear, "Linear"),
		      std::pair(CurveInterpolation::Constant, "Constant")})
		{
			if (ImGui::SmallButton(name))
			{
				if (std::optional<typename View::Intent> intent = m_View.SetInterpolation(document, interpolation, frame.Title))
				{
					Add(frame, changes, std::move(*intent));
				}
			}
			AddRect(name);
			ImGui::SameLine();
		}
		if (ImGui::SmallButton("Auto"))
		{
			if (std::optional<typename View::Intent> intent = m_View.AutoTangents(document, frame.Title))
			{
				Add(frame, changes, std::move(*intent));
			}
		}
		AddRect("Auto");
		ImGui::SameLine();
		if (ImGui::SmallButton("Flat"))
		{
			if (std::optional<typename View::Intent> intent = m_View.FlattenTangents(document, frame.Title))
			{
				Add(frame, changes, std::move(*intent));
			}
		}
		AddRect("Flat");
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::SmallButton("Frame"))
		{
			m_View.Frame(document);
		}
		AddRect("Frame");
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::DrawCurves(Frame const& frame, std::vector<Change>& changes)
	{
		using Traits = typename View::Traits;
		using CurveEditorDetail::ToImVec;
		using CurveEditorDetail::ToVec;
		using CurveEditorDetail::WithAlpha;
		TDocument const& document = frame.Document;
		float const fontSize = ImGui::GetFontSize();
		ImVec2 const origin = ImGui::GetCursorScreenPos();
		ImVec2 const size(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));
		ImGui::InvisibleButton("##curveView", size,
		                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		AddRect("Curves");
		bool const hovered = ImGui::IsItemHovered();
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		glm::vec2 const min = ToVec(origin);
		glm::vec2 const max(origin.x + size.x, origin.y + size.y);
		m_View.SetRect(min, max, fontSize);
		m_View.Sync(document);

		if (m_CancelGesture)
		{
			m_CancelGesture = false;
			for (typename View::Intent& intent : m_View.CancelGesture())
			{
				Add(frame, changes, std::move(intent));
			}
		}
		ImGuiIO const& io = ImGui::GetIO();
		CurveViewInput input;
		input.Mouse = ImGui::IsMousePosValid() ? ToVec(io.MousePos) : glm::vec2(-FLT_MAX);
		input.Hovered = hovered;
		for (int button = 0; button < 3; button++)
		{
			auto const index = static_cast<size_t>(button);
			input.Pressed[index] = io.MouseClicked[button] && hovered;
			input.Down[index] = io.MouseDown[button];
			input.Released[index] = io.MouseReleased[button];
		}
		input.DoubleClicked = input.Pressed[0] && io.MouseDoubleClicked[0];
		input.Shift = io.KeyShift;
		input.Ctrl = io.KeyCtrl;
		input.Alt = io.KeyAlt;
		input.Wheel = hovered ? io.MouseWheel : 0.0f;
		for (typename View::Intent& intent : m_View.Update(input, document, frame.Title))
		{
			Add(frame, changes, std::move(intent));
		}
		CurveGesture const gesture = m_View.GetGesture();
		if (gesture == CurveGesture::MoveKeys || gesture == CurveGesture::Tangent)
		{
			m_InteractionActive = true;
		}

		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(origin, ToImVec(max), true);
		uint32_t const text = ImGui::GetColorU32(ImGuiCol_Text);
		uint32_t const disabled = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		uint32_t const grid = ImGui::GetColorU32(ImGuiCol_Border);
		uint32_t const selectionColor = ImGui::GetColorU32(ImGuiCol_CheckMark);
		drawList->AddRectFilled(origin, ToImVec(max), ImGui::GetColorU32(ImGuiCol_FrameBg));

		// A bounded domain: what lies before its start and after its end, shaded.
		CurveDomain const domain = Traits::GetDomain(document);
		float const start = domain.Bounded ? m_View.ToScreenX(domain.Start) : min.x;
		float const end = domain.Bounded ? m_View.ToScreenX(domain.End) : max.x;
		if (domain.Bounded)
		{
			uint32_t const outside = WithAlpha(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.6f);
			if (start > min.x)
			{
				drawList->AddRectFilled(origin, ImVec2(start, max.y), outside);
			}
			if (end < max.x)
			{
				drawList->AddRectFilled(ImVec2(end, min.y), ToImVec(max), outside);
			}
		}

		// The ruler: times across, a tick at least TickSpacing font sizes apart (so at most one per that many pixels).
		float const rulerBottom = m_View.GetLanesTop();
		drawList->AddRectFilled(origin, ImVec2(max.x, rulerBottom), ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
		glm::dvec2 const times = m_View.GetTimeRange();
		double const tick = GetNiceCurveStep((times.y - times.x) * CurveEditorDetail::TickSpacing * fontSize / static_cast<double>(size.x));
		auto const [firstTick, ticks] =
			GetCurveRulerLines(times.x, times.y, tick, static_cast<int>(size.x / (CurveEditorDetail::TickSpacing * fontSize)) + 2);
		for (int line = 0; line < ticks; line++)
		{
			double const at = (firstTick + line) * tick;
			float const x = m_View.ToScreenX(at);
			drawList->AddLine(ImVec2(x, rulerBottom - fontSize * 0.35f), ImVec2(x, rulerBottom), disabled);
			drawList->AddLine(ImVec2(x, rulerBottom), ImVec2(x, max.y), WithAlpha(grid, 0.5f));
			std::string const label = fmt::format("{:g}", std::abs(at) < tick * 1e-6 ? 0.0 : at);
			drawList->AddText(ImVec2(x + 2.0f, origin.y + 1.0f), disabled, label.c_str());
		}
		if (domain.Bounded)
		{
			drawList->AddLine(ImVec2(end, origin.y), ImVec2(end, max.y), text);
		}

		// Values up the side, a line at least ValueLineSpacing font sizes apart.
		float const curvesTop = m_View.GetCurvesTop();
		float const curvesHeight = std::max(max.y - curvesTop, 1.0f);
		glm::dvec2 const values = m_View.GetValueRange();
		double const step =
			GetNiceCurveStep((values.y - values.x) * CurveEditorDetail::ValueLineSpacing * fontSize / static_cast<double>(curvesHeight));
		auto const [firstValue, valueLines] = GetCurveRulerLines(
			values.x, values.y, step, static_cast<int>(curvesHeight / (CurveEditorDetail::ValueLineSpacing * fontSize)) + 2);
		for (int line = 0; line < valueLines; line++)
		{
			double const at = (firstValue + line) * step;
			float const y = m_View.ToScreenY(at);
			if (y < curvesTop)
			{
				continue;
			}
			drawList->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), WithAlpha(grid, std::abs(at) < step * 1e-6 ? 1.0f : 0.5f));
			std::string const label = fmt::format("{:g}", std::abs(at) < step * 1e-6 ? 0.0 : at);
			drawList->AddText(ImVec2(min.x + 2.0f, y - fontSize - 1.0f), disabled, label.c_str());
		}

		float const radius = View::KeyRadius * fontSize;
		uint32_t const trackCount = Traits::GetTrackCount(document);
		// The event lanes, under the ruler.
		for (uint32_t track = 0; track < trackCount; track++)
		{
			std::optional<std::pair<float, float>> const lane = m_View.GetLane(track);
			if (!lane)
			{
				continue;
			}
			drawList->AddRectFilled(ImVec2(min.x, lane->first), ImVec2(max.x, lane->second), WithAlpha(grid, 0.35f));
			std::string const name(Traits::GetTrackName(document, track));
			drawList->AddText(ImVec2(min.x + 2.0f, lane->first), disabled, name.c_str());
			uint32_t const color = frame.Color ? frame.Color(track, 0) : text;
			for (uint32_t key = 0; key < Traits::GetKeyCount(document, track); key++)
			{
				if (std::optional<glm::vec2> const position = m_View.GetKeyPosition(document, track, key, 0))
				{
					bool const selected = m_View.IsSelected(track, key);
					DrawCurveKeyDiamond(drawList, *position, radius, selected ? selectionColor : color,
					                    selected ? text : WithAlpha(text, 0.5f));
					m_Record.EventKeys++;
				}
			}
		}

		// The curves, their keys and the tangent handles of the selected cubic keys.
		drawList->PushClipRect(ImVec2(min.x, curvesTop), ToImVec(max), true);
		std::vector<ImVec2> points;
		for (uint32_t track = 0; track < trackCount; track++)
		{
			uint32_t const components = Traits::GetComponents(document, track);
			for (uint32_t component = 0; component < components; component++)
			{
				if (!m_View.IsVisible(document, track, component))
				{
					continue;
				}
				uint32_t const color = frame.Color ? frame.Color(track, component) : text;
				auto const index = static_cast<glm::length_t>(component);
				points.clear();
				float const from = std::max(min.x, start);
				float const to = std::min(max.x, end);
				for (float x = from; x < to + CurveStep; x += CurveStep)
				{
					float const clamped = std::min(x, to);
					double const at =
						domain.Bounded ? std::clamp(m_View.ToTime(clamped), domain.Start, domain.End) : m_View.ToTime(clamped);
					points.emplace_back(clamped, m_View.ToScreenY(Traits::Evaluate(document, track, at)[index]));
				}
				if (points.size() >= 2)
				{
					drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, frame.Emphasized == track ? 2.0f : 1.5f);
					m_Record.Curves++;
				}
				for (uint32_t key = 0; key < Traits::GetKeyCount(document, track); key++)
				{
					std::optional<glm::vec2> const position = m_View.GetKeyPosition(document, track, key, component);
					if (!position)
					{
						continue;
					}
					bool const selected = m_View.IsSelected(track, key);
					for (bool const out : {false, true})
					{
						if (std::optional<glm::vec2> const handle = m_View.GetTangentHandle(document, track, key, component, out))
						{
							drawList->AddLine(ToImVec(*position), ToImVec(*handle), WithAlpha(text, 0.7f));
							drawList->AddCircleFilled(ToImVec(*handle), radius * 0.6f, selectionColor);
							m_Record.TangentHandles++;
						}
					}
					DrawCurveKeyDiamond(drawList, *position, radius, selected ? selectionColor : color,
					                    selected ? text : WithAlpha(text, 0.5f));
					m_Record.CurveKeys++;
				}
			}
		}
		drawList->PopClipRect();

		if (std::optional<std::pair<glm::vec2, glm::vec2>> const marquee = m_View.GetMarquee())
		{
			drawList->AddRectFilled(ToImVec(marquee->first), ToImVec(marquee->second), WithAlpha(selectionColor, 0.15f));
			drawList->AddRect(ToImVec(marquee->first), ToImVec(marquee->second), selectionColor);
			m_Record.Marquee = true;
		}
		// The playhead, over the ruler and the curves.
		if (m_Playhead && std::isfinite(*m_Playhead))
		{
			double const at = domain.Bounded ? std::clamp(*m_Playhead, domain.Start, domain.End) : *m_Playhead;
			float const x = m_View.ToScreenX(at);
			drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, max.y), frame.PlayheadColor, std::max(std::round(fontSize * 0.15f), 1.0f));
			m_Record.Playhead = true;
			m_Record.PlayheadX = x;
		}
		drawList->PopClipRect();
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::DrawKeyMenu(Frame const& frame, std::vector<Change>& changes)
	{
		using Traits = typename View::Traits;
		if (std::exchange(m_OpenKeyMenu, false))
		{
			ImGui::OpenPopup("##keyMenu");
		}
		if (!ImGui::BeginPopup("##keyMenu"))
		{
			return;
		}
		TDocument const& document = frame.Document;
		std::span<CurveKeyRef const> const selection = m_View.GetSelection();
		bool const curves =
			std::any_of(selection.begin(), selection.end(),
		                [&document](CurveKeyRef const& key)
		                {
							return key.first < Traits::GetTrackCount(document) && Traits::GetComponents(document, key.first) > 0;
						});
		ImGui::BeginDisabled(frame.ReadOnly);
		std::optional<typename View::Intent> intent;
		if (ImGui::BeginMenu("Interpolation", curves))
		{
			if (ImGui::MenuItem("Cubic"))
			{
				intent = m_View.SetInterpolation(document, CurveInterpolation::Cubic, frame.Title);
			}
			if (ImGui::MenuItem("Linear"))
			{
				intent = m_View.SetInterpolation(document, CurveInterpolation::Linear, frame.Title);
			}
			if (ImGui::MenuItem("Constant"))
			{
				intent = m_View.SetInterpolation(document, CurveInterpolation::Constant, frame.Title);
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Tangents", curves))
		{
			if (ImGui::MenuItem("Auto"))
			{
				intent = m_View.AutoTangents(document, frame.Title);
			}
			if (ImGui::MenuItem("Flat"))
			{
				intent = m_View.FlattenTangents(document, frame.Title);
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Delete", "Delete"))
		{
			intent = m_View.DeleteSelection(document, frame.Title);
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
		if (intent)
		{
			Add(frame, changes, std::move(*intent));
		}
	}

	template<typename TDocument>
	void CurveEditor<TDocument>::ApplyActions(Frame const& frame, std::vector<Change>& changes)
	{
		using Traits = typename View::Traits;
		TDocument const& document = frame.Document;
		for (CurveEditorAction const action : std::exchange(m_Actions, {}))
		{
			switch (action)
			{
				case CurveEditorAction::Delete:
					if (!frame.ReadOnly)
					{
						if (std::optional<typename View::Intent> intent = m_View.DeleteSelection(document, frame.Title))
						{
							Add(frame, changes, std::move(*intent));
						}
					}
					break;
				case CurveEditorAction::SelectAll:
					m_View.SelectAll(document);
					break;
				case CurveEditorAction::NudgeLeft:
				case CurveEditorAction::NudgeRight:
				case CurveEditorAction::NudgeLeftCoarse:
				case CurveEditorAction::NudgeRightCoarse:
					if (!frame.ReadOnly)
					{
						int const steps = action == CurveEditorAction::NudgeLeft || action == CurveEditorAction::NudgeLeftCoarse ? -1 : 1;
						bool const coarse = action == CurveEditorAction::NudgeLeftCoarse || action == CurveEditorAction::NudgeRightCoarse;
						// A run of presses and repeats is one step.
						if (std::optional<typename View::Intent> nudged = m_View.Nudge(document, coarse ? steps * 10 : steps, frame.Title))
						{
							changes.push_back(Change{CurveEditorChangeKind::Commit, std::move(Traits::GetDocument(*nudged)),
							                         std::move(nudged->Action), false, GetNudgeKey(frame.Time)});
						}
					}
					break;
				case CurveEditorAction::Frame:
					m_View.Frame(document);
					break;
			}
		}
	}
}
