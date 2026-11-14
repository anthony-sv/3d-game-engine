#include "Editor/VisualScripting/VisualScriptCurveEditor.h"

#include "Editor/UI/CurveEditorImpl.h"
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
		// Runs of arrow-key nudges are timed as the canvas's are.
		static_assert(CurveEditor<VisualScriptTimeline>::NudgeRunSeconds == VisualScriptNudgeRunSeconds);

		ImVec2 ToImVec(glm::vec2 value)
		{
			return ImVec2(value.x, value.y);
		}

		glm::vec2 ToVec(ImVec2 value)
		{
			return glm::vec2(value.x, value.y);
		}

		std::string DescribeLinks(uint32_t count, std::string_view verb)
		{
			return count == 0 ? std::string() : fmt::format(" ({}{} link{})", verb, count, count == 1 ? "" : "s");
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
				DrawCurveKeyDiamond(drawList, glm::vec2(toX(key.Time), (min.y + max.y) * 0.5f), radius, color, color);
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

	VisualScriptCurveEditor::VisualScriptCurveEditor()
		: m_Curves(VisualScriptTimelineNudgeMergeKey)
	{
	}

	void VisualScriptCurveEditor::QueueAction(VisualScriptEditorAction action)
	{
		using Action = VisualScriptEditorAction;
		switch (action)
		{
			case Action::Delete:
				m_Curves.QueueAction(CurveEditorAction::Delete);
				break;
			case Action::SelectAll:
				m_Curves.QueueAction(CurveEditorAction::SelectAll);
				break;
			case Action::NudgeLeft:
				m_Curves.QueueAction(CurveEditorAction::NudgeLeft);
				break;
			case Action::NudgeRight:
				m_Curves.QueueAction(CurveEditorAction::NudgeRight);
				break;
			case Action::NudgeLeftCoarse:
				m_Curves.QueueAction(CurveEditorAction::NudgeLeftCoarse);
				break;
			case Action::NudgeRightCoarse:
				m_Curves.QueueAction(CurveEditorAction::NudgeRightCoarse);
				break;
			case Action::FrameSelection:
				m_Curves.QueueAction(CurveEditorAction::Frame);
				break;
			default:
				break;
		}
	}

	void VisualScriptCurveEditor::Open(uint32_t node, std::optional<uint32_t> track)
	{
		if (node != m_Node)
		{
			m_Curves.Reset();
			m_Node = node;
			m_Renaming.reset();
			m_DragStart.reset();
		}
		m_FocusedTrack = track;
		m_FrameTrack = track.has_value();
	}

	void VisualScriptCurveEditor::CancelGesture()
	{
		m_Curves.CancelGesture();
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> VisualScriptCurveEditor::FindRect(std::string_view part) const
	{
		auto const found = std::find_if(m_Rects.begin(), m_Rects.end(),
		                                [part](auto const& rect)
		                                {
											return rect.first == part;
										});
		return found != m_Rects.end() ? std::optional(found->second) : m_Curves.FindRect(part);
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

	void VisualScriptCurveEditor::Draw(VisualScriptPaneContext& context, VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result,
	                                   uint32_t node)
	{
		m_Rects.clear();
		m_Curves.BeginFrame();
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
		VisualScriptCurveView& view = m_Curves.GetView();
		view.Sync(frame.Timeline);
		if (m_FocusedTrack)
		{
			m_FocusedTrack = view.FollowTrack(*m_FocusedTrack);
		}
		if (m_Renaming)
		{
			m_Renaming = view.FollowTrack(*m_Renaming);
		}

		VisualScriptTimeline const& timeline = frame.Timeline;
		CurveEditorFrame<VisualScriptTimeline> const curves{
			.Document = timeline,
			.Title = frame.Title,
			.ReadOnly = frame.ReadOnly,
			.Time = context.Time,
			.Color =
				[&timeline, &frame](uint32_t track, uint32_t component)
			{
				return GetVisualScriptTrackColor(timeline.Tracks[track], component, frame.Library);
			},
			.ValueField = [this, &timeline, &frame](uint32_t track, uint32_t key) -> std::optional<glm::vec4>
			{
				VisualScriptTimelineTrack const& owner = timeline.Tracks[track];
				Json const current = MakeVisualScriptTimelineValueLiteral(owner, owner.Keys[key].Value);
				std::optional<Json> const literal = m_Literals.Draw(owner.Type, current, nullptr, frame.Library);
				return literal ? ReadVisualScriptTimelineValueLiteral(owner, *literal) : std::nullopt;
			},
			.Emphasized = m_FocusedTrack,
			.PlayheadColor = VisualScriptPlayColor,
		};
		std::vector<CurveEditorChange<VisualScriptTimeline>> changes;

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
		m_Curves.DrawHeader(curves, changes);
		Apply(frame, changes);
		if (timeline.Tracks.empty())
		{
			DrawEmpty(frame);
		}
		else
		{
			if (m_FrameTrack && m_FocusedTrack)
			{
				// Opened on a track: its keys are selected and framed.
				m_FrameTrack = false;
				std::vector<VisualScriptTimelineKeyRef> keys;
				for (uint32_t key = 0; key < timeline.Tracks[*m_FocusedTrack].Keys.size(); key++)
				{
					keys.emplace_back(*m_FocusedTrack, key);
				}
				view.Select(std::move(keys));
				view.Frame(timeline);
			}
			m_Curves.DrawCurves(curves, changes);
			Apply(frame, changes);
		}
		m_Curves.DrawKeyMenu(curves, changes);
		Apply(frame, changes);
		ImGui::EndChild();
		m_Curves.ApplyActions(curves, changes);
		Apply(frame, changes);
		if (m_Curves.IsInteractionActive())
		{
			result.WidgetActive = true;
		}
	}

	void VisualScriptCurveEditor::DrawTracks(Frame& frame)
	{
		VisualScriptTimeline const& timeline = frame.Timeline;
		VisualScriptCurveView& view = m_Curves.GetView();
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
			VisualScriptCurveTrackView const trackView = view.GetTrackView(track.Name);

			// The type colour, the eye and the name (or its field while it is renamed).
			ImVec2 const start = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(square, ImGui::GetFrameHeight()));
			ImGui::GetWindowDrawList()->AddRectFilled(
				ImVec2(start.x, start.y + (ImGui::GetFrameHeight() - square) * 0.5f),
				ImVec2(start.x + square, start.y + (ImGui::GetFrameHeight() + square) * 0.5f),
				GetVisualScriptTypeColor(track.Components == 0 ? std::string(VisualScriptExecType) : track.Type, frame.Library));
			ImGui::SameLine();
			bool visible = trackView.Visible;
			if (ImGui::Checkbox("##eye", &visible))
			{
				view.SetTrackVisible(track.Name, visible);
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
					bool const on = trackView.Components[component];
					ImGui::PushStyleColor(ImGuiCol_Text, on ? GetVisualScriptTrackColor(track, component, frame.Library)
					                                        : ImGui::GetColorU32(ImGuiCol_TextDisabled));
					std::string const letter(1, GetVisualScriptTrackComponentLetter(track, component));
					if (ImGui::SmallButton(fmt::format("{}##component{}", letter, component).c_str()))
					{
						view.SetComponentVisible(track.Name, component, !on);
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

	void VisualScriptCurveEditor::DrawEmpty(Frame& frame)
	{
		m_Curves.SetEmpty();
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

	void VisualScriptCurveEditor::Apply(Frame& frame, std::vector<CurveEditorChange<VisualScriptTimeline>>& changes)
	{
		for (CurveEditorChange<VisualScriptTimeline>& change : changes)
		{
			switch (change.Kind)
			{
				case CurveEditorChangeKind::Drag:
					if (!frame.ReadOnly)
					{
						if (!m_DragStart && frame.Stored != nullptr)
						{
							// The drag's first frame: what Escape puts back (the setting as stored, not its canonical form).
							auto const setting = frame.Stored->Settings.find(std::string(VisualScriptTimelineSetting));
							m_DragStart = DragStart{setting != frame.Stored->Settings.end() ? std::optional<Json>(*setting) : std::nullopt,
							                        change.Action};
						}
						if (Result<VisualScriptTimelineEdit> edit =
						        MakeVisualScriptTimelineEdit(frame.Document, frame.Node, change.Document, std::move(change.Action)))
						{
							frame.Commits.Change(frame.Context, std::move(edit.GetValue().Document), std::move(edit.GetValue().Action),
							                     true, frame.Result);
						}
					}
					break;
				case CurveEditorChangeKind::EndDrag:
					m_DragStart.reset();
					frame.Commits.End(frame.Context, frame.Result);
					break;
				case CurveEditorChangeKind::CancelDrag:
					if (std::optional<DragStart> start = std::exchange(m_DragStart, std::nullopt); start && !frame.ReadOnly)
					{
						// The node's setting as the drag found it, committed in the drag's step, which then ends where it started.
						VisualScriptDocument reverted = frame.Document;
						VisualScriptGraph* const graph = FindVisualScriptGraph(reverted, frame.Context.Model.GetFunction());
						auto const found = graph != nullptr ? std::find_if(graph->Nodes.begin(), graph->Nodes.end(),
						                                                   [&frame](VisualScriptNode const& candidate)
						                                                   {
																			   return candidate.ID == frame.Node;
																		   })
						                                    : reverted.Graph.Nodes.end();
						if (graph != nullptr && found != graph->Nodes.end())
						{
							if (start->Setting)
							{
								found->Settings[std::string(VisualScriptTimelineSetting)] = std::move(*start->Setting);
							}
							else
							{
								found->Settings.erase(std::string(VisualScriptTimelineSetting));
							}
							frame.Commits.Change(frame.Context, std::move(reverted), std::move(start->Action), false, frame.Result);
						}
					}
					frame.Commits.End(frame.Context, frame.Result);
					break;
				case CurveEditorChangeKind::Change:
					if (!frame.ReadOnly)
					{
						if (Result<VisualScriptTimelineEdit> edit =
						        MakeVisualScriptTimelineEdit(frame.Document, frame.Node, change.Document, std::move(change.Action)))
						{
							frame.Commits.Change(frame.Context, std::move(edit.GetValue().Document), std::move(edit.GetValue().Action),
							                     change.Active, frame.Result);
						}
					}
					break;
				case CurveEditorChangeKind::End:
					frame.Commits.End(frame.Context, frame.Result);
					break;
				case CurveEditorChangeKind::Commit:
					if (!frame.ReadOnly)
					{
						Commit(frame, MakeVisualScriptTimelineEdit(frame.Document, frame.Node, change.Document, std::move(change.Action)),
						       change.MergeKey);
					}
					break;
			}
		}
		changes.clear();
	}

	template class CurveEditor<VisualScriptTimeline>;
}
