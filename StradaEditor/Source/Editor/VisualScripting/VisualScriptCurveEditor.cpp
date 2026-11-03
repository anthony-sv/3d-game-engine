#include "Editor/VisualScripting/VisualScriptCurveEditor.h"

#include "Editor/VisualScripting/VisualScriptController.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

namespace Strada
{
	namespace
	{
		// The colours of the components of vectors and colours: X (R), Y (G), Z (B) and W (A).
		constexpr std::array<uint32_t, 4> ComponentColors = {
			VisualScriptRgba(235, 92, 80),
			VisualScriptRgba(120, 200, 90),
			VisualScriptRgba(90, 150, 240),
			VisualScriptRgba(200, 200, 200),
		};
		// The ruler's ticks are at least this many font sizes apart; the value lines at least this many.
		constexpr float TickSpacing = 4.0f;
		constexpr float ValueLineSpacing = 2.5f;

		ImVec2 ToImVec(glm::vec2 value)
		{
			return ImVec2(value.x, value.y);
		}

		glm::vec2 ToVec(ImVec2 value)
		{
			return glm::vec2(value.x, value.y);
		}

		uint32_t WithAlpha(uint32_t color, float alpha)
		{
			auto const current = static_cast<float>((color >> 24) & 0xFF);
			return (color & 0x00FFFFFFu) | (static_cast<uint32_t>(std::clamp(current * alpha, 0.0f, 255.0f)) << 24);
		}

		std::string DescribeLinks(uint32_t count, std::string_view verb)
		{
			return count == 0 ? std::string() : fmt::format(" ({}{} link{})", verb, count, count == 1 ? "" : "s");
		}

		// The lines of a ruler, the multiples of step within [low, high]: the index of the first (its multiple of step) and how
		// many, at most limit of them; none for a range or a step that is not finite.
		std::pair<double, int> GetRulerLines(double low, double high, double step, int limit)
		{
			if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(step) || !(step > 0.0) || !(high >= low))
			{
				return {0.0, 0};
			}
			double const first = std::ceil(low / step);
			double const last = std::floor(high / step);
			if (!std::isfinite(first) || !std::isfinite(last) || !(last >= first))
			{
				return {0.0, 0};
			}
			return {first, static_cast<int>(std::min(last - first + 1.0, static_cast<double>(std::max(limit, 0))))};
		}

		// A step of 1, 2 or 5 times a power of ten that is at least minimum.
		double NiceStep(double minimum)
		{
			if (!(minimum > 0.0) || !std::isfinite(minimum))
			{
				return 1.0;
			}
			double const power = std::pow(10.0, std::floor(std::log10(minimum)));
			for (double const factor : {1.0, 2.0, 5.0, 10.0})
			{
				if (factor * power >= minimum)
				{
					return factor * power;
				}
			}
			return 10.0 * power;
		}

		// A key's diamond.
		void DrawDiamond(ImDrawList* drawList, glm::vec2 center, float radius, uint32_t fill, uint32_t outline)
		{
			ImVec2 const top(center.x, center.y - radius);
			ImVec2 const right(center.x + radius, center.y);
			ImVec2 const bottom(center.x, center.y + radius);
			ImVec2 const left(center.x - radius, center.y);
			drawList->AddQuadFilled(top, right, bottom, left, fill);
			drawList->AddQuad(top, right, bottom, left, outline, 1.0f);
		}

		// The default name base of a new track of a type.
		std::string_view GetTrackNameBase(std::string_view type)
		{
			return type == VisualScriptTimelineEventType ? "Event" : "Curve";
		}
	}

	struct VisualScriptCurveEditor::Frame
	{
		VisualScriptPaneContext& Context;
		VisualScriptWidgetCommits& Commits;
		VisualScriptPaneResult& Result;
		// The pending widget value shown, kept alive while a widget replaces it.
		Ref<VisualScriptDocument const> Hold;
		VisualScriptDocument const& Document;
		Ref<VisualScriptLibrary const> LibraryHold;
		VisualScriptLibrary const* Library = nullptr;
		uint32_t Node = 0;
		VisualScriptTimeline Timeline;
		std::string Title;
		bool ReadOnly = false;
		// The node in Document.
		VisualScriptNode const* Stored = nullptr;
	};

	Result<VisualScriptTimelineEdit> MakeVisualScriptTimelineEdit(VisualScriptDocument const& document, uint32_t node,
	                                                              VisualScriptTimeline const& timeline, std::string action,
	                                                              std::span<VisualScriptEditing::VisualScriptTrackRename const> renames)
	{
		VisualScriptTimelineEdit edit{document, std::move(action)};
		Result<uint32_t> const removed = VisualScriptEditing::SetTimeline(edit.Document, node, timeline, renames);
		if (!removed)
		{
			return Error{removed.GetError()};
		}
		edit.Action += DescribeLinks(removed.GetValue(), "removes ");
		return edit;
	}

	Result<VisualScriptTimelineEdit> AddVisualScriptTimelineTrackEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                  VisualScriptTimeline const& timeline, std::string_view type,
	                                                                  std::string_view title)
	{
		VisualScriptTimeline edited = timeline;
		std::string name = MakeVisualScriptTimelineTrackName(timeline, GetTrackNameBase(type));
		std::string action = fmt::format("Add Track {} to {}", name, title);
		if (Result<uint32_t> const added = AddVisualScriptTimelineTrack(edited, std::move(name), std::string(type)); !added)
		{
			return Error{added.GetError()};
		}
		return MakeVisualScriptTimelineEdit(document, node, edited, std::move(action));
	}

	Result<VisualScriptTimelineEdit> RenameVisualScriptTimelineTrackEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                     VisualScriptTimeline const& timeline, uint32_t track,
	                                                                     std::string const& name, std::string_view title)
	{
		if (track >= timeline.Tracks.size())
		{
			return MakeError("the timeline has no track {}", track);
		}
		std::string const& old = timeline.Tracks[track].Name;
		VisualScriptTimeline edited = timeline;
		if (Result<void> const renamed = RenameVisualScriptTimelineTrack(edited, track, name); !renamed)
		{
			return Error{renamed.GetError()};
		}
		uint32_t const links = CountVisualScriptTrackLinks(document, node, old);
		std::string action = fmt::format("Rename Track {} to {} on {}{}", old, name, title,
		                                 links == 0 ? std::string() : fmt::format(" (and {} link{})", links, links == 1 ? "" : "s"));
		std::array<VisualScriptEditing::VisualScriptTrackRename, 1> const renames = {{{old, name}}};
		return MakeVisualScriptTimelineEdit(document, node, edited, std::move(action), renames);
	}

	Result<VisualScriptTimelineEdit> RemoveVisualScriptTimelineTrackEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                     VisualScriptTimeline const& timeline, uint32_t track,
	                                                                     std::string_view title)
	{
		if (track >= timeline.Tracks.size())
		{
			return MakeError("the timeline has no track {}", track);
		}
		VisualScriptTimeline edited = timeline;
		std::string action = fmt::format("Remove Track {} from {}", timeline.Tracks[track].Name, title);
		if (Result<void> const removed = RemoveVisualScriptTimelineTrack(edited, track); !removed)
		{
			return Error{removed.GetError()};
		}
		return MakeVisualScriptTimelineEdit(document, node, edited, std::move(action));
	}

	Result<VisualScriptTimelineEdit> ChangeVisualScriptTimelineTrackTypeEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                         VisualScriptTimeline const& timeline, uint32_t track,
	                                                                         std::string_view type, std::string_view title)
	{
		if (track >= timeline.Tracks.size())
		{
			return MakeError("the timeline has no track {}", track);
		}
		VisualScriptTimeline edited = timeline;
		std::string action =
			fmt::format("Change {} to {} on {}", timeline.Tracks[track].Name, GetVisualScriptTimelineTypeLabel(type), title);
		if (Result<void> const changed = SetVisualScriptTimelineTrackType(edited, track, std::string(type)); !changed)
		{
			return Error{changed.GetError()};
		}
		return MakeVisualScriptTimelineEdit(document, node, edited, std::move(action));
	}

	Result<VisualScriptTimelineEdit> SetVisualScriptTimelineLengthEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                   VisualScriptTimeline const& start, float length,
	                                                                   std::string_view title)
	{
		VisualScriptTimeline edited = start;
		if (Result<void> const set = SetVisualScriptTimelineLength(edited, length); !set)
		{
			return Error{set.GetError()};
		}
		uint32_t const moved = CountVisualScriptTimelineKeysAfter(start, length);
		std::string action = fmt::format("Set Length on {}{}", title,
		                                 moved == 0 ? std::string() : fmt::format(" (moves {} key{})", moved, moved == 1 ? "" : "s"));
		return MakeVisualScriptTimelineEdit(document, node, edited, std::move(action));
	}

	Result<VisualScriptTimelineEdit> SetVisualScriptTimelineLoopEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                 VisualScriptTimeline const& timeline, bool loop,
	                                                                 std::string_view title)
	{
		VisualScriptTimeline edited = timeline;
		edited.Loop = loop;
		return MakeVisualScriptTimelineEdit(document, node, edited,
		                                    loop ? fmt::format("Loop {}", title) : fmt::format("Stop Looping {}", title));
	}

	uint32_t CountVisualScriptTrackLinks(VisualScriptDocument const& document, uint32_t node, std::string_view track)
	{
		std::optional<uint32_t> const function = FindVisualScriptElementGraph(document, node);
		VisualScriptGraph const* const graph = function ? FindVisualScriptGraph(document, *function) : nullptr;
		if (graph == nullptr)
		{
			return 0;
		}
		return static_cast<uint32_t>(
			std::count_if(graph->Links.begin(), graph->Links.end(),
		                  [node, track](VisualScriptLink const& link)
		                  {
							  std::string_view const pin = link.From.Pin;
							  return link.From.Node == node &&
			                         (pin == track || (pin.size() > track.size() && pin.starts_with(track) && pin[track.size()] == '.'));
						  }));
	}

	uint32_t GetVisualScriptTrackColor(VisualScriptTimelineTrack const& track, uint32_t component, VisualScriptLibrary const* library)
	{
		if (track.Components <= 1)
		{
			return GetVisualScriptTypeColor(track.Type, library);
		}
		return ComponentColors[std::min<uint32_t>(component, 3)];
	}

	char GetVisualScriptTrackComponentLetter(VisualScriptTimelineTrack const& track, uint32_t component)
	{
		constexpr std::string_view Vector = "XYZW";
		constexpr std::string_view Color = "RGBA";
		std::string_view const letters = track.Type == "Strada.Color" ? Color : Vector;
		return letters[std::min<size_t>(component, 3)];
	}

	void DrawVisualScriptTrackPreview(VisualScriptTimeline const& timeline, uint32_t track, VisualScriptLibrary const* library,
	                                  glm::vec2 min, glm::vec2 max)
	{
		if (track >= timeline.Tracks.size() || !(max.x > min.x) || !(max.y > min.y))
		{
			return;
		}
		VisualScriptTimelineTrack const& shown = timeline.Tracks[track];
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(ToImVec(min), ToImVec(max), ImGui::GetColorU32(ImGuiCol_FrameBg));
		float const width = max.x - min.x;
		float const height = max.y - min.y;
		double const length = timeline.Length;
		auto const toX = [&](double time)
		{
			return min.x + static_cast<float>(time / length) * width;
		};
		if (shown.Components == 0)
		{
			float const radius = std::min(height * 0.3f, ImGui::GetFontSize() * 0.3f);
			for (VisualScriptTimelineKey const& key : shown.Keys)
			{
				uint32_t const color = GetVisualScriptTrackColor(shown, 0, library);
				DrawDiamond(drawList, glm::vec2(toX(key.Time), (min.y + max.y) * 0.5f), radius, color, color);
			}
			return;
		}
		// The values over the length, framed to the rectangle.
		int const samples = std::max(static_cast<int>(width / VisualScriptCurveEditor::CurveStep), 2);
		std::vector<glm::vec4> values(static_cast<size_t>(samples) + 1);
		float low = FLT_MAX;
		float high = -FLT_MAX;
		for (int sample = 0; sample <= samples; sample++)
		{
			glm::vec4 const value = EvaluateVisualScriptTimelineTrack(shown, length * sample / samples);
			values[static_cast<size_t>(sample)] = value;
			for (uint32_t component = 0; component < shown.Components; component++)
			{
				low = std::min(low, value[static_cast<glm::length_t>(component)]);
				high = std::max(high, value[static_cast<glm::length_t>(component)]);
			}
		}
		float const span = high - low > 1e-6f ? high - low : 1.0f;
		float const middle = (high + low) * 0.5f;
		std::vector<ImVec2> points(values.size());
		drawList->PushClipRect(ToImVec(min), ToImVec(max), true);
		for (uint32_t component = 0; component < shown.Components; component++)
		{
			for (size_t sample = 0; sample < values.size(); sample++)
			{
				float const value = values[sample][static_cast<glm::length_t>(component)];
				float const y = high - low > 1e-6f ? max.y - (value - low) / span * height : (min.y + max.y) * 0.5f - (value - middle);
				points[sample] = ImVec2(min.x + width * static_cast<float>(sample) / static_cast<float>(samples), y);
			}
			drawList->AddPolyline(points.data(), static_cast<int>(points.size()), GetVisualScriptTrackColor(shown, component, library),
			                      1.0f);
		}
		drawList->PopClipRect();
	}

	void VisualScriptCurveEditor::Open(uint32_t node, std::optional<uint32_t> track)
	{
		if (node != m_Node)
		{
			m_View = {};
			m_Node = node;
			m_Renaming.reset();
			m_HeaderStart.reset();
			m_DragStart.reset();
			m_Actions.clear();
		}
		m_FocusedTrack = track;
		m_FrameTrack = track.has_value();
	}

	void VisualScriptCurveEditor::CancelGesture()
	{
		m_CancelGesture = true;
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptCurveEditor::FindRect(std::string_view part) const
	{
		auto const found = std::find_if(m_Rects.begin(), m_Rects.end(),
		                                [part](auto const& rect)
		                                {
											return rect.first == part;
										});
		return found != m_Rects.end() ? std::optional(found->second) : std::nullopt;
	}

	void VisualScriptCurveEditor::AddRect(std::string label)
	{
		m_Rects.emplace_back(std::move(label), std::pair(ToVec(ImGui::GetItemRectMin()), ToVec(ImGui::GetItemRectMax())));
	}

	void VisualScriptCurveEditor::Commit(Frame& frame, Result<VisualScriptTimelineEdit> edit, uint64_t mergeKey)
	{
		if (!edit)
		{
			frame.Result.Notes.push_back(edit.GetError());
			return;
		}
		VisualScriptTimelineEdit& made = edit.GetValue();
		frame.Commits.Commit(frame.Context, std::move(made.Document), std::move(made.Action), mergeKey, frame.Result);
	}

	uint64_t VisualScriptCurveEditor::GetNudgeKey(double time)
	{
		// As the canvas's nudges: a run of presses less than VisualScriptNudgeRunSeconds apart is one step.
		if (m_NudgeKey == 0 || time - m_LastNudge >= VisualScriptNudgeRunSeconds)
		{
			m_NudgeKey = VisualScriptTimelineNudgeMergeKey + ++m_NudgeRuns;
		}
		m_LastNudge = time;
		return m_NudgeKey;
	}

	void VisualScriptCurveEditor::Draw(VisualScriptPaneContext& context, VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result,
	                                   uint32_t node)
	{
		m_Rects.clear();
		m_Record = {};
		if (node != m_Node)
		{
			Open(node, std::nullopt);
		}
		VisualScriptEditorModel& model = context.Model;
		Ref<VisualScriptDocument const> const hold = commits.GetPendingDocument(model);
		VisualScriptDocument const& document = commits.GetDocument(model);
		VisualScriptGraph const* const shown = FindVisualScriptGraph(document, model.GetFunction());
		VisualScriptNode const* stored = nullptr;
		if (shown != nullptr)
		{
			auto const found = std::find_if(shown->Nodes.begin(), shown->Nodes.end(),
			                                [node](VisualScriptNode const& candidate)
			                                {
												return candidate.ID == node;
											});
			stored = found != shown->Nodes.end() ? &*found : nullptr;
		}
		if (stored == nullptr || stored->Type != VisualScriptTimelineNodeType)
		{
			ImGui::TextDisabled("The sub-graph shown has no Timeline node %u.", node);
			return;
		}
		Result<VisualScriptTimeline> read = ReadVisualScriptTimeline(*stored);
		if (!read)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("This timeline cannot be shown: %s", read.GetError().c_str());
			ImGui::PopTextWrapPos();
			return;
		}
		Ref<VisualScriptLibrary const> library = context.Semantics.GetLibrary();
		VisualScriptLibrary const* const libraryPointer = library.get();
		Frame frame{context,
		            commits,
		            result,
		            hold,
		            document,
		            std::move(library),
		            libraryPointer,
		            node,
		            read.TakeValue(),
		            GetVisualScriptNodeTitle(document, model.GetAnalysis(), *stored),
		            model.IsReadOnly(),
		            stored};
		// The highlighted track and the one being renamed follow the track list as the view's selection does.
		m_View.Sync(frame.Timeline);
		if (m_FocusedTrack)
		{
			m_FocusedTrack = m_View.FollowTrack(*m_FocusedTrack);
		}
		if (m_Renaming)
		{
			m_Renaming = m_View.FollowTrack(*m_Renaming);
		}

		float const fontSize = ImGui::GetFontSize();
		ImVec2 const available = ImGui::GetContentRegionAvail();
		float const listWidth = std::min(TrackListWidth * fontSize, std::max(available.x * 0.5f, 1.0f));
		// The drawer owns the arrows while it has the keyboard (they nudge keys), so neither part navigates with them.
		ImGui::BeginChild("##timelineTracks", ImVec2(listWidth, std::max(available.y, 1.0f)), ImGuiChildFlags_Borders,
		                  ImGuiWindowFlags_NoNavInputs);
		DrawTracks(frame);
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##timelineCurves", ImVec2(0.0f, std::max(available.y, 1.0f)), ImGuiChildFlags_None,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
		DrawHeader(frame);
		if (frame.Timeline.Tracks.empty())
		{
			DrawEmpty(frame);
		}
		else
		{
			DrawCurves(frame);
		}
		DrawKeyMenu(frame);
		ImGui::EndChild();
		ApplyActions(frame);
	}

	void VisualScriptCurveEditor::DrawTracks(Frame& frame)
	{
		VisualScriptTimeline const& timeline = frame.Timeline;
		ImGui::BeginDisabled(frame.ReadOnly || timeline.Tracks.size() >= MaxVisualScriptTimelineTracks);
		if (ImGui::SmallButton("+ Add Track"))
		{
			ImGui::OpenPopup("##addTrack");
		}
		ImGui::EndDisabled();
		AddRect("Add Track");
		if (ImGui::BeginPopup("##addTrack"))
		{
			for (VisualScriptTimelineTrackKind const& kind : VisualScriptTimelineTrackKinds)
			{
				if (ImGui::MenuItem(std::string(kind.Label).c_str()))
				{
					Commit(frame, AddVisualScriptTimelineTrackEdit(frame.Document, frame.Node, timeline, kind.Type, frame.Title));
				}
			}
			ImGui::EndPopup();
		}
		ImGui::Separator();

		float const square = ImGui::GetTextLineHeight() * 0.7f;
		for (uint32_t index = 0; index < timeline.Tracks.size(); index++)
		{
			VisualScriptTimelineTrack const& track = timeline.Tracks[index];
			ImGui::PushID(static_cast<int>(index));
			std::string const prefix = fmt::format("Track {}", index);
			VisualScriptCurveTrackView const view = m_View.GetTrackView(track.Name);

			// The type colour, the eye and the name (or its field while it is renamed).
			ImVec2 const start = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(square, ImGui::GetFrameHeight()));
			ImGui::GetWindowDrawList()->AddRectFilled(
				ImVec2(start.x, start.y + (ImGui::GetFrameHeight() - square) * 0.5f),
				ImVec2(start.x + square, start.y + (ImGui::GetFrameHeight() + square) * 0.5f),
				GetVisualScriptTypeColor(track.Components == 0 ? std::string(VisualScriptExecType) : track.Type, frame.Library));
			ImGui::SameLine();
			bool visible = view.Visible;
			if (ImGui::Checkbox("##eye", &visible))
			{
				m_View.SetTrackVisible(track.Name, visible);
			}
			AddRect(prefix + " Eye");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(visible ? "Shown in the curve view: click to hide" : "Hidden in the curve view: click to show");
			}
			ImGui::SameLine();
			if (m_Renaming == index)
			{
				ImGui::SetNextItemWidth(-FLT_MIN);
				VisualScriptNameField::Outcome const outcome = m_Names.Draw(
					"##name", track.Name,
					[&timeline, index](std::string_view name) -> std::optional<std::string>
					{
						return CheckVisualScriptTimelineTrackName(timeline, name, index);
					},
					std::exchange(m_RenameFocus, false));
				AddRect(prefix + " Name");
				if (outcome.Committed)
				{
					m_Renaming.reset();
					Commit(frame, RenameVisualScriptTimelineTrackEdit(frame.Document, frame.Node, timeline, index, *outcome.Committed,
					                                                  frame.Title));
				}
				else if (outcome.Refused)
				{
					frame.Result.Notes.push_back(*outcome.Refused);
				}
				if (!outcome.Active && !m_Names.IsEditing() && !outcome.Committed)
				{
					m_Renaming.reset();
				}
			}
			else
			{
				std::string const label = fmt::format("{} ({})", track.Name, track.Keys.size());
				if (ImGui::Selectable(label.c_str(), m_FocusedTrack == index, ImGuiSelectableFlags_AllowDoubleClick))
				{
					m_FocusedTrack = index;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !frame.ReadOnly)
					{
						m_Renaming = index;
						m_RenameFocus = true;
					}
				}
				AddRect(prefix);
				if (ImGui::BeginPopupContextItem("##trackMenu"))
				{
					DrawTrackMenu(frame, index);
					ImGui::EndPopup();
				}
			}

			// The components of vectors and colours.
			if (track.Components > 1)
			{
				ImGui::Indent(square + ImGui::GetStyle().ItemSpacing.x);
				for (uint32_t component = 0; component < track.Components; component++)
				{
					if (component > 0)
					{
						ImGui::SameLine();
					}
					bool const on = view.Components[component];
					ImGui::PushStyleColor(ImGuiCol_Text, on ? GetVisualScriptTrackColor(track, component, frame.Library)
					                                        : ImGui::GetColorU32(ImGuiCol_TextDisabled));
					std::string const letter(1, GetVisualScriptTrackComponentLetter(track, component));
					if (ImGui::SmallButton(fmt::format("{}##component{}", letter, component).c_str()))
					{
						m_View.SetComponentVisible(track.Name, component, !on);
					}
					ImGui::PopStyleColor();
					AddRect(fmt::format("{} Component {}", prefix, component));
				}
				ImGui::Unindent(square + ImGui::GetStyle().ItemSpacing.x);
			}
			ImGui::PopID();
		}
	}

	void VisualScriptCurveEditor::DrawTrackMenu(Frame& frame, uint32_t track)
	{
		VisualScriptTimeline const& timeline = frame.Timeline;
		ImGui::BeginDisabled(frame.ReadOnly);
		if (ImGui::MenuItem("Rename"))
		{
			m_Renaming = track;
			m_RenameFocus = true;
		}
		if (ImGui::BeginMenu("Change Type"))
		{
			for (VisualScriptTimelineTrackKind const& kind : VisualScriptTimelineTrackKinds)
			{
				bool const current = kind.Type == timeline.Tracks[track].Type;
				if (ImGui::MenuItem(std::string(kind.Label).c_str(), nullptr, current, !current))
				{
					Commit(frame,
					       ChangeVisualScriptTimelineTrackTypeEdit(frame.Document, frame.Node, timeline, track, kind.Type, frame.Title));
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Remove"))
		{
			Commit(frame, RemoveVisualScriptTimelineTrackEdit(frame.Document, frame.Node, timeline, track, frame.Title));
		}
		ImGui::EndDisabled();
	}

	void VisualScriptCurveEditor::DrawHeader(Frame& frame)
	{
		VisualScriptTimeline const& timeline = frame.Timeline;
		std::span<VisualScriptTimelineKeyRef const> const selection = m_View.GetSelection();
		bool const any = !selection.empty();
		bool const curves = std::any_of(selection.begin(), selection.end(),
		                                [&timeline](VisualScriptTimelineKeyRef const& key)
		                                {
											return timeline.Tracks[key.first].Components > 0;
										});
		float const fontSize = ImGui::GetFontSize();
		ImGui::BeginDisabled(frame.ReadOnly);

		// The first selected key's time and value.
		ImGui::BeginDisabled(!any);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Time");
		ImGui::SameLine();
		VisualScriptTimelineKeyRef const first = any ? selection.front() : VisualScriptTimelineKeyRef(0, 0);
		float time = any ? timeline.Tracks[first.first].Keys[first.second].Time : 0.0f;
		ImGui::SetNextItemWidth(6.0f * fontSize);
		bool const timeChanged = ImGui::DragFloat("##time", &time, 0.01f, 0.0f, timeline.Length, "%.3f s", ImGuiSliderFlags_AlwaysClamp);
		AddRect("Time");
		if (ImGui::IsItemActivated())
		{
			m_HeaderStart = timeline;
		}
		bool const timeActive = ImGui::IsItemActive();
		if (timeChanged && any && m_HeaderStart && first.first < m_HeaderStart->Tracks.size() &&
		    first.second < m_HeaderStart->Tracks[first.first].Keys.size())
		{
			// From the interaction's start: the selection moves as a block by the total change of the first key's time.
			VisualScriptTimeline moved = *m_HeaderStart;
			std::vector<VisualScriptTimelineKeyRef> const keys(selection.begin(), selection.end());
			float const offset = time - m_HeaderStart->Tracks[first.first].Keys[first.second].Time;
			if (MoveVisualScriptTimelineKeys(moved, keys, offset, glm::vec4(0.0f)))
			{
				if (Result<VisualScriptTimelineEdit> edit = MakeVisualScriptTimelineEdit(
						frame.Document, frame.Node, moved, VisualScriptCurveView::DescribeKeys(*m_HeaderStart, keys, "Move", frame.Title)))
				{
					frame.Commits.Change(frame.Context, std::move(edit.GetValue().Document), std::move(edit.GetValue().Action), timeActive,
					                     frame.Result);
				}
			}
		}
		else if (timeActive)
		{
			frame.Result.WidgetActive = true;
		}
		if (ImGui::IsItemDeactivated())
		{
			m_HeaderStart.reset();
			frame.Commits.End(frame.Context, frame.Result);
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		VisualScriptTimelineTrack const* const valueTrack =
			any && timeline.Tracks[first.first].Components > 0 ? &timeline.Tracks[first.first] : nullptr;
		ImGui::BeginDisabled(valueTrack == nullptr);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Value");
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::PushItemWidth(std::max(14.0f * fontSize, 1.0f));
		std::optional<Json> literal;
		if (valueTrack != nullptr)
		{
			Json const current = MakeVisualScriptTimelineValueLiteral(*valueTrack, valueTrack->Keys[first.second].Value);
			literal = m_Literals.Draw(valueTrack->Type, current, nullptr, frame.Library);
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
		if (literal && valueTrack != nullptr)
		{
			if (std::optional<glm::vec4> const value = ReadVisualScriptTimelineValueLiteral(*valueTrack, *literal))
			{
				VisualScriptTimeline edited = timeline;
				if (SetVisualScriptTimelineKeyValue(edited, first, *value))
				{
					std::array<VisualScriptTimelineKeyRef, 1> const keys = {first};
					if (Result<VisualScriptTimelineEdit> edit =
					        MakeVisualScriptTimelineEdit(frame.Document, frame.Node, edited,
					                                     VisualScriptCurveView::DescribeKeys(timeline, keys, "Set Value of", frame.Title)))
					{
						frame.Commits.Change(frame.Context, std::move(edit.GetValue().Document), std::move(edit.GetValue().Action),
						                     valueActive, frame.Result);
					}
				}
			}
		}
		else if (valueActive)
		{
			frame.Result.WidgetActive = true;
		}
		if (valueDeactivated)
		{
			frame.Commits.End(frame.Context, frame.Result);
		}
		ImGui::EndDisabled();

		// Interpolation, tangents and Frame.
		ImGui::SameLine();
		ImGui::BeginDisabled(!curves);
		for (auto const& [interpolation, name] :
		     {std::pair(VisualScriptKeyInterpolation::Cubic, "Cubic"), std::pair(VisualScriptKeyInterpolation::Linear, "Linear"),
		      std::pair(VisualScriptKeyInterpolation::Constant, "Constant")})
		{
			if (ImGui::SmallButton(name))
			{
				if (std::optional<VisualScriptCurveIntent> intent = m_View.SetInterpolation(timeline, interpolation, frame.Title))
				{
					ApplyIntent(frame, std::move(*intent));
				}
			}
			AddRect(name);
			ImGui::SameLine();
		}
		if (ImGui::SmallButton("Auto"))
		{
			if (std::optional<VisualScriptCurveIntent> intent = m_View.AutoTangents(timeline, frame.Title))
			{
				ApplyIntent(frame, std::move(*intent));
			}
		}
		AddRect("Auto");
		ImGui::SameLine();
		if (ImGui::SmallButton("Flat"))
		{
			if (std::optional<VisualScriptCurveIntent> intent = m_View.FlattenTangents(timeline, frame.Title))
			{
				ApplyIntent(frame, std::move(*intent));
			}
		}
		AddRect("Flat");
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::SmallButton("Frame"))
		{
			m_View.Frame(timeline);
		}
		AddRect("Frame");
	}

	void VisualScriptCurveEditor::DrawEmpty(Frame& frame)
	{
		m_Record.Empty = true;
		ImGui::Spacing();
		ImGui::TextDisabled("No tracks yet");
		ImGui::BeginDisabled(frame.ReadOnly);
		for (VisualScriptTimelineTrackKind const& kind : VisualScriptTimelineTrackKinds)
		{
			std::string const label = fmt::format("Add {}", kind.Label);
			if (ImGui::Button(label.c_str()))
			{
				Commit(frame, AddVisualScriptTimelineTrackEdit(frame.Document, frame.Node, frame.Timeline, kind.Type, frame.Title));
			}
			AddRect(label);
			ImGui::SameLine();
		}
		ImGui::NewLine();
		ImGui::EndDisabled();
	}

	void VisualScriptCurveEditor::DrawCurves(Frame& frame)
	{
		VisualScriptTimeline const& timeline = frame.Timeline;
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
		m_View.Sync(timeline);
		if (m_FrameTrack && m_FocusedTrack)
		{
			// Opened on a track: its keys are selected and framed.
			m_FrameTrack = false;
			std::vector<VisualScriptTimelineKeyRef> keys;
			for (uint32_t key = 0; key < timeline.Tracks[*m_FocusedTrack].Keys.size(); key++)
			{
				keys.emplace_back(*m_FocusedTrack, key);
			}
			m_View.Select(std::move(keys));
			m_View.Frame(timeline);
		}

		if (m_CancelGesture)
		{
			m_CancelGesture = false;
			for (VisualScriptCurveIntent& intent : m_View.CancelGesture())
			{
				ApplyIntent(frame, std::move(intent));
			}
		}
		ImGuiIO const& io = ImGui::GetIO();
		VisualScriptCurveViewInput input;
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
		std::vector<VisualScriptCurveIntent> intents = m_View.Update(input, timeline, frame.Title);
		for (VisualScriptCurveIntent& intent : intents)
		{
			ApplyIntent(frame, std::move(intent));
		}
		VisualScriptCurveGesture const gesture = m_View.GetGesture();
		if (gesture == VisualScriptCurveGesture::MoveKeys || gesture == VisualScriptCurveGesture::Tangent)
		{
			frame.Result.WidgetActive = true;
		}

		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(origin, ToImVec(max), true);
		uint32_t const text = ImGui::GetColorU32(ImGuiCol_Text);
		uint32_t const disabled = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		uint32_t const grid = ImGui::GetColorU32(ImGuiCol_Border);
		uint32_t const selectionColor = ImGui::GetColorU32(ImGuiCol_CheckMark);
		drawList->AddRectFilled(origin, ToImVec(max), ImGui::GetColorU32(ImGuiCol_FrameBg));

		// Before 0 and after the length, shaded.
		float const start = m_View.ToScreenX(0.0);
		float const end = m_View.ToScreenX(timeline.Length);
		uint32_t const outside = WithAlpha(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.6f);
		if (start > min.x)
		{
			drawList->AddRectFilled(origin, ImVec2(start, max.y), outside);
		}
		if (end < max.x)
		{
			drawList->AddRectFilled(ImVec2(end, min.y), ToImVec(max), outside);
		}

		// The ruler: seconds across, a tick at least TickSpacing font sizes apart (so at most one per that many pixels).
		float const rulerBottom = m_View.GetLanesTop();
		drawList->AddRectFilled(origin, ImVec2(max.x, rulerBottom), ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
		glm::dvec2 const times = m_View.GetTimeRange();
		double const tick = NiceStep((times.y - times.x) * TickSpacing * fontSize / static_cast<double>(size.x));
		auto const [firstTick, ticks] = GetRulerLines(times.x, times.y, tick, static_cast<int>(size.x / (TickSpacing * fontSize)) + 2);
		for (int line = 0; line < ticks; line++)
		{
			double const at = (firstTick + line) * tick;
			float const x = m_View.ToScreenX(at);
			drawList->AddLine(ImVec2(x, rulerBottom - fontSize * 0.35f), ImVec2(x, rulerBottom), disabled);
			drawList->AddLine(ImVec2(x, rulerBottom), ImVec2(x, max.y), WithAlpha(grid, 0.5f));
			std::string const label = fmt::format("{:g}", std::abs(at) < tick * 1e-6 ? 0.0 : at);
			drawList->AddText(ImVec2(x + 2.0f, origin.y + 1.0f), disabled, label.c_str());
		}
		drawList->AddLine(ImVec2(end, origin.y), ImVec2(end, max.y), text);

		// Values up the side, a line at least ValueLineSpacing font sizes apart.
		float const curvesTop = m_View.GetCurvesTop();
		float const curvesHeight = std::max(max.y - curvesTop, 1.0f);
		glm::dvec2 const values = m_View.GetValueRange();
		double const step = NiceStep((values.y - values.x) * ValueLineSpacing * fontSize / static_cast<double>(curvesHeight));
		auto const [firstValue, valueLines] =
			GetRulerLines(values.x, values.y, step, static_cast<int>(curvesHeight / (ValueLineSpacing * fontSize)) + 2);
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

		float const radius = VisualScriptCurveView::KeyRadius * fontSize;
		// The event lanes, under the ruler.
		for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
		{
			std::optional<std::pair<float, float>> const lane = m_View.GetLane(track);
			if (!lane)
			{
				continue;
			}
			VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
			drawList->AddRectFilled(ImVec2(min.x, lane->first), ImVec2(max.x, lane->second), WithAlpha(grid, 0.35f));
			drawList->AddText(ImVec2(min.x + 2.0f, lane->first), disabled, owner.Name.c_str());
			uint32_t const color = GetVisualScriptTrackColor(owner, 0, frame.Library);
			for (uint32_t key = 0; key < owner.Keys.size(); key++)
			{
				if (std::optional<glm::vec2> const position = m_View.GetKeyPosition(timeline, track, key, 0))
				{
					bool const selected = m_View.IsSelected(track, key);
					DrawDiamond(drawList, *position, radius, selected ? selectionColor : color, selected ? text : WithAlpha(text, 0.5f));
					m_Record.EventKeys++;
				}
			}
		}

		// The curves, their keys and the tangent handles of the selected cubic keys.
		drawList->PushClipRect(ImVec2(min.x, curvesTop), ToImVec(max), true);
		std::vector<ImVec2> points;
		for (uint32_t track = 0; track < timeline.Tracks.size(); track++)
		{
			VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
			for (uint32_t component = 0; component < owner.Components; component++)
			{
				if (!m_View.IsVisible(timeline, track, component))
				{
					continue;
				}
				uint32_t const color = GetVisualScriptTrackColor(owner, component, frame.Library);
				auto const index = static_cast<glm::length_t>(component);
				points.clear();
				float const from = std::max(min.x, start);
				float const to = std::min(max.x, end);
				for (float x = from; x < to + CurveStep; x += CurveStep)
				{
					float const clamped = std::min(x, to);
					double const at = std::clamp(m_View.ToTime(clamped), 0.0, static_cast<double>(timeline.Length));
					points.emplace_back(clamped, m_View.ToScreenY(EvaluateVisualScriptTimelineTrack(owner, at)[index]));
				}
				if (points.size() >= 2)
				{
					drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, m_FocusedTrack == track ? 2.0f : 1.5f);
					m_Record.Curves++;
				}
				for (uint32_t key = 0; key < owner.Keys.size(); key++)
				{
					std::optional<glm::vec2> const position = m_View.GetKeyPosition(timeline, track, key, component);
					if (!position)
					{
						continue;
					}
					bool const selected = m_View.IsSelected(track, key);
					for (bool const out : {false, true})
					{
						if (std::optional<glm::vec2> const handle = m_View.GetTangentHandle(timeline, track, key, component, out))
						{
							drawList->AddLine(ToImVec(*position), ToImVec(*handle), WithAlpha(text, 0.7f));
							drawList->AddCircleFilled(ToImVec(*handle), radius * 0.6f, selectionColor);
							m_Record.TangentHandles++;
						}
					}
					DrawDiamond(drawList, *position, radius, selected ? selectionColor : color, selected ? text : WithAlpha(text, 0.5f));
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
		drawList->PopClipRect();
	}

	void VisualScriptCurveEditor::DrawKeyMenu(Frame& frame)
	{
		if (std::exchange(m_OpenKeyMenu, false))
		{
			ImGui::OpenPopup("##keyMenu");
		}
		if (!ImGui::BeginPopup("##keyMenu"))
		{
			return;
		}
		VisualScriptTimeline const& timeline = frame.Timeline;
		bool const curves = std::any_of(m_View.GetSelection().begin(), m_View.GetSelection().end(),
		                                [&timeline](VisualScriptTimelineKeyRef const& key)
		                                {
											return key.first < timeline.Tracks.size() && timeline.Tracks[key.first].Components > 0;
										});
		ImGui::BeginDisabled(frame.ReadOnly);
		std::optional<VisualScriptCurveIntent> intent;
		if (ImGui::BeginMenu("Interpolation", curves))
		{
			if (ImGui::MenuItem("Cubic"))
			{
				intent = m_View.SetInterpolation(timeline, VisualScriptKeyInterpolation::Cubic, frame.Title);
			}
			if (ImGui::MenuItem("Linear"))
			{
				intent = m_View.SetInterpolation(timeline, VisualScriptKeyInterpolation::Linear, frame.Title);
			}
			if (ImGui::MenuItem("Constant"))
			{
				intent = m_View.SetInterpolation(timeline, VisualScriptKeyInterpolation::Constant, frame.Title);
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Tangents", curves))
		{
			if (ImGui::MenuItem("Auto"))
			{
				intent = m_View.AutoTangents(timeline, frame.Title);
			}
			if (ImGui::MenuItem("Flat"))
			{
				intent = m_View.FlattenTangents(timeline, frame.Title);
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Delete", "Delete"))
		{
			intent = m_View.DeleteSelection(timeline, frame.Title);
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
		if (intent)
		{
			ApplyIntent(frame, std::move(*intent));
		}
	}

	void VisualScriptCurveEditor::ApplyActions(Frame& frame)
	{
		using Action = VisualScriptEditorAction;
		VisualScriptTimeline const& timeline = frame.Timeline;
		for (Action const action : std::exchange(m_Actions, {}))
		{
			std::optional<VisualScriptCurveIntent> intent;
			switch (action)
			{
				case Action::Delete:
					if (!frame.ReadOnly)
					{
						intent = m_View.DeleteSelection(timeline, frame.Title);
					}
					break;
				case Action::SelectAll:
					m_View.SelectAll(timeline);
					break;
				case Action::NudgeLeft:
				case Action::NudgeRight:
				case Action::NudgeLeftCoarse:
				case Action::NudgeRightCoarse:
					if (!frame.ReadOnly)
					{
						int const steps = action == Action::NudgeLeft || action == Action::NudgeLeftCoarse ? -1 : 1;
						bool const coarse = action == Action::NudgeLeftCoarse || action == Action::NudgeRightCoarse;
						// A run of presses and repeats is one step (s2 §4.4's cadence of arrow-key nudges).
						if (std::optional<VisualScriptCurveIntent> nudged =
						        m_View.Nudge(timeline, coarse ? steps * 10 : steps, frame.Title))
						{
							Commit(frame,
							       MakeVisualScriptTimelineEdit(frame.Document, frame.Node, nudged->Timeline, std::move(nudged->Action)),
							       GetNudgeKey(frame.Context.Time));
						}
					}
					break;
				case Action::FrameSelection:
					m_View.Frame(timeline);
					break;
				default:
					break;
			}
			if (intent)
			{
				ApplyIntent(frame, std::move(*intent));
			}
		}
	}

	void VisualScriptCurveEditor::ApplyIntent(Frame& frame, VisualScriptCurveIntent intent)
	{
		switch (intent.Kind)
		{
			case VisualScriptCurveIntentKind::Edit:
				if (!frame.ReadOnly)
				{
					if (!m_DragStart && frame.Stored != nullptr)
					{
						// The drag's first frame: what Escape puts back (the setting as stored, not its canonical form).
						auto const setting = frame.Stored->Settings.find(std::string(VisualScriptTimelineSetting));
						m_DragStart = DragStart{setting != frame.Stored->Settings.end() ? std::optional<Json>(*setting) : std::nullopt,
						                        intent.Action};
					}
					if (Result<VisualScriptTimelineEdit> edit =
					        MakeVisualScriptTimelineEdit(frame.Document, frame.Node, intent.Timeline, std::move(intent.Action)))
					{
						frame.Commits.Change(frame.Context, std::move(edit.GetValue().Document), std::move(edit.GetValue().Action), true,
						                     frame.Result);
					}
				}
				break;
			case VisualScriptCurveIntentKind::EndEdit:
				m_DragStart.reset();
				frame.Commits.End(frame.Context, frame.Result);
				break;
			case VisualScriptCurveIntentKind::CancelEdit:
				if (std::optional<DragStart> start = std::exchange(m_DragStart, std::nullopt); start && !frame.ReadOnly)
				{
					// The node's setting as the drag found it, committed in the drag's step, which then ends where it started.
					VisualScriptDocument reverted = frame.Document;
					VisualScriptGraph* const graph = FindVisualScriptGraph(reverted, frame.Context.Model.GetFunction());
					auto const node = graph != nullptr ? std::find_if(graph->Nodes.begin(), graph->Nodes.end(),
					                                                  [&frame](VisualScriptNode const& candidate)
					                                                  {
																		  return candidate.ID == frame.Node;
																	  })
					                                   : reverted.Graph.Nodes.end();
					if (graph != nullptr && node != graph->Nodes.end())
					{
						if (start->Setting)
						{
							node->Settings[std::string(VisualScriptTimelineSetting)] = std::move(*start->Setting);
						}
						else
						{
							node->Settings.erase(std::string(VisualScriptTimelineSetting));
						}
						frame.Commits.Change(frame.Context, std::move(reverted), std::move(start->Action), false, frame.Result);
					}
				}
				frame.Commits.End(frame.Context, frame.Result);
				break;
			case VisualScriptCurveIntentKind::Commit:
				if (!frame.ReadOnly)
				{
					Commit(frame, MakeVisualScriptTimelineEdit(frame.Document, frame.Node, intent.Timeline, std::move(intent.Action)));
				}
				break;
			case VisualScriptCurveIntentKind::OpenKeyMenu:
				m_OpenKeyMenu = true;
				break;
		}
	}
}
