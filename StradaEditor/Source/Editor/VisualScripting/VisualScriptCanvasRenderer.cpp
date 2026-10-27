#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"

#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptBrushes.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptHitTest.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Strada
{
	namespace
	{
		namespace Metrics = VisualScriptCanvasMetrics;
		namespace Elements = VisualScriptDrawElements;

		using Clock = std::chrono::steady_clock;

		// The grid's three levels (canvas units) and how far each one's lines are mixed from the canvas toward the text colour.
		constexpr std::array<float, 3> GridCells = {16.0f, 128.0f, 1024.0f};
		constexpr std::array<float, 3> GridMix = {0.045f, 0.09f, 0.13f};
		// A grid level fades in between these cell sizes on the screen (pixels).
		constexpr float GridFadeStart = 6.0f;
		constexpr float GridFadeEnd = 20.0f;

		// Node drawing (canvas units unless named otherwise).
		constexpr float ShadowAlpha = 0.45f;
		constexpr float GlowReach = 8.0f;
		constexpr float GlowAlpha = 0.30f;
		constexpr float HeaderHighlightAlpha = 0.08f;
		constexpr float HatchSpacing = 8.0f;
		constexpr float HatchAlpha = 0.10f;
		constexpr float StripeSpacing = 10.0f;
		constexpr float StripeWidth = 4.0f;
		constexpr float StripeAlpha = 0.50f;
		constexpr float StripAlpha = 0.35f;
		constexpr float StripLabelTextSize = 11.0f;
		constexpr float StripPillPaddingX = 6.0f;
		constexpr float BubbleRounding = 4.0f;
		constexpr float OverviewBubbleSize = 10.0f;
		constexpr float BadgeCountTextSize = 11.0f;
		constexpr float CollapsedDotRadius = 1.5f;
		constexpr float BypassDash = 6.0f;
		constexpr float BypassGap = 4.0f;
		constexpr float BypassAlpha = 0.60f;
		constexpr float CommentRounding = 6.0f;
		constexpr float CommentGripSize = 12.0f;

		// Pins.
		constexpr float HoveredPinScale = 1.3f;
		constexpr float CompatiblePinScale = 1.15f;
		constexpr float SnappedPinScale = 1.4f;
		constexpr float HoveredGlowAlpha = 0.35f;
		constexpr float CompatibleGlowAlpha = 0.25f;
		constexpr float SnappedGlowAlpha = 0.50f;
		constexpr float IncompatibleAlpha = 0.30f;
		constexpr float PinGlowRadius = 2.4f;
		constexpr float SplitMarkerRadius = 1.5f;
		constexpr float ArrayCell = 3.0f;
		constexpr float ArrayGap = 1.0f;
		constexpr float ArrayUnlinkedAlpha = 0.35f;
		constexpr float WildcardHalfDiagonal = 6.0f;
		constexpr float BracketAlpha = 0.60f;
		constexpr float BracketInset = 3.0f;
		constexpr float RowHoverAlpha = 0.06f;
		// The exec pin's pentagon, pointing the way the flow goes, listed clockwise on the screen (y down).
		constexpr std::array<glm::vec2, 5> ExecPentagon = {{{-5.0f, -6.0f}, {1.0f, -6.0f}, {5.0f, 0.0f}, {1.0f, 6.0f}, {-5.0f, 6.0f}}};
		// FieldEditor's axis colours (X, Y, Z, W), which the live widget's cells show as stripes too.
		constexpr std::array<uint32_t, 4> AxisColors = {VisualScriptRgba(179, 51, 51), VisualScriptRgba(64, 143, 64),
		                                                VisualScriptRgba(56, 92, 184), VisualScriptRgba(107, 107, 107)};

		// Wires.
		constexpr float DataWireAlpha = 0.85f;
		constexpr float ExecWireAlpha = 0.92f;
		constexpr float ErrorDash = 8.0f;
		constexpr float ErrorGap = 6.0f;
		constexpr float CutDash = 6.0f;
		constexpr float CutGap = 4.0f;
		constexpr float CutWireAlpha = 0.40f;
		constexpr float GhostAlpha = 0.90f;
		constexpr float HaloAlpha = 0.35f;
		constexpr float PulseMix = 0.5f;
		constexpr float PulseDotRadius = 7.0f;

		// The overlay (pixels × UI scale).
		constexpr float MarqueeFillAlpha = 0.10f;
		constexpr float MarqueeOutlineAlpha = 0.80f;
		constexpr float MinimapBodyAlpha = 0.92f;
		constexpr float MinimapCommentAlpha = 0.50f;
		constexpr float MinimapViewFillAlpha = 0.10f;
		constexpr float MinimapViewOutlineAlpha = 0.90f;
		constexpr float MinimapRounding = 4.0f;
		constexpr float MinimapCell = 2.0f;

		ImVec2 ToIm(glm::vec2 point)
		{
			return ImVec2(point.x, point.y);
		}

		float Smoothstep(float edge0, float edge1, float x)
		{
			float const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		float EaseOutCubic(float t)
		{
			float const u = 1.0f - std::clamp(t, 0.0f, 1.0f);
			return 1.0f - u * u * u;
		}

		// Whole pixels, at least min.
		float WholePixels(float pixels, float lower = 1.0f)
		{
			return std::max(std::round(pixels), lower);
		}

		uint64_t HashText(std::string_view text, uint64_t hash)
		{
			for (char const c : text)
			{
				hash = (hash ^ static_cast<uint8_t>(c)) * 0x100000001B3ull;
			}
			return hash;
		}

		uint64_t HashValue(uint64_t value, uint64_t hash)
		{
			for (int shift = 0; shift < 64; shift += 8)
			{
				hash = (hash ^ ((value >> shift) & 0xFFu)) * 0x100000001B3ull;
			}
			return hash;
		}

		constexpr uint64_t HashSeed = 0xCBF29CE484222325ull;

		// The link of the visual model a document link names (by the names the document stores, or the canvas's).
		std::optional<uint32_t> FindLinkIndex(VisualScriptCanvasModel const& canvas, VisualScriptLink const& link)
		{
			std::optional<uint32_t> const from = canvas.FindNode(link.From.Node);
			if (!from)
			{
				return std::nullopt;
			}
			std::span<VisualScriptLinkVisual const> const links = canvas.GetLinks();
			std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
			std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
			for (uint32_t const index : canvas.GetIncidentLinks(*from))
			{
				VisualScriptLinkVisual const& visual = links[index];
				if (visual.FromNode != *from || nodes[visual.ToNode].ID != link.To.Node)
				{
					continue;
				}
				bool const fromMatches =
					canvas.GetText(visual.FromName) == link.From.Pin || canvas.GetText(pins[visual.FromPin].Name) == link.From.Pin;
				bool const toMatches =
					canvas.GetText(visual.ToName) == link.To.Pin || canvas.GetText(pins[visual.ToPin].Name) == link.To.Pin;
				if (fromMatches && toMatches)
				{
					return index;
				}
			}
			return std::nullopt;
		}

		// The short name of a type in a wire's label: a type ID's last segment ("Strada.Vector3" → "Vector3", "float").
		std::string_view GetShortTypeName(std::string_view type)
		{
			size_t const dot = type.rfind('.');
			return dot != std::string_view::npos && dot + 1 < type.size() ? type.substr(dot + 1) : type;
		}

		// Whether a chip kind has a live widget (the others are edited in Details, or show no value).
		bool IsEditableChip(VisualScriptChipKind kind)
		{
			switch (kind)
			{
				case VisualScriptChipKind::Check:
				case VisualScriptChipKind::Number:
				case VisualScriptChipKind::Text:
				case VisualScriptChipKind::Cells:
				case VisualScriptChipKind::Color:
				case VisualScriptChipKind::Choice:
				case VisualScriptChipKind::Asset:
					return true;
				case VisualScriptChipKind::None:
				case VisualScriptChipKind::Matrix:
				case VisualScriptChipKind::Placeholder:
				case VisualScriptChipKind::Raw:
					return false;
			}
			return false;
		}

		uint32_t CountLines(std::string_view text)
		{
			return text.empty() ? 0u : static_cast<uint32_t>(std::count(text.begin(), text.end(), '\n')) + 1u;
		}

		// The line of a text (lines separated by '\n'); empty past the last.
		std::string_view GetLine(std::string_view text, uint32_t line)
		{
			size_t start = 0;
			for (uint32_t i = 0; i < line; i++)
			{
				size_t const end = text.find('\n', start);
				if (end == std::string_view::npos)
				{
					return {};
				}
				start = end + 1;
			}
			size_t const end = text.find('\n', start);
			return text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
		}

		// The part of a convex polygon inside a rectangle (Sutherland-Hodgman against its four edges): stripes and hatching are
		// clipped analytically, as a clip rectangle per stripe would add draw commands. Writes at most 8 points.
		size_t ClipToRect(std::array<ImVec2, 8>& points, size_t count, ImVec2 lower, ImVec2 upper)
		{
			std::array<ImVec2, 8> buffer;
			auto const clipEdge = [&](auto&& inside, auto&& intersect)
			{
				size_t written = 0;
				for (size_t i = 0; i < count; i++)
				{
					ImVec2 const current = points[i];
					ImVec2 const previous = points[(i + count - 1) % count];
					bool const currentIn = inside(current);
					bool const previousIn = inside(previous);
					if (currentIn)
					{
						if (!previousIn && written < buffer.size())
						{
							buffer[written++] = intersect(previous, current);
						}
						if (written < buffer.size())
						{
							buffer[written++] = current;
						}
					}
					else if (previousIn && written < buffer.size())
					{
						buffer[written++] = intersect(previous, current);
					}
				}
				std::copy_n(buffer.begin(), written, points.begin());
				count = written;
			};
			auto const lerp = [](ImVec2 a, ImVec2 b, float t)
			{
				return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
			};
			clipEdge(
				[&](ImVec2 p)
				{
					return p.x >= lower.x;
				},
				[&](ImVec2 a, ImVec2 b)
				{
					return lerp(a, b, (lower.x - a.x) / (b.x - a.x));
				});
			clipEdge(
				[&](ImVec2 p)
				{
					return p.x <= upper.x;
				},
				[&](ImVec2 a, ImVec2 b)
				{
					return lerp(a, b, (upper.x - a.x) / (b.x - a.x));
				});
			clipEdge(
				[&](ImVec2 p)
				{
					return p.y >= lower.y;
				},
				[&](ImVec2 a, ImVec2 b)
				{
					return lerp(a, b, (lower.y - a.y) / (b.y - a.y));
				});
			clipEdge(
				[&](ImVec2 p)
				{
					return p.y <= upper.y;
				},
				[&](ImVec2 a, ImVec2 b)
				{
					return lerp(a, b, (upper.y - a.y) / (b.y - a.y));
				});
			return count;
		}

		// 45° stripes across a rectangle, each spacing apart and width wide, clipped to it.
		void DrawStripes(ImDrawList* drawList, ImVec2 lower, ImVec2 upper, float spacing, float width, ImU32 color)
		{
			if (!(spacing > 0.0f) || upper.x <= lower.x || upper.y <= lower.y || (color & IM_COL32_A_MASK) == 0)
			{
				return;
			}
			float const height = upper.y - lower.y;
			// Each stripe runs from the bottom edge up and to the right; its left edge starts at x on the bottom edge.
			for (float x = lower.x - height; x < upper.x; x += spacing)
			{
				std::array<ImVec2, 8> points = {ImVec2(x, upper.y), ImVec2(x + height, lower.y), ImVec2(x + height + width, lower.y),
				                                ImVec2(x + width, upper.y)};
				size_t const count = ClipToRect(points, 4, lower, upper);
				if (count >= 3)
				{
					drawList->AddConvexPolyFilled(points.data(), static_cast<int>(count), color);
				}
			}
		}

		// A rectangle clamped to another one; false when nothing of it is left.
		bool ClampRect(ImVec2& lower, ImVec2& upper, VisualScriptRect const& bounds)
		{
			lower.x = std::max(lower.x, bounds.Min.x);
			lower.y = std::max(lower.y, bounds.Min.y);
			upper.x = std::min(upper.x, bounds.Max.x);
			upper.y = std::min(upper.y, bounds.Max.y);
			return upper.x > lower.x && upper.y > lower.y;
		}
	}

	VisualScriptDrawRecordItem const* VisualScriptDrawRecord::Find(VisualScriptDrawItemKind kind, uint32_t element) const
	{
		auto const found = std::find_if(Items.begin(), Items.end(),
		                                [kind, element](VisualScriptDrawRecordItem const& item)
		                                {
											return item.Kind == kind && item.Element == element;
										});
		return found != Items.end() ? &*found : nullptr;
	}

	size_t VisualScriptDrawRecord::Count(VisualScriptDrawItemKind kind) const
	{
		return static_cast<size_t>(std::count_if(Items.begin(), Items.end(),
		                                         [kind](VisualScriptDrawRecordItem const& item)
		                                         {
													 return item.Kind == kind;
												 }));
	}

	VisualScriptLiveChipScope::VisualScriptLiveChipScope(VisualScriptLiveChip const& chip)
	{
		ImGui::PushFont(nullptr, chip.FontSizeBase);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, chip.FramePadding);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, chip.ItemSpacing);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, chip.ItemSpacing);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, chip.FrameRounding);
		ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, chip.GrabMinSize);
		ImGui::SetCursorScreenPos(ToIm(chip.Rect.Min));
	}

	VisualScriptLiveChipScope::~VisualScriptLiveChipScope()
	{
		ImGui::PopStyleVar(5);
		ImGui::PopFont();
	}

	VisualScriptRect DrawVisualScriptValuePill(ImDrawList* drawList, ImFont* font, float fontSize, glm::vec2 anchor,
	                                           VisualScriptRect const& bounds, std::string_view text, VisualScriptThemeAnchors const& theme,
	                                           uint32_t textColor)
	{
		if (font == nullptr)
		{
			font = ImGui::GetFont();
		}
		ImVec2 const textSize = UI::CalcArrowTextSize(font, fontSize, text);
		glm::vec2 const padding(std::round(fontSize * 0.5f), std::round(fontSize * 0.25f));
		glm::vec2 const size = glm::vec2(textSize.x, textSize.y) + padding * 2.0f;
		// Below and right of the anchor, moved inside the bounds where it fits.
		glm::vec2 position = anchor + glm::vec2(std::round(fontSize * 0.6f));
		position.x = std::min(position.x, bounds.Max.x - size.x);
		position.y = std::min(position.y, bounds.Max.y - size.y);
		position = glm::max(position, bounds.Min);
		position = glm::floor(position);
		VisualScriptRect const rect{position, position + size};
		float const rounding = std::min(size.y * 0.5f, fontSize * 0.5f);
		drawList->AddRectFilled(ToIm(rect.Min), ToIm(rect.Max), MultiplyVisualScriptAlpha(theme.Body, 0.97f), rounding);
		drawList->AddRect(ToIm(rect.Min), ToIm(rect.Max), theme.Border, rounding, 1.0f);
		UI::DrawArrowText(drawList, font, fontSize, ToIm(position + padding), textColor, text);
		return rect;
	}

	// What one Draw works with: the model's state for this frame, read once.
	struct VisualScriptCanvasRenderer::Frame
	{
		Frame(VisualScriptEditorModel const& model, VisualScriptCanvasDrawInput const& input)
			: Model(&model),
			  Canvas(model.GetCanvas()),
			  View(model.GetView()),
			  Input(input),
			  Theme(input.Style.Theme),
			  DrawList(input.DrawList),
			  Font(input.Font != nullptr ? input.Font : ImGui::GetFont()),
			  Gesture(model.GetGesture()),
			  Preview(model.GetNodePreview()),
			  Nodes(model.GetCanvas().GetNodes()),
			  Pins(model.GetCanvas().GetPins()),
			  Links(model.GetCanvas().GetLinks()),
			  Comments(model.GetCanvas().GetComments()),
			  NodeSelected(model.GetNodeSelection()),
			  CommentSelected(model.GetCommentSelection()),
			  NodeRaise(model.GetNodeRaise())
		{
			SetView();
		}

		// A node drawn alone (the palette's preview): no editor model, so no gesture, selection, hover or live widget.
		Frame(VisualScriptCanvasModel const& canvas, VisualScriptView const& view, VisualScriptCanvasDrawInput const& input)
			: Canvas(canvas),
			  View(view),
			  Input(input),
			  Theme(input.Style.Theme),
			  DrawList(input.DrawList),
			  Font(input.Font != nullptr ? input.Font : ImGui::GetFont()),
			  Nodes(canvas.GetNodes()),
			  Pins(canvas.GetPins()),
			  Links(canvas.GetLinks()),
			  Comments(canvas.GetComments())
		{
			SetView();
		}

		void SetView()
		{
			Level = View.GetDetailLevel();
			Zoom = View.GetState().Zoom;
			UIScale = View.GetUIScale();
			PixelsPerUnit = View.GetPixelsPerUnit();
			CanvasMin = View.GetCanvasMin();
			CanvasMax = View.GetCanvasMin() + View.GetCanvasSize();
			Visible = View.GetVisibleRect();
			LabelsDrawn = VisualScriptGeometry::ArePinLabelsDrawn(Level, Zoom) && Metrics::IsTextVisible(Metrics::PinLabelTextSize, Zoom);
		}

		// Null for a node drawn alone: only Draw's own phases (culling, hover, live widget, pills, minimap) read it.
		VisualScriptEditorModel const* Model = nullptr;
		VisualScriptCanvasModel const& Canvas;
		VisualScriptView const& View;
		VisualScriptCanvasDrawInput const& Input;
		VisualScriptThemeAnchors const& Theme;
		ImDrawList* DrawList = nullptr;
		ImFont* Font = nullptr;
		VisualScriptGestureView Gesture;
		VisualScriptNodePreview Preview;
		std::span<VisualScriptNodeVisual const> Nodes;
		std::span<VisualScriptPinVisual const> Pins;
		std::span<VisualScriptLinkVisual const> Links;
		std::span<VisualScriptCommentVisual const> Comments;
		std::span<uint8_t const> NodeSelected;
		std::span<uint8_t const> CommentSelected;
		std::span<uint32_t const> NodeRaise;
		VisualScriptDetailLevel Level = VisualScriptDetailLevel::Detail;
		float Zoom = 1.0f;
		float UIScale = 1.0f;
		float PixelsPerUnit = 1.0f;
		glm::vec2 CanvasMin{0.0f};
		glm::vec2 CanvasMax{0.0f};
		VisualScriptRect Visible;
		bool LabelsDrawn = false;
		// What the mouse is over (model indices).
		std::optional<uint32_t> HoveredNode;
		std::optional<uint32_t> HoveredPin;
		std::optional<uint32_t> HoveredComment;
		std::optional<uint32_t> HoveredLink;
		bool HoveredCommentEdge = false;
		// The chip drawn as the live widget.
		std::optional<uint32_t> LiveNode;
		std::optional<uint32_t> LivePin;
		std::optional<uint32_t> SpliceLink;
		uint32_t Order = 0;

		glm::vec2 ToScreen(glm::vec2 point) const { return View.ToScreen(point); }
		ImVec2 ToScreenIm(glm::vec2 point) const { return ToIm(View.ToScreen(point)); }
		float ToPixels(float canvasUnits) const { return canvasUnits * PixelsPerUnit; }
		VisualScriptRect ToScreen(VisualScriptRect const& rect) const { return {ToScreen(rect.Min), ToScreen(rect.Max)}; }
		bool IsNodeSelected(uint32_t index) const { return index < NodeSelected.size() && NodeSelected[index] != 0; }
		bool IsCommentSelected(uint32_t index) const { return index < CommentSelected.size() && CommentSelected[index] != 0; }
		bool IsMarqueed(uint32_t id) const
		{
			return std::binary_search(Gesture.MarqueeElements.begin(), Gesture.MarqueeElements.end(), id);
		}
		bool IsOutlined(uint32_t id) const { return std::binary_search(Input.Outlined.begin(), Input.Outlined.end(), id); }
		VisualScriptBrushes* Brushes() const { return Input.Brushes; }
	};

	VisualScriptCanvasRenderer::VisualScriptCanvasRenderer() = default;
	VisualScriptCanvasRenderer::~VisualScriptCanvasRenderer() = default;

	void VisualScriptCanvasRenderer::Draw(VisualScriptEditorModel const& model, VisualScriptCanvasDrawInput const& input)
	{
		Clock::time_point const start = Clock::now();
		Frame frame(model, input);
		m_Stats = {};
		m_TextSizeCount = 0;
		if (input.Record != nullptr)
		{
			input.Record->Clear();
		}
		if (frame.DrawList == nullptr || frame.View.GetCanvasSize().x <= 0.0f || frame.View.GetCanvasSize().y <= 0.0f)
		{
			return;
		}
		ImDrawList* const drawList = frame.DrawList;
		int const vertices = drawList->VtxBuffer.Size;
		int const indices = drawList->IdxBuffer.Size;
		int const commands = drawList->CmdBuffer.Size;

		auto const phase = [&](double& milliseconds, auto&& work)
		{
			if (!input.MeasurePhases)
			{
				work();
				return;
			}
			Clock::time_point const begin = Clock::now();
			work();
			milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
		};

		TakeEvents(model, frame);
		phase(m_Stats.CullMilliseconds,
		      [&]
		      {
				  Cull(frame);
			  });
		DrawGrid(frame);
		phase(m_Stats.CommentsMilliseconds,
		      [&]
		      {
				  DrawComments(frame);
			  });
		phase(m_Stats.WiresMilliseconds,
		      [&]
		      {
				  DrawWires(frame, false);
				  DrawWires(frame, true);
			  });
		phase(m_Stats.NodesMilliseconds,
		      [&]
		      {
				  DrawNodes(frame, false);
				  DrawNodes(frame, true);
			  });
		phase(m_Stats.OverlayMilliseconds,
		      [&]
		      {
				  DrawOverlay(frame);
			  });
		phase(m_Stats.MinimapMilliseconds,
		      [&]
		      {
				  DrawMinimap(frame);
			  });

		// Clear the per-link marks this frame set, so the next frame starts from none without touching every link.
		for (uint32_t const link : m_MarkedLinks)
		{
			if (link < m_LinkMarks.size())
			{
				m_LinkMarks[link] = 0;
			}
		}
		m_MarkedLinks.clear();

		m_Stats.Vertices = static_cast<uint32_t>(drawList->VtxBuffer.Size - vertices);
		m_Stats.Indices = static_cast<uint32_t>(drawList->IdxBuffer.Size - indices);
		m_Stats.DrawCommands = static_cast<uint32_t>(drawList->CmdBuffer.Size - commands);
		m_Stats.TextSizes = m_TextSizeCount;
		if (input.MeasurePhases)
		{
			m_Stats.TotalMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		}
	}

	void VisualScriptCanvasRenderer::StartGlide(std::span<std::pair<uint32_t, glm::vec2> const> oldPositions, double time)
	{
		m_Glides.clear();
		m_GlideIndices.clear();
		m_GlideRevision = ~0ull;
		for (auto const& [node, position] : oldPositions)
		{
			m_Glides.push_back({node, position});
		}
		m_GlideStart = time;
	}

	void VisualScriptCanvasRenderer::DrawNodePreview(VisualScriptCanvasModel const& canvas, uint32_t node, VisualScriptView const& view,
	                                                 VisualScriptCanvasDrawInput const& input)
	{
		Frame frame(canvas, view, input);
		m_Stats = {};
		m_TextSizeCount = 0;
		if (input.Record != nullptr)
		{
			input.Record->Clear();
		}
		if (frame.DrawList == nullptr || node >= frame.Nodes.size())
		{
			return;
		}
		ImDrawList* const drawList = frame.DrawList;
		int const vertices = drawList->VtxBuffer.Size;
		int const indices = drawList->IdxBuffer.Size;
		int const commands = drawList->CmdBuffer.Size;
		DrawNode(frame, node, false);
		m_Stats.VisibleNodes = 1;
		m_Stats.Vertices = static_cast<uint32_t>(drawList->VtxBuffer.Size - vertices);
		m_Stats.Indices = static_cast<uint32_t>(drawList->IdxBuffer.Size - indices);
		m_Stats.DrawCommands = static_cast<uint32_t>(drawList->CmdBuffer.Size - commands);
		m_Stats.TextSizes = m_TextSizeCount;
	}

	void VisualScriptCanvasRenderer::StopAnimations()
	{
		m_Pulses.clear();
		m_Flashes.clear();
		m_Glides.clear();
		m_GlideIndices.clear();
		m_ActivePulses.clear();
	}

	size_t VisualScriptCanvasRenderer::GetCapacityBytes() const
	{
		auto const bytes = [](auto const& vector)
		{
			return vector.capacity() * sizeof(typename std::decay_t<decltype(vector)>::value_type);
		};
		return bytes(m_Query) + bytes(m_VisibleNodes) + bytes(m_NodeOrder) + bytes(m_VisibleComments) + bytes(m_VisibleLinks) +
		       bytes(m_HighlightedLinks) + bytes(m_LinkMarks) + bytes(m_MarkedLinks) + bytes(m_NodeStamps) + bytes(m_Polyline) +
		       bytes(m_DashPolyline) + bytes(m_Lengths) + bytes(m_ActivePulses) + bytes(m_Pulses) + bytes(m_Flashes) + bytes(m_Glides) +
		       bytes(m_GlideIndices) + bytes(m_MinimapCells) + m_Text.capacity() + m_PillText.capacity();
	}

	void VisualScriptCanvasRenderer::TakeEvents(VisualScriptEditorModel const& model, Frame& frame)
	{
		double const time = frame.Input.Time;
		// The model reports a commit's links and an outside change's elements for the frame of its Sync: a second Draw in the
		// same frame must not start them twice.
		if (time != m_LastEventTime)
		{
			m_LastEventTime = time;
			for (VisualScriptLink const& link : model.CommittedLinks())
			{
				m_Pulses.push_back({link, time});
			}
			for (uint32_t const element : model.ChangedElements())
			{
				m_Flashes.push_back({element, time});
			}
		}
		std::erase_if(m_Pulses,
		              [time](Pulse const& pulse)
		              {
						  return time - pulse.Start > std::max(PulseSeconds, PulseHighlightSeconds) || time < pulse.Start;
					  });
		std::erase_if(m_Flashes,
		              [time](Flash const& flash)
		              {
						  return time - flash.Start > FlashSeconds || time < flash.Start;
					  });
		if (!m_Glides.empty() && (time - m_GlideStart >= GlideSeconds || time < m_GlideStart || frame.Input.ReduceMotion))
		{
			m_Glides.clear();
			m_GlideIndices.clear();
		}
		if (!m_Glides.empty() && m_GlideRevision != model.GetCanvasRevision())
		{
			m_GlideRevision = model.GetCanvasRevision();
			m_GlideIndices.clear();
			for (uint32_t glide = 0; glide < m_Glides.size(); glide++)
			{
				if (std::optional<uint32_t> const index = frame.Canvas.FindNode(m_Glides[glide].Node))
				{
					m_GlideIndices.emplace_back(*index, glide);
				}
			}
			std::sort(m_GlideIndices.begin(), m_GlideIndices.end());
		}

		m_ActivePulses.clear();
		for (Pulse const& pulse : m_Pulses)
		{
			std::optional<uint32_t> const link = FindLinkIndex(frame.Canvas, pulse.Link);
			if (!link)
			{
				continue;
			}
			float const elapsed = static_cast<float>(time - pulse.Start);
			ActivePulse active;
			active.Link = *link;
			active.Progress = elapsed < PulseSeconds ? EaseOutCubic(elapsed / static_cast<float>(PulseSeconds)) : -1.0f;
			if (elapsed < PulseHighlightSeconds)
			{
				// Reduce Motion: a static highlight of the same duration.
				active.Highlight =
					frame.Input.ReduceMotion ? PulseMix : PulseMix * (1.0f - elapsed / static_cast<float>(PulseHighlightSeconds));
			}
			if (frame.Input.ReduceMotion)
			{
				active.Progress = -1.0f;
			}
			m_ActivePulses.push_back(active);
		}
	}

	glm::vec2 VisualScriptCanvasRenderer::GetNodeOffset(Frame const& frame, uint32_t index) const
	{
		glm::vec2 offset = frame.Preview.GetOffset(index);
		if (!m_GlideIndices.empty() && frame.Model != nullptr)
		{
			auto const found = std::lower_bound(m_GlideIndices.begin(), m_GlideIndices.end(), std::make_pair(index, 0u));
			if (found != m_GlideIndices.end() && found->first == index)
			{
				float const t = EaseOutCubic(static_cast<float>((frame.Input.Time - m_GlideStart) / GlideSeconds));
				offset += (m_Glides[found->second].From - frame.Nodes[index].Position) * (1.0f - t);
			}
		}
		return offset;
	}

	std::array<glm::vec2, 4> VisualScriptCanvasRenderer::GetWirePoints(Frame const& frame, uint32_t link) const
	{
		if (m_GlideIndices.empty())
		{
			return VisualScriptGeometry::GetLinkControlPoints(frame.Canvas, link, frame.Preview);
		}
		// A glide moves each node by its own offset, which the preview cannot express: the wire keeps the facing of its stored
		// shape and takes its ends' drawn anchors.
		std::array<glm::vec2, 4> const stored = VisualScriptGeometry::GetLinkControlPoints(frame.Canvas, link, frame.Preview);
		VisualScriptLinkVisual const& visual = frame.Links[link];
		VisualScriptWireEnds ends;
		ends.FromReversed = stored[1].x < stored[0].x;
		ends.ToReversed = stored[2].x > stored[3].x;
		auto const anchor = [&](uint32_t nodeIndex, uint32_t pinIndex)
		{
			VisualScriptNodeVisual node = frame.Nodes[nodeIndex];
			if (frame.Preview.Expanded == nodeIndex)
			{
				node = VisualScriptGeometry::GetExpandedNode(node);
			}
			return VisualScriptGeometry::GetPinAnchor(node, frame.Pins[pinIndex], GetNodeOffset(frame, nodeIndex));
		};
		return VisualScriptGeometry::GetWireControlPoints(anchor(visual.FromNode, visual.FromPin), anchor(visual.ToNode, visual.ToPin),
		                                                  ends);
	}

	void VisualScriptCanvasRenderer::Cull(Frame& frame)
	{
		VisualScriptCanvasModel const& canvas = frame.Canvas;
		m_Stamp++;
		if (m_NodeStamps.size() != frame.Nodes.size() || m_Stamp == 0)
		{
			// A new node count, or the stamp wrapped around: no node may look found already.
			m_Stamp = 1;
			m_NodeStamps.assign(frame.Nodes.size(), 0);
		}
		if (m_LinkMarks.size() != frame.Links.size())
		{
			m_LinkMarks.assign(frame.Links.size(), 0);
		}

		// Badges keep a minimum size and glows reach beyond their nodes: the query looks a little beyond the canvas.
		float const margin = 24.0f * frame.UIScale / frame.PixelsPerUnit;
		VisualScriptRect const query = VisualScriptGeometry::Expand(frame.Visible, margin);
		frame.Model->QueryDrawn(query, VisualScriptSpatialIndex::AllMask, m_Query, &m_Stats.Index);
		m_VisibleNodes.clear();
		m_VisibleComments.clear();
		m_VisibleLinks.clear();
		for (uint32_t const item : m_Query)
		{
			uint32_t const index = VisualScriptSpatialIndex::GetIndex(item);
			switch (VisualScriptSpatialIndex::GetKind(item))
			{
				case VisualScriptSpatialIndex::ItemKind::Node:
					if (index < m_NodeStamps.size() && m_NodeStamps[index] != m_Stamp)
					{
						m_NodeStamps[index] = m_Stamp;
						m_VisibleNodes.push_back(index);
					}
					break;
				case VisualScriptSpatialIndex::ItemKind::Comment:
					m_VisibleComments.push_back(index);
					break;
				case VisualScriptSpatialIndex::ItemKind::Link:
					m_VisibleLinks.push_back(index);
					break;
			}
		}
		// Nodes gliding after an Arrange are drawn away from where the index has them, with their wires.
		for (auto const& [index, glide] : m_GlideIndices)
		{
			(void)glide;
			if (index >= frame.Nodes.size())
			{
				continue;
			}
			VisualScriptRect const bounds = VisualScriptGeometry::Translate(frame.Nodes[index].VisualBounds, GetNodeOffset(frame, index));
			if (!VisualScriptGeometry::Intersects(bounds, query))
			{
				continue;
			}
			if (m_NodeStamps[index] != m_Stamp)
			{
				m_NodeStamps[index] = m_Stamp;
				m_VisibleNodes.push_back(index);
			}
			for (uint32_t const link : canvas.GetIncidentLinks(index))
			{
				m_VisibleLinks.push_back(link);
			}
		}
		std::sort(m_VisibleComments.begin(), m_VisibleComments.end());
		std::sort(m_VisibleLinks.begin(), m_VisibleLinks.end());
		m_VisibleLinks.erase(std::unique(m_VisibleLinks.begin(), m_VisibleLinks.end()), m_VisibleLinks.end());

		m_Stats.VisibleNodes = static_cast<uint32_t>(m_VisibleNodes.size());
		m_Stats.VisibleComments = static_cast<uint32_t>(m_VisibleComments.size());
		m_Stats.VisibleWires = static_cast<uint32_t>(m_VisibleLinks.size());

		// The hover, in model indices.
		VisualScriptHit const& hover = frame.Model->GetHover();
		switch (hover.Kind)
		{
			case VisualScriptHitKind::Pin:
				frame.HoveredNode = canvas.FindNode(hover.Element);
				if (hover.Pin < frame.Pins.size())
				{
					frame.HoveredPin = hover.Pin;
				}
				break;
			case VisualScriptHitKind::Badge:
			case VisualScriptHitKind::NodeHeader:
			case VisualScriptHitKind::Node:
			case VisualScriptHitKind::Reroute:
				frame.HoveredNode = canvas.FindNode(hover.Element);
				break;
			case VisualScriptHitKind::Link:
				if (hover.Link)
				{
					frame.HoveredLink = FindLinkIndex(canvas, *hover.Link);
				}
				break;
			case VisualScriptHitKind::CommentEdge:
				frame.HoveredComment = canvas.FindComment(hover.Element);
				frame.HoveredCommentEdge = true;
				break;
			case VisualScriptHitKind::CommentTitle:
				frame.HoveredComment = canvas.FindComment(hover.Element);
				break;
			case VisualScriptHitKind::None:
			case VisualScriptHitKind::Minimap:
			case VisualScriptHitKind::Background:
				break;
		}

		// Links marked for this frame: cut, picked up, the splice target, pulsing.
		auto const mark = [&](VisualScriptLink const& link, uint8_t bit)
		{
			if (std::optional<uint32_t> const index = FindLinkIndex(canvas, link))
			{
				if (m_LinkMarks[*index] == 0)
				{
					m_MarkedLinks.push_back(*index);
				}
				m_LinkMarks[*index] |= bit;
				return index;
			}
			return std::optional<uint32_t>();
		};
		for (VisualScriptLink const& link : frame.Gesture.CutLinks)
		{
			mark(link, CutMark);
		}
		for (VisualScriptLink const& link : frame.Gesture.PickedLinks)
		{
			mark(link, PickedMark);
		}
		if (frame.Gesture.SpliceTarget != nullptr)
		{
			frame.SpliceLink = mark(*frame.Gesture.SpliceTarget, HighlightMark);
		}
		for (ActivePulse const& pulse : m_ActivePulses)
		{
			if (pulse.Highlight > 0.0f)
			{
				if (m_LinkMarks[pulse.Link] == 0)
				{
					m_MarkedLinks.push_back(pulse.Link);
				}
				m_LinkMarks[pulse.Link] |= HighlightMark;
			}
		}

		// The live widget: the chip whose widget is in use, else the editable chip under the mouse, at Detail only.
		if (frame.Level == VisualScriptDetailLevel::Detail && frame.Input.LiveChips != nullptr && !frame.Model->IsReadOnly())
		{
			auto const accept = [&](uint32_t nodeIndex, uint32_t pinIndex)
			{
				VisualScriptPinVisual const& pin = frame.Pins[pinIndex];
				if (pin.Direction != VisualScriptPinDirection::In || pin.HasFlag(VisualScriptPinFlags::Linked) ||
				    !IsEditableChip(pin.ChipKind) || VisualScriptGeometry::IsEmpty(pin.ChipRect))
				{
					return false;
				}
				frame.LiveNode = nodeIndex;
				frame.LivePin = pinIndex;
				return true;
			};
			if (frame.Input.ActiveChip != nullptr)
			{
				if (std::optional<uint32_t> const node = canvas.FindNode(frame.Input.ActiveChip->Node))
				{
					if (std::optional<uint32_t> const pin =
					        canvas.FindPin(*node, frame.Input.ActiveChip->Pin, VisualScriptPinDirection::In))
					{
						accept(*node, *pin);
					}
				}
			}
			else if (frame.Input.Hovered && frame.Gesture.Kind == VisualScriptGesture::Idle && frame.HoveredNode)
			{
				uint32_t const nodeIndex = *frame.HoveredNode;
				VisualScriptNodeVisual const& node = frame.Nodes[nodeIndex];
				if (node.Shape == VisualScriptNodeShape::Standard && !node.HasFlag(VisualScriptNodeFlags::Collapsed))
				{
					glm::vec2 const offset = GetNodeOffset(frame, nodeIndex);
					for (uint32_t pinIndex = node.FirstPin; pinIndex < node.FirstPin + node.PinCount; pinIndex++)
					{
						VisualScriptRect const chip = frame.ToScreen(VisualScriptGeometry::GetChipRect(node, frame.Pins[pinIndex], offset));
						if (VisualScriptGeometry::Contains(chip, frame.Input.Mouse) && accept(nodeIndex, pinIndex))
						{
							break;
						}
					}
				}
			}
		}
	}

	void VisualScriptCanvasRenderer::CountTextSize(float pixels)
	{
		for (uint32_t i = 0; i < m_TextSizeCount && i < m_TextSizes.size(); i++)
		{
			if (m_TextSizes[i] == pixels)
			{
				return;
			}
		}
		// Counted past the array's end too, so a test sees every distinct size even when they are more than it stores.
		if (m_TextSizeCount < m_TextSizes.size())
		{
			m_TextSizes[m_TextSizeCount] = pixels;
		}
		m_TextSizeCount++;
	}

	void VisualScriptCanvasRenderer::DrawText(Frame& frame, float pixels, glm::vec2 position, uint32_t color, std::string_view text,
	                                          bool ellipsis)
	{
		if (!(pixels > 0.0f) || (text.empty() && !ellipsis) || (color & IM_COL32_A_MASK) == 0)
		{
			return;
		}
		CountTextSize(pixels);
		if (ellipsis)
		{
			m_Text.assign(text);
			m_Text.append(VisualScriptEllipsis);
			text = m_Text;
		}
		frame.DrawList->AddText(frame.Font, pixels, ToIm(position), color, text.data(), text.data() + text.size());
	}

	void VisualScriptCanvasRenderer::DrawGrid(Frame& frame)
	{
		ImDrawList* const drawList = frame.DrawList;
		drawList->AddRectFilled(ToIm(frame.CanvasMin), ToIm(frame.CanvasMax), frame.Theme.Canvas);
		float const thickness = std::max(1.0f, std::round(frame.UIScale));
		for (size_t level = 0; level < GridCells.size(); level++)
		{
			float const cell = GridCells[level];
			float const alpha = Smoothstep(GridFadeStart, GridFadeEnd, frame.ToPixels(cell));
			if (alpha <= 0.0f)
			{
				continue;
			}
			uint32_t const color =
				MultiplyVisualScriptAlpha(MixVisualScriptColors(frame.Theme.Canvas, frame.Theme.Text, GridMix[level]), alpha);
			float const firstX = std::ceil(frame.Visible.Min.x / cell) * cell;
			float const firstY = std::ceil(frame.Visible.Min.y / cell) * cell;
			int const columns = std::max(0, static_cast<int>(std::floor((frame.Visible.Max.x - firstX) / cell)) + 1);
			int const rows = std::max(0, static_cast<int>(std::floor((frame.Visible.Max.y - firstY) / cell)) + 1);
			int const lines = columns + rows;
			if (lines == 0)
			{
				continue;
			}
			// One reservation per level: every line is an axis-aligned rectangle snapped to whole pixels.
			drawList->PrimReserve(lines * 6, lines * 4);
			for (int column = 0; column < columns; column++)
			{
				float const x = std::floor(frame.ToScreen(glm::vec2(firstX + static_cast<float>(column) * cell, 0.0f)).x);
				drawList->PrimRect(ImVec2(x, frame.CanvasMin.y), ImVec2(x + thickness, frame.CanvasMax.y), color);
			}
			for (int row = 0; row < rows; row++)
			{
				float const y = std::floor(frame.ToScreen(glm::vec2(0.0f, firstY + static_cast<float>(row) * cell)).y);
				drawList->PrimRect(ImVec2(frame.CanvasMin.x, y), ImVec2(frame.CanvasMax.x, y + thickness), color);
			}
		}
		if (frame.Input.Record != nullptr)
		{
			VisualScriptDrawRecordItem item;
			item.Layer = VisualScriptDrawLayer::Canvas;
			item.Kind = VisualScriptDrawItemKind::Grid;
			item.Level = frame.Level;
			item.Order = frame.Order++;
			item.Screen = {frame.CanvasMin, frame.CanvasMax};
			item.Color = frame.Theme.Canvas;
			frame.Input.Record->Items.push_back(std::move(item));
		}
	}

	void VisualScriptCanvasRenderer::DrawComments(Frame& frame)
	{
		for (uint32_t const index : m_VisibleComments)
		{
			DrawComment(frame, index);
		}
	}

	void VisualScriptCanvasRenderer::DrawComment(Frame& frame, uint32_t index)
	{
		ImDrawList* const drawList = frame.DrawList;
		VisualScriptCommentVisual comment = frame.Comments[index];
		// Drawn where the gesture has it: moved with the selection, or resized.
		if (frame.Gesture.ResizedComment == comment.ID)
		{
			comment.Rect = frame.Gesture.ResizedRect;
		}
		else if (std::find(frame.Gesture.MovedElements.begin(), frame.Gesture.MovedElements.end(), comment.ID) !=
		         frame.Gesture.MovedElements.end())
		{
			comment.Rect = VisualScriptGeometry::Translate(comment.Rect, frame.Gesture.MoveOffset);
		}
		VisualScriptRect const screen = frame.ToScreen(comment.Rect);
		VisualScriptDrawRecordItem item;
		item.Layer = VisualScriptDrawLayer::Comments;
		item.Kind = VisualScriptDrawItemKind::Comment;
		item.Element = comment.ID;
		item.Level = frame.Level;
		item.Screen = screen;
		item.Color = comment.BodyColor;
		item.VertexBegin = static_cast<uint32_t>(drawList->VtxBuffer.Size);

		float const k = frame.PixelsPerUnit;
		float const s = frame.UIScale;
		float const rounding = CommentRounding * k;
		drawList->AddRectFilled(ToIm(screen.Min), ToIm(screen.Max), comment.BodyColor, rounding);
		item.Elements |= Elements::Body;
		VisualScriptRect const bar = frame.ToScreen(VisualScriptGeometry::GetCommentBarRect(comment, k, s));
		// The bar is GetCommentBarRect's height on the screen; it starts at the comment's top.
		VisualScriptRect const barScreen{screen.Min, glm::vec2(screen.Max.x, screen.Min.y + (bar.Max.y - bar.Min.y))};
		bool const fullBar = barScreen.Max.y >= screen.Max.y - 0.5f;
		drawList->AddRectFilled(ToIm(barScreen.Min), ToIm(barScreen.Max), comment.BarColor, rounding,
		                        fullBar ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
		item.Elements |= Elements::Header;

		// The title: at a readable floor, ellipsized again when drawn larger than its layout, and left out when it does not fit.
		float const titlePixels = VisualScriptGeometry::GetCommentTitlePixels(k, s);
		float const inset = std::max(Metrics::HorizontalPadding * k, 4.0f * s);
		float const available = (screen.Max.x - screen.Min.x) - 2.0f * inset;
		if (titlePixels > 0.0f && barScreen.Max.y - barScreen.Min.y >= titlePixels && available > 0.0f && comment.Title.Length > 0)
		{
			std::string_view const title = frame.Canvas.GetText(comment.Title);
			float const layoutWidth = comment.Title.Width * titlePixels / Metrics::CommentTitleTextSize;
			glm::vec2 const position(screen.Min.x + inset, std::floor((barScreen.Min.y + barScreen.Max.y - titlePixels) * 0.5f));
			if (layoutWidth <= available)
			{
				DrawText(frame, titlePixels, position, comment.BarTextColor, frame.Canvas.GetVisibleText(comment.Title),
				         comment.Title.IsEllipsized());
				item.Elements |= Elements::Title;
			}
			else
			{
				float const ellipsisWidth = frame.Font
				                                ->CalcTextSizeA(titlePixels, FLT_MAX, 0.0f, VisualScriptEllipsis.data(),
				                                                VisualScriptEllipsis.data() + VisualScriptEllipsis.size())
				                                .x;
				char const* remaining = nullptr;
				frame.Font->CalcTextSizeA(titlePixels, available - ellipsisWidth, 0.0f, title.data(), title.data() + title.size(),
				                          &remaining);
				size_t const visible = remaining != nullptr ? static_cast<size_t>(remaining - title.data()) : 0;
				if (visible > 0)
				{
					DrawText(frame, titlePixels, position, comment.BarTextColor, title.substr(0, visible), true);
					item.Elements |= Elements::Title;
				}
			}
		}
		// The body text, at its layout's size, line by line while the lines fit the box.
		if (Metrics::IsTextVisible(Metrics::CommentBodyTextSize, frame.Zoom) && comment.Body.Length > 0)
		{
			float const pixels = Metrics::LadderFloor(Metrics::CommentBodyTextSize * k);
			std::string_view const body = frame.Canvas.GetText(comment.Body);
			float const lineHeight = Metrics::CommentBodyTextSize * k;
			float y = barScreen.Max.y + Metrics::PinRowsTop * k;
			float const x = screen.Min.x + Metrics::CommentBodyInset * k;
			uint32_t const lines = CountLines(body);
			for (uint32_t line = 0; line < lines && y + lineHeight <= screen.Max.y; line++)
			{
				DrawText(frame, pixels, glm::vec2(x, y), frame.Theme.Text, GetLine(body, line));
				y += lineHeight;
			}
			item.Elements |= Elements::BodyText;
		}

		bool const selected = frame.IsCommentSelected(index);
		bool const hovered = frame.HoveredComment == index;
		std::optional<double> const flash = FindFlash(comment.ID);
		float const outline = WholePixels(s);
		if (selected)
		{
			drawList->AddRect(ToIm(screen.Min), ToIm(screen.Max), frame.Theme.Selection, rounding, 2.0f * outline);
			item.Elements |= Elements::Outline;
			item.Color = frame.Theme.Selection;
		}
		else if (frame.IsOutlined(comment.ID))
		{
			// A match of the find bar.
			drawList->AddRect(ToIm(screen.Min), ToIm(screen.Max), VisualScriptWarningColor, rounding, 2.0f * outline);
			item.Elements |= Elements::Outline;
			item.Color = VisualScriptWarningColor;
		}
		else if (frame.IsMarqueed(comment.ID))
		{
			drawList->AddRect(ToIm(screen.Min), ToIm(screen.Max), VisualScriptHoverOutlineColor, rounding, outline);
			item.Elements |= Elements::Outline;
		}
		else
		{
			drawList->AddRect(ToIm(screen.Min), ToIm(screen.Max), comment.BorderColor, rounding, outline);
			item.Elements |= Elements::Border;
		}
		if (flash)
		{
			float const alpha = frame.Input.ReduceMotion ? 1.0f : 1.0f - static_cast<float>((frame.Input.Time - *flash) / FlashSeconds);
			drawList->AddRect(ToIm(screen.Min), ToIm(screen.Max), MultiplyVisualScriptAlpha(frame.Theme.Selection, alpha), rounding,
			                  2.0f * outline);
			item.Elements |= Elements::Flash;
		}
		if (hovered || frame.Gesture.ResizedComment == comment.ID)
		{
			// The resize grip in the bottom-right corner, as ImGui draws a window's.
			float const grip = std::max(CommentGripSize * k, 6.0f * s);
			uint32_t const color = frame.HoveredCommentEdge || frame.Gesture.ResizedComment == comment.ID
			                           ? frame.Input.Style.ResizeGripHovered
			                           : frame.Input.Style.ResizeGrip;
			drawList->AddTriangleFilled(ImVec2(screen.Max.x, screen.Max.y - grip), ImVec2(screen.Max.x, screen.Max.y),
			                            ImVec2(screen.Max.x - grip, screen.Max.y), color);
			item.Elements |= Elements::ResizeHandles;
		}
		if (frame.Input.Record != nullptr)
		{
			item.VertexEnd = static_cast<uint32_t>(drawList->VtxBuffer.Size);
			item.Order = frame.Order++;
			frame.Input.Record->Items.push_back(std::move(item));
		}
	}

	std::optional<double> VisualScriptCanvasRenderer::FindFlash(uint32_t element) const
	{
		std::optional<double> latest;
		for (Flash const& flash : m_Flashes)
		{
			if (flash.Element == element && (!latest || flash.Start > *latest))
			{
				latest = flash.Start;
			}
		}
		return latest;
	}

	std::pair<uint32_t, uint32_t> VisualScriptCanvasRenderer::Stroke(Frame& frame, std::span<glm::vec2 const> points, uint32_t color,
	                                                                 float thickness, float dash, float gap)
	{
		ImDrawList* const drawList = frame.DrawList;
		uint32_t const begin = static_cast<uint32_t>(drawList->VtxBuffer.Size);
		if (points.size() < 2)
		{
			return {begin, begin};
		}
		if (!(dash > 0.0f) || !(gap > 0.0f))
		{
			static_assert(sizeof(glm::vec2) == sizeof(ImVec2));
			drawList->AddPolyline(reinterpret_cast<ImVec2 const*>(points.data()), static_cast<int>(points.size()), color, thickness);
			return {begin, static_cast<uint32_t>(drawList->VtxBuffer.Size)};
		}
		// Dashes cut from the polyline by arc length: each one is its own short polyline.
		bool drawing = true;
		float remaining = dash;
		m_DashPolyline.clear();
		m_DashPolyline.push_back(points[0]);
		for (size_t i = 1; i < points.size(); i++)
		{
			glm::vec2 from = points[i - 1];
			glm::vec2 const to = points[i];
			float length = glm::length(to - from);
			while (length > 0.0f)
			{
				float const step = std::min(length, remaining);
				glm::vec2 const point = from + (to - from) * (step / length);
				if (drawing)
				{
					m_DashPolyline.push_back(point);
				}
				remaining -= step;
				length -= step;
				from = point;
				if (remaining <= 0.0f)
				{
					if (drawing && m_DashPolyline.size() >= 2)
					{
						drawList->AddPolyline(reinterpret_cast<ImVec2 const*>(m_DashPolyline.data()),
						                      static_cast<int>(m_DashPolyline.size()), color, thickness);
					}
					drawing = !drawing;
					remaining = drawing ? dash : gap;
					m_DashPolyline.clear();
					if (drawing)
					{
						m_DashPolyline.push_back(point);
					}
				}
			}
		}
		if (drawing && m_DashPolyline.size() >= 2)
		{
			drawList->AddPolyline(reinterpret_cast<ImVec2 const*>(m_DashPolyline.data()), static_cast<int>(m_DashPolyline.size()), color,
			                      thickness);
		}
		return {begin, static_cast<uint32_t>(drawList->VtxBuffer.Size)};
	}

	bool VisualScriptCanvasRenderer::RecolorAlongLength(Frame& frame, std::pair<uint32_t, uint32_t> vertices,
	                                                    std::span<glm::vec2 const> points, float thickness, uint32_t from, uint32_t to)
	{
		ImDrawList* const drawList = frame.DrawList;
		if (vertices.second <= vertices.first || points.size() < 2)
		{
			return true;
		}
		// The vertices AddPolyline writes per point of an open polyline, chosen as it chooses (imgui_draw.cpp, AddPolyline): 2 on
		// the textured anti-aliased path (whole widths below IM_DRAWLIST_TEX_LINES_WIDTH_MAX with a fringe of 1), else 3 for
		// thin and 4 for thick anti-aliased lines. Without anti-aliasing it writes 4 per segment, which has no per-point layout.
		uint32_t perPoint = 0;
		if ((drawList->Flags & ImDrawListFlags_AntiAliasedLines) != 0)
		{
			float const width = std::max(thickness, 1.0f);
			int const whole = static_cast<int>(width);
			bool const textured = (drawList->Flags & ImDrawListFlags_AntiAliasedLinesUseTex) != 0 &&
			                      whole < IM_DRAWLIST_TEX_LINES_WIDTH_MAX && width - static_cast<float>(whole) <= 0.00001f &&
			                      drawList->_FringeScale == 1.0f;
			perPoint = textured ? 2u : (thickness > drawList->_FringeScale ? 4u : 3u);
		}
		uint32_t const count = vertices.second - vertices.first;
		if (perPoint == 0 || count != static_cast<uint32_t>(points.size()) * perPoint)
		{
			ImGui::ShadeVertsLinearColorGradientKeepAlpha(drawList, static_cast<int>(vertices.first), static_cast<int>(vertices.second),
			                                              ToIm(points.front()), ToIm(points.back()), from, to);
			return false;
		}
		// By arc length: projection on the line between the ends would be wrong for loops, which first move away from their end.
		m_Lengths.resize(points.size());
		m_Lengths[0] = 0.0f;
		for (size_t i = 1; i < points.size(); i++)
		{
			m_Lengths[i] = m_Lengths[i - 1] + glm::length(points[i] - points[i - 1]);
		}
		float const total = std::max(m_Lengths.back(), 1e-6f);
		ImDrawVert* const vertexData = drawList->VtxBuffer.Data + vertices.first;
		for (size_t i = 0; i < points.size(); i++)
		{
			uint32_t const color = MixVisualScriptColors(from, to, m_Lengths[i] / total) & ~IM_COL32_A_MASK;
			for (uint32_t v = 0; v < perPoint; v++)
			{
				ImDrawVert& vertex = vertexData[i * perPoint + v];
				vertex.col = color | (vertex.col & IM_COL32_A_MASK);
			}
		}
		return true;
	}

	void VisualScriptCanvasRenderer::DrawWires(Frame& frame, bool highlighted)
	{
		if (!highlighted)
		{
			m_HighlightedLinks.clear();
			// At Far, a crowded view draws only exec wires and the wires of selected nodes.
			bool const crowded = frame.Level == VisualScriptDetailLevel::Far && m_VisibleLinks.size() > FarWireLimit;
			for (uint32_t const link : m_VisibleLinks)
			{
				VisualScriptLinkVisual const& visual = frame.Links[link];
				uint8_t const marks = m_LinkMarks[link];
				if ((marks & PickedMark) != 0)
				{
					// A link picked up with Ctrl+drag is drawn as its ghost in the overlay.
					continue;
				}
				bool const selectedEnd = frame.IsNodeSelected(visual.FromNode) || frame.IsNodeSelected(visual.ToNode);
				if (crowded && !visual.HasFlag(VisualScriptLinkFlags::Exec) && !selectedEnd)
				{
					m_Stats.SkippedWires++;
					continue;
				}
				bool const hoveredEnd = frame.HoveredNode && (visual.FromNode == *frame.HoveredNode || visual.ToNode == *frame.HoveredNode);
				if (selectedEnd || hoveredEnd || frame.HoveredLink == link || (marks & HighlightMark) != 0)
				{
					m_HighlightedLinks.push_back(link);
					continue;
				}
				DrawWire(frame, link, false);
			}
			return;
		}
		for (uint32_t const link : m_HighlightedLinks)
		{
			DrawWire(frame, link, true);
		}
	}

	void VisualScriptCanvasRenderer::DrawWire(Frame& frame, uint32_t link, bool highlighted)
	{
		VisualScriptLinkVisual const& visual = frame.Links[link];
		std::array<glm::vec2, 4> points = GetWirePoints(frame, link);
		for (glm::vec2& point : points)
		{
			point = frame.ToScreen(point);
		}
		float const s = frame.UIScale;
		if (frame.Level == VisualScriptDetailLevel::Far)
		{
			VisualScriptRect const bounds = VisualScriptGeometry::GetWireBounds(points);
			glm::vec2 const size = bounds.GetSize();
			if (std::max(size.x, size.y) < FarMinWireLength * s)
			{
				m_Stats.SkippedWires++;
				return;
			}
		}
		VisualScriptGeometry::WireFlattening const flattening =
			VisualScriptGeometry::GetWireFlattening(frame.Level, frame.Input.Style.CurveTessellationTolerance);
		VisualScriptGeometry::FlattenWire(points, flattening.Tolerance, flattening.SegmentCap, m_Polyline);

		bool const exec = visual.HasFlag(VisualScriptLinkFlags::Exec);
		float const k = frame.PixelsPerUnit;
		float thickness = exec ? std::clamp(std::round(3.0f * k), 1.0f, std::round(5.0f * s))
		                       : std::clamp(std::round(2.0f * k), 1.0f, std::round(4.0f * s));
		if (highlighted)
		{
			thickness += 1.0f;
		}
		uint8_t const marks = m_LinkMarks[link];
		bool const cut = (marks & CutMark) != 0;
		bool const error = visual.HasFlag(VisualScriptLinkFlags::Error);
		bool const splice = frame.SpliceLink == link;
		float alpha = highlighted ? 1.0f : (exec ? ExecWireAlpha : DataWireAlpha);
		if (visual.HasFlag(VisualScriptLinkFlags::Disabled))
		{
			alpha *= VisualScriptDisabledAlpha;
		}
		uint32_t fromColor = exec ? frame.Theme.Text : visual.FromColor;
		uint32_t toColor = exec ? frame.Theme.Text : visual.ToColor;
		if (error)
		{
			fromColor = VisualScriptErrorColor;
			toColor = VisualScriptErrorColor;
		}
		float pulse = 0.0f;
		for (ActivePulse const& active : m_ActivePulses)
		{
			if (active.Link == link)
			{
				pulse = std::max(pulse, active.Highlight);
			}
		}
		if (pulse > 0.0f)
		{
			fromColor = MixVisualScriptColors(fromColor, VisualScriptRgba(255, 255, 255, (fromColor >> IM_COL32_A_SHIFT) & 0xFFu), pulse);
			toColor = MixVisualScriptColors(toColor, VisualScriptRgba(255, 255, 255, (toColor >> IM_COL32_A_SHIFT) & 0xFFu), pulse);
		}
		if (cut)
		{
			// The cut preview shows on every type colour by shape and fading, not by hue.
			alpha *= CutWireAlpha;
		}

		VisualScriptDrawRecordItem item;
		item.Layer = highlighted ? VisualScriptDrawLayer::HighlightedWires : VisualScriptDrawLayer::Wires;
		item.Kind = VisualScriptDrawItemKind::Wire;
		item.Element = link;
		item.Level = frame.Level;
		item.Screen = VisualScriptGeometry::GetWireBounds(points);
		item.PointCount = static_cast<uint32_t>(m_Polyline.size());

		if (splice)
		{
			// The splice target stands out on every type colour: a wider stroke over a halo in the text colour.
			thickness += 2.0f * WholePixels(s);
			Stroke(frame, m_Polyline, MultiplyVisualScriptAlpha(frame.Theme.Text, HaloAlpha), thickness + 4.0f * WholePixels(s));
			item.Elements |= Elements::Halo;
		}
		uint32_t const strokeColor = MultiplyVisualScriptAlpha(fromColor, alpha);
		std::pair<uint32_t, uint32_t> vertices;
		if (cut)
		{
			vertices = Stroke(frame, m_Polyline, strokeColor, thickness, CutDash * s, CutGap * s);
			item.Elements |= Elements::Dashed | Elements::Faded;
		}
		else if (error)
		{
			vertices = Stroke(frame, m_Polyline, strokeColor, thickness, ErrorDash * s, ErrorGap * s);
			item.Elements |= Elements::Dashed;
		}
		else
		{
			vertices = Stroke(frame, m_Polyline, strokeColor, thickness);
		}
		if (!error && fromColor != toColor)
		{
			bool const exact = RecolorAlongLength(frame, vertices, m_Polyline, thickness, fromColor, toColor);
			item.Elements |= exact ? Elements::Gradient : Elements::GradientFallback;
		}
		if (visual.HasFlag(VisualScriptLinkFlags::Disabled))
		{
			item.Elements |= Elements::Faded;
		}
		if (pulse > 0.0f)
		{
			item.Elements |= Elements::PulseHighlight;
		}
		if (error && frame.Level != VisualScriptDetailLevel::Far)
		{
			// The problem's badge at half the wire's length; hovering the wire shows its message.
			glm::vec2 const middle = VisualScriptGeometry::GetPolylinePoint(m_Polyline, 0.5f);
			float const radius = VisualScriptGeometry::GetBadgeRadius(k, s) * k;
			frame.DrawList->AddCircleFilled(ToIm(middle), radius, VisualScriptErrorColor);
			DrawExclamation(frame, middle, radius, frame.Theme.Text);
			item.Elements |= Elements::ErrorBadge;
		}
		m_Stats.DrawnWires++;
		if (frame.Input.Record != nullptr)
		{
			item.Order = frame.Order++;
			item.Color = strokeColor;
			item.VertexBegin = vertices.first;
			item.VertexEnd = vertices.second;
			item.Thickness = thickness;
			frame.Input.Record->Items.push_back(std::move(item));
		}
	}

	void VisualScriptCanvasRenderer::DrawExclamation(Frame& frame, glm::vec2 center, float radius, uint32_t color)
	{
		// An exclamation mark drawn as shapes, so badges read at any size: a bar and a dot.
		float const width = std::max(radius * 0.28f, 1.0f);
		frame.DrawList->AddRectFilled(ImVec2(center.x - width * 0.5f, center.y - radius * 0.55f),
		                              ImVec2(center.x + width * 0.5f, center.y + radius * 0.15f), color);
		frame.DrawList->AddRectFilled(ImVec2(center.x - width * 0.5f, center.y + radius * 0.32f),
		                              ImVec2(center.x + width * 0.5f, center.y + radius * 0.32f + width), color);
	}

	bool VisualScriptCanvasRenderer::DrawSoftRect(Frame& frame, ImVec2 lower, ImVec2 upper, float reach, uint32_t color)
	{
		if (frame.Brushes() == nullptr || !frame.Brushes()->DrawSoftRect(frame.DrawList, lower, upper, reach, color))
		{
			return false;
		}
		m_Stats.BrushDraws++;
		return true;
	}

	bool VisualScriptCanvasRenderer::DrawSoftDot(Frame& frame, glm::vec2 center, float radius, uint32_t color)
	{
		if (frame.Brushes() == nullptr || !frame.Brushes()->DrawSoftDot(frame.DrawList, ToIm(center), radius, color))
		{
			return false;
		}
		m_Stats.BrushDraws++;
		return true;
	}

	void VisualScriptCanvasRenderer::DrawNodes(Frame& frame, bool selected)
	{
		if (!selected)
		{
			// Drawn in the order hit testing reads backwards, so the node on top is the one the mouse hits.
			m_NodeOrder.clear();
			for (uint32_t const index : m_VisibleNodes)
			{
				uint32_t const raise = index < frame.NodeRaise.size() ? frame.NodeRaise[index] : 0u;
				m_NodeOrder.push_back(GetVisualScriptNodeDrawKey(frame.IsNodeSelected(index), raise, index));
			}
			std::sort(m_NodeOrder.begin(), m_NodeOrder.end());
		}
		for (uint64_t const key : m_NodeOrder)
		{
			bool const isSelected = (key >> 63) != 0;
			if (isSelected == selected)
			{
				DrawNode(frame, static_cast<uint32_t>(key & 0xFFFFFFFFull), selected);
			}
		}
	}

	void VisualScriptCanvasRenderer::DrawNode(Frame& frame, uint32_t index, bool selected)
	{
		VisualScriptNodeVisual node = frame.Nodes[index];
		if (frame.Preview.Expanded == index)
		{
			node = VisualScriptGeometry::GetExpandedNode(node);
		}
		NodeState state;
		state.Index = index;
		state.Offset = GetNodeOffset(frame, index);
		state.Selected = selected;
		state.Hovered = frame.HoveredNode == index || frame.IsMarqueed(node.ID);
		// A node drawn alone flashes with nothing of the canvas.
		state.Flash = frame.Model != nullptr ? FindFlash(node.ID) : std::nullopt;
		state.Outlined = frame.IsOutlined(node.ID);
		state.Fade = node.HasFlag(VisualScriptNodeFlags::Disabled) ? VisualScriptDisabledAlpha : 1.0f;
		VisualScriptRect const rect = VisualScriptGeometry::GetNodeRect(node, state.Offset);
		state.Screen = frame.ToScreen(rect);

		VisualScriptDrawRecordItem item;
		item.Layer = selected ? VisualScriptDrawLayer::SelectedNodes : VisualScriptDrawLayer::Nodes;
		item.Kind = node.Shape == VisualScriptNodeShape::Reroute ? VisualScriptDrawItemKind::Reroute : VisualScriptDrawItemKind::Node;
		item.Element = node.ID;
		item.Level = frame.Level;
		item.Screen = state.Screen;
		item.VertexBegin = static_cast<uint32_t>(frame.DrawList->VtxBuffer.Size);

		if (node.Shape == VisualScriptNodeShape::Reroute)
		{
			DrawReroute(frame, node, state, item);
		}
		else if (frame.Level == VisualScriptDetailLevel::Far || frame.Level == VisualScriptDetailLevel::Overview)
		{
			DrawNodeBox(frame, node, state, item);
		}
		else if (node.Shape == VisualScriptNodeShape::Standard)
		{
			DrawStandardNode(frame, node, state, item);
		}
		else
		{
			DrawCompactShape(frame, node, state, item);
		}
		if (frame.Input.Record != nullptr)
		{
			item.VertexEnd = static_cast<uint32_t>(frame.DrawList->VtxBuffer.Size);
			item.Order = frame.Order++;
			frame.Input.Record->Items.push_back(std::move(item));
		}
	}

	void VisualScriptCanvasRenderer::DrawNodeOutline(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                                 float rounding, VisualScriptDrawRecordItem& item)
	{
		ImDrawList* const drawList = frame.DrawList;
		float const unit = WholePixels(frame.UIScale);
		bool const error = node.ErrorCount > 0;
		uint32_t color = 0;
		float thickness = unit;
		if (state.Selected && state.Hovered)
		{
			color = MixVisualScriptColors(frame.Theme.Selection, frame.Theme.Text, 0.25f);
			thickness = 2.0f * unit;
		}
		else if (state.Selected)
		{
			color = frame.Theme.Selection;
			thickness = 2.0f * unit;
		}
		else if (state.Outlined)
		{
			// A match of the find bar.
			color = VisualScriptWarningColor;
			thickness = 2.0f * unit;
		}
		else if (state.Hovered)
		{
			color = VisualScriptHoverOutlineColor;
		}
		else if (error)
		{
			color = VisualScriptErrorColor;
			thickness = 2.0f * unit;
		}
		if (color != 0)
		{
			// State outlines lie outside the body.
			float const half = thickness * 0.5f;
			drawList->AddRect(ImVec2(state.Screen.Min.x - half, state.Screen.Min.y - half),
			                  ImVec2(state.Screen.Max.x + half, state.Screen.Max.y + half), MultiplyVisualScriptAlpha(color, state.Fade),
			                  rounding + half, thickness);
			item.Elements |= Elements::Outline;
			item.Color = color;
		}
		else if (frame.Level == VisualScriptDetailLevel::Detail || frame.Level == VisualScriptDetailLevel::Compact)
		{
			drawList->AddRect(ToIm(state.Screen.Min), ToIm(state.Screen.Max), MultiplyVisualScriptAlpha(frame.Theme.Border, state.Fade),
			                  rounding, unit);
			item.Elements |= Elements::Border;
		}
		if (state.Flash)
		{
			float const age = static_cast<float>((frame.Input.Time - *state.Flash) / FlashSeconds);
			float const alpha = frame.Input.ReduceMotion ? 1.0f : std::clamp(1.0f - age, 0.0f, 1.0f);
			float const half = unit;
			drawList->AddRect(ImVec2(state.Screen.Min.x - half, state.Screen.Min.y - half),
			                  ImVec2(state.Screen.Max.x + half, state.Screen.Max.y + half),
			                  MultiplyVisualScriptAlpha(frame.Theme.Selection, alpha), rounding + half, 2.0f * unit);
			item.Elements |= Elements::Flash;
		}
	}

	void VisualScriptCanvasRenderer::DrawShadowAndGlow(Frame& frame, NodeState const& state, VisualScriptDrawRecordItem& item)
	{
		float const k = frame.PixelsPerUnit;
		if (frame.Level == VisualScriptDetailLevel::Detail || frame.Level == VisualScriptDetailLevel::Compact)
		{
			float const drop = Metrics::ShadowOffsetY * k;
			if (DrawSoftRect(frame, ImVec2(state.Screen.Min.x, state.Screen.Min.y + drop),
			                 ImVec2(state.Screen.Max.x, state.Screen.Max.y + drop), Metrics::ShadowReach * k,
			                 VisualScriptRgba(0, 0, 0, static_cast<uint32_t>(255.0f * ShadowAlpha * state.Fade + 0.5f))))
			{
				item.Elements |= Elements::Shadow;
			}
		}
		if (state.Flash && frame.Level != VisualScriptDetailLevel::Far)
		{
			// The flash's glow lies under the node, as the selection's does.
			float const age = static_cast<float>((frame.Input.Time - *state.Flash) / FlashSeconds);
			float const alpha = frame.Input.ReduceMotion ? 1.0f : std::clamp(1.0f - age, 0.0f, 1.0f);
			if (DrawSoftRect(frame, ToIm(state.Screen.Min), ToIm(state.Screen.Max), GlowReach * k,
			                 MultiplyVisualScriptAlpha(frame.Theme.Selection, GlowAlpha * alpha)))
			{
				item.Elements |= Elements::Glow;
			}
		}
		if (state.Selected && frame.Level != VisualScriptDetailLevel::Far &&
		    DrawSoftRect(frame, ToIm(state.Screen.Min), ToIm(state.Screen.Max), GlowReach * k,
		                 MultiplyVisualScriptAlpha(frame.Theme.Selection, GlowAlpha)))
		{
			item.Elements |= Elements::Glow;
		}
	}

	void VisualScriptCanvasRenderer::DrawNodeBox(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                             VisualScriptDrawRecordItem& item)
	{
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		float const s = frame.UIScale;
		uint32_t const box = MultiplyVisualScriptAlpha(node.BoxColor, state.Fade);
		if (frame.Level == VisualScriptDetailLevel::Far)
		{
			// A plain rectangle: no rounding, no title, no pins.
			drawList->AddRectFilled(ToIm(state.Screen.Min), ToIm(state.Screen.Max), box);
			item.Elements |= Elements::Body;
			if (state.Selected || state.Hovered || state.Outlined || node.ErrorCount > 0 || state.Flash)
			{
				DrawNodeOutline(frame, node, state, 0.0f, item);
			}
			// Problems stay visible at every zoom: badges keep their minimum size.
			DrawBadges(frame, node, state, item);
			return;
		}
		float const rounding = Metrics::NodeCornerRadius * k;
		DrawShadowAndGlow(frame, state, item);
		drawList->AddRectFilled(ToIm(state.Screen.Min), ToIm(state.Screen.Max), box, rounding);
		item.Elements |= Elements::Body;

		// The title centred, at a readable floor; shortened again to the box, and left out when fewer than a few glyphs fit.
		float const pixels =
			Metrics::LadderFloor(std::max(Metrics::TitleTextSize * frame.Zoom, Metrics::OverviewTitleMinLogicalPixels) * s);
		float const available = (state.Screen.Max.x - state.Screen.Min.x) - 2.0f * std::max(4.0f * s, Metrics::HorizontalPadding * k);
		if (pixels > 0.0f && node.Title.Length > 0 && available > 0.0f && state.Screen.Max.y - state.Screen.Min.y >= pixels)
		{
			std::string_view const title = frame.Canvas.GetText(node.Title);
			float const sizeCu = node.Shape == VisualScriptNodeShape::Operator ? Metrics::OperatorSymbolTextSize : Metrics::TitleTextSize;
			float width = node.Title.Width * pixels / sizeCu;
			std::string_view shown = frame.Canvas.GetVisibleText(node.Title);
			bool ellipsis = node.Title.IsEllipsized();
			if (width > available)
			{
				float const ellipsisWidth = frame.Font
				                                ->CalcTextSizeA(pixels, FLT_MAX, 0.0f, VisualScriptEllipsis.data(),
				                                                VisualScriptEllipsis.data() + VisualScriptEllipsis.size())
				                                .x;
				char const* remaining = nullptr;
				ImVec2 const fitted = frame.Font->CalcTextSizeA(pixels, available - ellipsisWidth, 0.0f, title.data(),
				                                                title.data() + title.size(), &remaining);
				shown = title.substr(0, remaining != nullptr ? static_cast<size_t>(remaining - title.data()) : 0);
				ellipsis = true;
				width = fitted.x + ellipsisWidth;
			}
			size_t glyphs = 0;
			for (char const c : shown)
			{
				glyphs += (static_cast<uint8_t>(c) & 0xC0u) != 0x80u ? 1u : 0u;
			}
			if (glyphs >= static_cast<size_t>(Metrics::OverviewTitleMinGlyphs) || (!ellipsis && glyphs > 0))
			{
				glm::vec2 const center = state.Screen.GetCenter();
				DrawText(frame, pixels, glm::floor(glm::vec2(center.x - width * 0.5f, center.y - pixels * 0.5f)), frame.Theme.Text, shown,
				         ellipsis);
				item.Elements |= Elements::Title;
			}
		}
		DrawPins(frame, node, state, item);
		DrawBadges(frame, node, state, item);
		DrawNodeOutline(frame, node, state, rounding, item);
		if (state.Selected && node.Comment.Length > 0)
		{
			// The node comment's marker: a small speech bubble above the node's top-left corner.
			float const size = std::max(OverviewBubbleSize * k, 8.0f * s);
			ImVec2 const lower(state.Screen.Min.x, state.Screen.Min.y - size * 1.1f);
			ImVec2 const upper(lower.x + size, lower.y + size * 0.7f);
			drawList->AddRectFilled(lower, upper, frame.Theme.Button, size * 0.2f);
			drawList->AddTriangleFilled(ImVec2(lower.x + size * 0.2f, upper.y), ImVec2(lower.x + size * 0.5f, upper.y),
			                            ImVec2(lower.x + size * 0.25f, upper.y + size * 0.3f), frame.Theme.Button);
			item.Elements |= Elements::Bubble;
		}
	}

	void VisualScriptCanvasRenderer::DrawStandardNode(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                                  VisualScriptDrawRecordItem& item)
	{
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		float const s = frame.UIScale;
		float const rounding = Metrics::NodeCornerRadius * k;
		float const unit = WholePixels(s);
		VisualScriptThemeAnchors const& theme = frame.Theme;
		bool const collapsed = node.HasFlag(VisualScriptNodeFlags::Collapsed) && frame.Preview.Expanded != state.Index;

		DrawShadowAndGlow(frame, state, item);
		drawList->AddRectFilled(ToIm(state.Screen.Min), ToIm(state.Screen.Max), MultiplyVisualScriptAlpha(theme.Body, state.Fade),
		                        rounding);
		item.Elements |= Elements::Body;

		VisualScriptRect const header = frame.ToScreen(VisualScriptGeometry::GetHeaderRect(node, state.Offset));
		VisualScriptRect const strip = frame.ToScreen(VisualScriptGeometry::GetEnabledStripRect(node, state.Offset));
		if (node.HasFlag(VisualScriptNodeFlags::UnknownType))
		{
			// A type the library does not know: the body hatched with faint diagonals.
			ImVec2 const lower(state.Screen.Min.x, header.Max.y);
			ImVec2 const upper(state.Screen.Max.x,
			                   VisualScriptGeometry::IsEmpty(strip) ? state.Screen.Max.y - rounding * 0.5f : strip.Min.y);
			DrawStripes(drawList, lower, upper, HatchSpacing * k, std::max(unit, 1.0f),
			            MultiplyVisualScriptAlpha(VisualScriptRgba(255, 255, 255), HatchAlpha * state.Fade));
			item.Elements |= Elements::Hatching;
		}

		// The header: a left-to-right gradient, its top corners rounded (all four when it is the whole node).
		bool const headerIsNode = header.Max.y >= state.Screen.Max.y - 0.5f;
		int const headerStart = drawList->VtxBuffer.Size;
		drawList->AddRectFilled(ToIm(header.Min), ToIm(header.Max), MultiplyVisualScriptAlpha(node.HeaderColor, state.Fade), rounding,
		                        headerIsNode ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
		ImGui::ShadeVertsLinearColorGradientKeepAlpha(drawList, headerStart, drawList->VtxBuffer.Size, ToIm(header.Min),
		                                              ImVec2(header.Max.x, header.Min.y), node.HeaderColor, node.HeaderEndColor);
		drawList->AddRectFilled(ImVec2(header.Min.x + rounding, header.Min.y), ImVec2(header.Max.x - rounding, header.Min.y + unit),
		                        MultiplyVisualScriptAlpha(theme.Text, HeaderHighlightAlpha * state.Fade));
		if (!headerIsNode)
		{
			drawList->AddRectFilled(ImVec2(header.Min.x, header.Max.y - unit), ImVec2(header.Max.x, header.Max.y),
			                        MultiplyVisualScriptAlpha(MixVisualScriptColors(node.HeaderColor, theme.Border, 0.5f), state.Fade));
		}
		item.Elements |= Elements::Header;

		// The kind's glyph, the title and subtitle, the collapsed marker. Text keeps full opacity on disabled nodes.
		float const glyphPixels = Metrics::HeaderGlyphSize * k;
		if (glyphPixels >= 6.0f)
		{
			VisualScriptRect const kind = frame.ToScreen(VisualScriptGeometry::GetKindGlyphRect(node, state.Offset));
			if (!VisualScriptGeometry::IsEmpty(kind))
			{
				glm::vec2 const center = kind.GetCenter();
				float const half = (kind.Max.x - kind.Min.x) * 0.5f;
				if (node.Kind == VisualScriptNodeKind::Event)
				{
					std::array<ImVec2, 4> const diamond = {ImVec2(center.x, center.y - half), ImVec2(center.x + half, center.y),
					                                       ImVec2(center.x, center.y + half), ImVec2(center.x - half, center.y)};
					drawList->AddConvexPolyFilled(diamond.data(), 4, theme.Text);
				}
				else if (node.HasFlag(VisualScriptNodeFlags::FunctionNode))
				{
					float const pixels = Metrics::LadderFloor(Metrics::HeaderGlyphSize * k * 1.25f);
					float const width =
						frame.Font
							->CalcTextSizeA(pixels, FLT_MAX, 0.0f, VisualScriptCanvasTexts::FunctionGlyph.data(),
					                        VisualScriptCanvasTexts::FunctionGlyph.data() + VisualScriptCanvasTexts::FunctionGlyph.size())
							.x;
					DrawText(frame, pixels, glm::floor(center - glm::vec2(width, pixels) * 0.5f), theme.Text,
					         VisualScriptCanvasTexts::FunctionGlyph);
				}
				item.Elements |= Elements::KindGlyph;
			}
			VisualScriptRect const latent = frame.ToScreen(VisualScriptGeometry::GetLatentGlyphRect(node, state.Offset));
			if (!VisualScriptGeometry::IsEmpty(latent))
			{
				// A clock: a ring and two hands.
				glm::vec2 const center = latent.GetCenter();
				float const radius = (latent.Max.x - latent.Min.x) * 0.5f;
				float const width = std::max(unit, std::round(radius * 0.18f));
				drawList->AddCircle(ToIm(center), radius - width * 0.5f, theme.Text, 0, width);
				drawList->AddLine(ToIm(center), ImVec2(center.x, center.y - radius * 0.6f), theme.Text, width);
				drawList->AddLine(ToIm(center), ImVec2(center.x + radius * 0.45f, center.y), theme.Text, width);
				item.Elements |= Elements::KindGlyph;
			}
		}
		if (Metrics::IsTextVisible(Metrics::TitleTextSize, frame.Zoom) && node.Title.Length > 0)
		{
			float const pixels = Metrics::LadderFloor(Metrics::TitleTextSize * k);
			VisualScriptRect const title = frame.ToScreen(VisualScriptGeometry::GetTitleRect(node, state.Offset));
			DrawText(frame, pixels, glm::floor(glm::vec2(title.Min.x, title.Min.y + (Metrics::TitleTextSize * k - pixels) * 0.5f)),
			         theme.Text, frame.Canvas.GetVisibleText(node.Title), node.Title.IsEllipsized());
			item.Elements |= Elements::Title;
		}
		if (frame.Level == VisualScriptDetailLevel::Detail && node.Subtitle.Length > 0 &&
		    Metrics::SubtitleTextSize * frame.Zoom >= Metrics::MinSubtitleLogicalPixels)
		{
			float const pixels = Metrics::LadderFloor(Metrics::SubtitleTextSize * k);
			VisualScriptRect const subtitle = frame.ToScreen(VisualScriptGeometry::GetSubtitleRect(node, state.Offset));
			uint32_t const color = ComputeVisualScriptNodeColors(node.HeaderColor, theme).Subtitle;
			DrawText(frame, pixels, glm::floor(glm::vec2(subtitle.Min.x, subtitle.Min.y + (Metrics::SubtitleTextSize * k - pixels) * 0.5f)),
			         color, frame.Canvas.GetVisibleText(node.Subtitle), node.Subtitle.IsEllipsized());
			item.Elements |= Elements::Subtitle;
		}
		if (collapsed)
		{
			VisualScriptRect const marker = frame.ToScreen(VisualScriptGeometry::GetCollapsedMarkerRect(node, state.Offset));
			if (!VisualScriptGeometry::IsEmpty(marker))
			{
				// Three dots (the UI font has no U+22EF).
				glm::vec2 const center = marker.GetCenter();
				float const radius = std::max(CollapsedDotRadius * k, 1.0f);
				float const step = (marker.Max.x - marker.Min.x) / 3.0f;
				for (int dot = -1; dot <= 1; dot++)
				{
					drawList->AddCircleFilled(ImVec2(center.x + static_cast<float>(dot) * step, center.y), radius, theme.Text, 8);
				}
				item.Elements |= Elements::CollapsedMarker;
			}
		}

		DrawPins(frame, node, state, item);
		DrawStrip(frame, node, state, strip, item);
		DrawBadges(frame, node, state, item);
		DrawNodeOutline(frame, node, state, rounding, item);
		DrawBubble(frame, node, state, item);
	}

	void VisualScriptCanvasRenderer::DrawCompactShape(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                                  VisualScriptDrawRecordItem& item)
	{
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		VisualScriptThemeAnchors const& theme = frame.Theme;
		bool const pill = node.Shape == VisualScriptNodeShape::Pill;
		float const rounding = pill ? (state.Screen.Max.y - state.Screen.Min.y) * 0.5f : Metrics::NodeCornerRadius * k;

		DrawShadowAndGlow(frame, state, item);
		if (pill)
		{
			// A variable's value or Self: the whole pill in its header's gradient.
			int const start = drawList->VtxBuffer.Size;
			drawList->AddRectFilled(ToIm(state.Screen.Min), ToIm(state.Screen.Max), MultiplyVisualScriptAlpha(node.HeaderColor, state.Fade),
			                        rounding);
			ImGui::ShadeVertsLinearColorGradientKeepAlpha(drawList, start, drawList->VtxBuffer.Size, ToIm(state.Screen.Min),
			                                              ImVec2(state.Screen.Max.x, state.Screen.Min.y), node.HeaderColor,
			                                              node.HeaderEndColor);
			item.Elements |= Elements::Header;
		}
		else
		{
			drawList->AddRectFilled(ToIm(state.Screen.Min), ToIm(state.Screen.Max), MultiplyVisualScriptAlpha(theme.Body, state.Fade),
			                        rounding);
		}
		item.Elements |= Elements::Body;
		float const sizeCu = pill ? Metrics::TitleTextSize : Metrics::OperatorSymbolTextSize;
		if (Metrics::IsTextVisible(sizeCu, frame.Zoom) && node.Title.Length > 0)
		{
			float const pixels = Metrics::LadderFloor(sizeCu * k);
			VisualScriptRect const title = frame.ToScreen(VisualScriptGeometry::GetTitleRect(node, state.Offset));
			DrawText(frame, pixels, glm::floor(glm::vec2(title.Min.x, title.Min.y + (sizeCu * k - pixels) * 0.5f)), theme.Text,
			         frame.Canvas.GetVisibleText(node.Title), node.Title.IsEllipsized());
			item.Elements |= Elements::Title;
		}
		DrawPins(frame, node, state, item);
		DrawBadges(frame, node, state, item);
		DrawNodeOutline(frame, node, state, rounding, item);
		DrawBubble(frame, node, state, item);
	}

	void VisualScriptCanvasRenderer::DrawReroute(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                             VisualScriptDrawRecordItem& item)
	{
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		float const s = frame.UIScale;
		float const unit = WholePixels(s);
		glm::vec2 const center = frame.ToScreen(VisualScriptGeometry::GetRerouteCenter(node, state.Offset));
		uint32_t typeColor = frame.Theme.Text;
		if (node.PinCount > 0)
		{
			VisualScriptPinVisual const& pin = frame.Pins[node.FirstPin];
			typeColor = pin.Shape == VisualScriptPinShape::Exec ? frame.Theme.Text : pin.Color;
		}
		float radius = std::max(Metrics::RerouteRadius * k, 3.0f * s);
		if (state.Hovered)
		{
			radius += 1.5f * s;
		}
		drawList->AddCircleFilled(ToIm(center), radius, MultiplyVisualScriptAlpha(typeColor, state.Fade));
		item.Elements |= Elements::Body;
		item.Color = typeColor;
		if (state.Hovered)
		{
			drawList->AddCircle(ToIm(center), radius + 0.5f * unit, frame.Theme.Text, 0, unit);
			item.Elements |= Elements::Outline;
		}
		if (state.Selected)
		{
			// The selection ring keeps a gap in the canvas's colour, and turns to the text colour where the selection colour is too
			// close to the carried type's (an Entity reroute).
			uint32_t const ring = GetVisualScriptRingColor(frame.Theme.Selection, typeColor, frame.Theme);
			drawList->AddCircle(ToIm(center), radius + 0.5f * unit, frame.Theme.Canvas, 0, unit);
			drawList->AddCircle(ToIm(center), radius + unit + unit, ring, 0, 2.0f * unit);
			item.Elements |= Elements::Outline;
			item.RingColor = ring;
			item.GapColor = frame.Theme.Canvas;
		}
		if (state.Flash)
		{
			float const alpha = frame.Input.ReduceMotion
			                        ? 1.0f
			                        : std::clamp(1.0f - static_cast<float>((frame.Input.Time - *state.Flash) / FlashSeconds), 0.0f, 1.0f);
			drawList->AddCircle(ToIm(center), radius + 3.0f * unit, MultiplyVisualScriptAlpha(frame.Theme.Selection, alpha), 0,
			                    2.0f * unit);
			item.Elements |= Elements::Flash;
		}
		item.Screen = {center - glm::vec2(radius), center + glm::vec2(radius)};
	}

	void VisualScriptCanvasRenderer::DrawBadges(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                            VisualScriptDrawRecordItem& item)
	{
		if (node.ErrorCount == 0 && node.WarningCount == 0)
		{
			return;
		}
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		float const s = frame.UIScale;
		float const radius = VisualScriptGeometry::GetBadgeRadius(k, s) * k;
		auto const drawCount = [&](glm::vec2 center, uint16_t count, uint32_t color)
		{
			if (count <= 1)
			{
				DrawExclamation(frame, center, radius, color);
				return;
			}
			char digits[4] = {};
			int const length = count > 9 ? std::snprintf(digits, sizeof(digits), "9+") : std::snprintf(digits, sizeof(digits), "%u", count);
			float const pixels = Metrics::LadderFloor(radius * BadgeCountTextSize / Metrics::BadgeRadius);
			if (pixels <= 0.0f || length <= 0)
			{
				DrawExclamation(frame, center, radius, color);
				return;
			}
			std::string_view const text(digits, static_cast<size_t>(length));
			float const width = frame.Font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
			DrawText(frame, pixels, glm::floor(center - glm::vec2(width, pixels) * 0.5f), color, text);
		};
		if (std::optional<glm::vec2> const error = VisualScriptGeometry::GetErrorBadgeCenter(node, state.Offset);
		    error && node.ErrorCount > 0)
		{
			glm::vec2 const center = frame.ToScreen(*error);
			drawList->AddCircleFilled(ToIm(center), radius, VisualScriptErrorColor);
			drawCount(center, node.ErrorCount, frame.Theme.Text);
		}
		if (std::optional<glm::vec2> const warning = VisualScriptGeometry::GetWarningBadgeCenter(node, k, s, state.Offset);
		    warning && node.WarningCount > 0)
		{
			// A triangle, listed clockwise on the screen.
			glm::vec2 const center = frame.ToScreen(*warning);
			drawList->AddTriangleFilled(ImVec2(center.x, center.y - radius), ImVec2(center.x + radius, center.y + radius * 0.8f),
			                            ImVec2(center.x - radius, center.y + radius * 0.8f), VisualScriptWarningColor);
			drawCount(center + glm::vec2(0.0f, radius * 0.15f), node.WarningCount, frame.Theme.Body);
		}
		item.Elements |= Elements::Badges;
	}

	void VisualScriptCanvasRenderer::DrawStrip(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                           VisualScriptRect const& strip, VisualScriptDrawRecordItem& item)
	{
		bool const disabled = node.HasFlag(VisualScriptNodeFlags::Disabled);
		bool const development = node.HasFlag(VisualScriptNodeFlags::DevelopmentOnly);
		if ((!disabled && !development) || VisualScriptGeometry::IsEmpty(strip))
		{
			return;
		}
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		float const unit = WholePixels(frame.UIScale);
		VisualScriptThemeAnchors const& theme = frame.Theme;
		std::string_view const label = development ? VisualScriptCanvasTexts::DevelopmentOnly : VisualScriptCanvasTexts::Disabled;
		// The pill that holds the label, centred in the strip, in the body's solid colour so the label reads the same along
		// its length.
		float const pixels = Metrics::IsTextVisible(StripLabelTextSize, frame.Zoom) ? Metrics::LadderFloor(StripLabelTextSize * k) : 0.0f;
		float const labelWidth =
			pixels > 0.0f ? frame.Font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, label.data(), label.data() + label.size()).x : 0.0f;
		glm::vec2 const center = strip.GetCenter();
		float const pillHalfWidth = labelWidth * 0.5f + StripPillPaddingX * k;
		float const pillHalfHeight = std::min((strip.Max.y - strip.Min.y) * 0.5f - unit, pixels * 0.5f + 2.0f * unit);
		ImVec2 const pillMin(center.x - pillHalfWidth, center.y - pillHalfHeight);
		ImVec2 const pillMax(center.x + pillHalfWidth, center.y + pillHalfHeight);

		drawList->AddRectFilled(ImVec2(strip.Min.x, strip.Min.y), ImVec2(strip.Max.x, strip.Min.y + unit),
		                        MultiplyVisualScriptAlpha(theme.Border, state.Fade));
		if (development)
		{
			// Amber, with 45° stripes outside the pill.
			float const rounding = Metrics::NodeCornerRadius * k;
			drawList->AddRectFilled(ImVec2(strip.Min.x, strip.Min.y + unit), ToIm(strip.Max),
			                        MultiplyVisualScriptAlpha(VisualScriptWarningColor, StripAlpha), rounding,
			                        ImDrawFlags_RoundCornersBottom);
			uint32_t const stripes = MultiplyVisualScriptAlpha(VisualScriptWarningColor, StripeAlpha);
			if (pixels > 0.0f)
			{
				DrawStripes(drawList, ImVec2(strip.Min.x + rounding * 0.5f, strip.Min.y + unit), ImVec2(pillMin.x, strip.Max.y),
				            StripeSpacing * k, StripeWidth * k, stripes);
				DrawStripes(drawList, ImVec2(pillMax.x, strip.Min.y + unit), ImVec2(strip.Max.x - rounding * 0.5f, strip.Max.y),
				            StripeSpacing * k, StripeWidth * k, stripes);
			}
			else
			{
				DrawStripes(drawList, ImVec2(strip.Min.x + rounding * 0.5f, strip.Min.y + unit),
				            ImVec2(strip.Max.x - rounding * 0.5f, strip.Max.y), StripeSpacing * k, StripeWidth * k, stripes);
			}
			item.Elements |= Elements::Hatching;
		}
		item.Elements |= Elements::Strip;
		if (pixels > 0.0f)
		{
			drawList->AddRectFilled(pillMin, pillMax, theme.Body, pillHalfHeight);
			DrawText(frame, pixels, glm::floor(glm::vec2(center.x - labelWidth * 0.5f, center.y - pixels * 0.5f)), theme.Text, label);
			item.Elements |= Elements::StripLabel;
		}
	}

	void VisualScriptCanvasRenderer::DrawBubble(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                            VisualScriptDrawRecordItem& item)
	{
		VisualScriptRect const bubble = VisualScriptGeometry::GetBubbleRect(node, state.Offset);
		if (node.Comment.Length == 0 || VisualScriptGeometry::IsEmpty(bubble))
		{
			return;
		}
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		VisualScriptRect const screen = frame.ToScreen(bubble);
		float const tail = Metrics::NodeCommentTailHeight * k;
		ImVec2 const lower = ToIm(screen.Min);
		ImVec2 const upper(screen.Max.x, screen.Max.y - tail);
		drawList->AddRectFilled(lower, upper, frame.Theme.Button, BubbleRounding * k);
		float const tailLeft = lower.x + Metrics::NodeCommentPadding * k * 2.0f;
		drawList->AddTriangleFilled(ImVec2(tailLeft, upper.y), ImVec2(tailLeft + Metrics::NodeCommentTailWidth * k, upper.y),
		                            ImVec2(tailLeft + Metrics::NodeCommentTailWidth * k * 0.5f, screen.Max.y), frame.Theme.Button);
		if (Metrics::IsTextVisible(Metrics::NodeCommentTextSize, frame.Zoom))
		{
			float const pixels = Metrics::LadderFloor(Metrics::NodeCommentTextSize * k);
			std::string_view const text = frame.Canvas.GetText(node.Comment);
			float const lineHeight = Metrics::NodeCommentTextSize * k;
			float const x = lower.x + Metrics::NodeCommentPadding * k;
			float y = lower.y + Metrics::NodeCommentPadding * k;
			uint32_t const lines = CountLines(text);
			for (uint32_t line = 0; line < lines; line++)
			{
				DrawText(frame, pixels, glm::floor(glm::vec2(x, y)), frame.Theme.Text, GetLine(text, line));
				y += lineHeight;
			}
		}
		item.Elements |= Elements::Bubble;
	}

	void VisualScriptCanvasRenderer::DrawPins(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                          VisualScriptDrawRecordItem& item)
	{
		if (frame.Level == VisualScriptDetailLevel::Far || node.PinCount == 0)
		{
			return;
		}
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		float const unit = WholePixels(frame.UIScale);
		bool const collapsed = node.HasFlag(VisualScriptNodeFlags::Collapsed) && frame.Preview.Expanded != state.Index &&
		                       node.Shape == VisualScriptNodeShape::Standard;
		if (collapsed)
		{
			// A collapsed node's wires end at its header's edges: one glyph per side stands for its pins.
			for (VisualScriptPinDirection const direction : {VisualScriptPinDirection::In, VisualScriptPinDirection::Out})
			{
				std::optional<uint32_t> first;
				bool linked = false;
				bool exec = false;
				for (uint32_t pin = node.FirstPin; pin < node.FirstPin + node.PinCount; pin++)
				{
					VisualScriptPinVisual const& visual = frame.Pins[pin];
					if (visual.Direction != direction)
					{
						continue;
					}
					if (!first)
					{
						first = pin;
					}
					linked = linked || visual.HasFlag(VisualScriptPinFlags::Linked);
					exec = exec || visual.Shape == VisualScriptPinShape::Exec;
				}
				if (first)
				{
					VisualScriptPinVisual pin = frame.Pins[*first];
					pin.Shape = exec ? VisualScriptPinShape::Exec : VisualScriptPinShape::Data;
					pin.Color = exec ? frame.Theme.Text : pin.Color;
					pin.Flags = static_cast<uint16_t>(linked ? VisualScriptPinFlags::Linked : 0u);
					DrawPinGlyph(frame, frame.ToScreen(VisualScriptGeometry::GetPinAnchor(node, frame.Pins[*first], state.Offset)), pin,
					             1.0f, MultiplyVisualScriptAlpha(pin.Color, state.Fade));
				}
			}
			item.Elements |= Elements::Pins;
			return;
		}
		for (uint32_t pin = node.FirstPin; pin < node.FirstPin + node.PinCount; pin++)
		{
			DrawPin(frame, node, state, pin, item);
		}
		item.Elements |= Elements::Pins;

		// Brackets along the node's edge span the rows of each split pin's fields, in the split pin type's colour.
		if (frame.Level == VisualScriptDetailLevel::Detail || frame.Level == VisualScriptDetailLevel::Compact)
		{
			for (uint32_t pin = node.FirstPin; pin < node.FirstPin + node.PinCount; pin++)
			{
				VisualScriptPinVisual const& start = frame.Pins[pin];
				if (!start.HasFlag(VisualScriptPinFlags::SplitGroupStart))
				{
					continue;
				}
				uint32_t last = pin;
				while (last + 1 < node.FirstPin + node.PinCount && frame.Pins[last + 1].HasFlag(VisualScriptPinFlags::SplitField) &&
				       !frame.Pins[last + 1].HasFlag(VisualScriptPinFlags::SplitGroupStart) &&
				       frame.Pins[last + 1].Direction == start.Direction)
				{
					last++;
				}
				glm::vec2 const top = frame.ToScreen(VisualScriptGeometry::GetPinAnchor(node, start, state.Offset));
				glm::vec2 const bottom = frame.ToScreen(VisualScriptGeometry::GetPinAnchor(node, frame.Pins[last], state.Offset));
				float const half = Metrics::PinRowHeight * 0.5f * k * 0.8f;
				float const x = start.Direction == VisualScriptPinDirection::In ? state.Screen.Min.x + BracketInset * k
				                                                                : state.Screen.Max.x - BracketInset * k - unit;
				drawList->AddRectFilled(ImVec2(x, top.y - half), ImVec2(x + unit, bottom.y + half),
				                        MultiplyVisualScriptAlpha(start.BracketColor, BracketAlpha * state.Fade));
				pin = last;
			}
		}
		DrawBypass(frame, node, state, item);
	}

	void VisualScriptCanvasRenderer::DrawBypass(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                            VisualScriptDrawRecordItem& item)
	{
		if (!node.HasFlag(VisualScriptNodeFlags::Disabled) || !node.HasFlag(VisualScriptNodeFlags::PassThroughExit) ||
		    node.PassThroughPin >= node.PinCount || frame.Level == VisualScriptDetailLevel::Far)
		{
			return;
		}
		std::optional<uint32_t> input;
		for (uint32_t pin = node.FirstPin; pin < node.FirstPin + node.PinCount; pin++)
		{
			if (frame.Pins[pin].Direction == VisualScriptPinDirection::In && frame.Pins[pin].Shape == VisualScriptPinShape::Exec)
			{
				input = pin;
				break;
			}
		}
		if (!input)
		{
			return;
		}
		// Where the flow goes through the disabled node: a dashed line from its exec input to its pass-through exit.
		float const s = frame.UIScale;
		glm::vec2 const from = frame.ToScreen(VisualScriptGeometry::GetPinAnchor(node, frame.Pins[*input], state.Offset));
		glm::vec2 const to =
			frame.ToScreen(VisualScriptGeometry::GetPinAnchor(node, frame.Pins[node.FirstPin + node.PassThroughPin], state.Offset));
		std::array<glm::vec2, 2> const line = {from, to};
		Stroke(frame, line, MultiplyVisualScriptAlpha(frame.Theme.Text, BypassAlpha), 2.0f * WholePixels(s), BypassDash * s, BypassGap * s);
		item.Elements |= Elements::Bypass;
	}

	void VisualScriptCanvasRenderer::DrawPinGlyph(Frame& frame, glm::vec2 anchor, VisualScriptPinVisual const& pin, float scale,
	                                              uint32_t color)
	{
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit * scale;
		bool const linked = pin.HasFlag(VisualScriptPinFlags::Linked);
		if (frame.Level == VisualScriptDetailLevel::Overview)
		{
			// Dots: every pin's place and type colour, nothing more.
			drawList->AddCircleFilled(ToIm(anchor), std::max(Metrics::PinGlyphRadius * k, 1.5f * frame.UIScale), color, 8);
			return;
		}
		float const line = std::max(1.0f, std::round(1.5f * frame.PixelsPerUnit));
		bool const orphan = pin.HasFlag(VisualScriptPinFlags::Orphan);
		VisualScriptPinShape const shape = orphan ? VisualScriptPinShape::Wildcard : pin.Shape;
		switch (shape)
		{
			case VisualScriptPinShape::Exec:
			{
				std::array<ImVec2, 5> points;
				for (size_t i = 0; i < points.size(); i++)
				{
					points[i] = ToIm(anchor + ExecPentagon[i] * k);
				}
				if (linked)
				{
					drawList->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
				}
				else
				{
					drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, line, ImDrawFlags_Closed);
				}
				break;
			}
			case VisualScriptPinShape::Data:
			{
				float const radius = Metrics::PinGlyphRadius * k;
				if (linked)
				{
					drawList->AddCircleFilled(ToIm(anchor), radius, color);
				}
				else
				{
					drawList->AddCircle(ToIm(anchor), std::max(radius - line * 0.5f, 0.5f), color, 0, line);
					if (pin.HasFlag(VisualScriptPinFlags::Splittable))
					{
						// The split marker: a centre dot in the ring.
						drawList->AddCircleFilled(ToIm(anchor), std::max(SplitMarkerRadius * k, 1.0f), color, 8);
					}
				}
				break;
			}
			case VisualScriptPinShape::Array:
			{
				float const cell = ArrayCell * k;
				float const gap = ArrayGap * k;
				float const total = 3.0f * cell + 2.0f * gap;
				glm::vec2 const origin = anchor - glm::vec2(total * 0.5f);
				uint32_t const fill = linked ? color : MultiplyVisualScriptAlpha(color, ArrayUnlinkedAlpha);
				for (int row = 0; row < 3; row++)
				{
					for (int column = 0; column < 3; column++)
					{
						glm::vec2 const lower = origin + glm::vec2(static_cast<float>(column), static_cast<float>(row)) * (cell + gap);
						drawList->AddRectFilled(ToIm(lower), ToIm(lower + glm::vec2(cell)), fill);
					}
				}
				if (!linked)
				{
					drawList->AddRect(ToIm(origin), ToIm(origin + glm::vec2(total)), color, 0.0f, 1.0f);
				}
				break;
			}
			case VisualScriptPinShape::Wildcard:
			{
				float const half = WildcardHalfDiagonal * k;
				std::array<ImVec2, 4> const diamond = {ImVec2(anchor.x, anchor.y - half), ImVec2(anchor.x + half, anchor.y),
				                                       ImVec2(anchor.x, anchor.y + half), ImVec2(anchor.x - half, anchor.y)};
				uint32_t const diamondColor = orphan ? VisualScriptErrorColor : color;
				if (linked && !orphan)
				{
					drawList->AddConvexPolyFilled(diamond.data(), 4, diamondColor);
				}
				else
				{
					drawList->AddPolyline(diamond.data(), 4, diamondColor, line, ImDrawFlags_Closed);
				}
				break;
			}
		}
	}

	void VisualScriptCanvasRenderer::DrawPin(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, uint32_t pinIndex,
	                                         VisualScriptDrawRecordItem& item)
	{
		ImDrawList* const drawList = frame.DrawList;
		VisualScriptPinVisual const& pin = frame.Pins[pinIndex];
		float const k = frame.PixelsPerUnit;
		float const unit = WholePixels(frame.UIScale);
		glm::vec2 const anchor = frame.ToScreen(VisualScriptGeometry::GetPinAnchor(node, pin, state.Offset));
		auto const pinClass = pinIndex < frame.Gesture.PinClasses.size()
		                          ? static_cast<VisualScriptPinClass>(frame.Gesture.PinClasses[pinIndex])
		                          : VisualScriptPinClass::None;
		bool const hovered = frame.HoveredPin == pinIndex;
		bool const problem = pin.HasFlag(VisualScriptPinFlags::HasError) || pin.HasFlag(VisualScriptPinFlags::HasWarning);

		VisualScriptDrawRecordPin record;
		record.Node = node.ID;
		record.Pin = pinIndex;
		float glow = 0.0f;
		uint32_t ringState = 0;
		float ringWidth = 0.0f;
		switch (pinClass)
		{
			case VisualScriptPinClass::Compatible:
				record.States |= VisualScriptPinDrawStates::Compatible;
				record.Scale = CompatiblePinScale;
				glow = CompatibleGlowAlpha;
				ringState = frame.Theme.Selection;
				ringWidth = 1.5f * frame.UIScale;
				break;
			case VisualScriptPinClass::Converting:
				record.States |= VisualScriptPinDrawStates::Converting;
				ringState = VisualScriptWarningColor;
				ringWidth = 1.5f * frame.UIScale;
				break;
			case VisualScriptPinClass::Incompatible:
				record.States |= VisualScriptPinDrawStates::Incompatible;
				record.Alpha = IncompatibleAlpha;
				break;
			case VisualScriptPinClass::Snapped:
				// The snapped pin's glow is drawn in the overlay, above every node.
				record.States |= VisualScriptPinDrawStates::Snapped;
				record.Scale = SnappedPinScale;
				break;
			case VisualScriptPinClass::None:
				break;
		}
		if (hovered)
		{
			record.States |= VisualScriptPinDrawStates::Hovered;
			record.Scale = std::max(record.Scale, HoveredPinScale);
			glow = std::max(glow, HoveredGlowAlpha);
		}
		if (problem && ringState == 0)
		{
			record.States |= VisualScriptPinDrawStates::Problem;
			ringState = pin.HasFlag(VisualScriptPinFlags::HasError) ? VisualScriptErrorColor : VisualScriptWarningColor;
			ringWidth = unit;
		}
		uint32_t const typeColor = pin.HasFlag(VisualScriptPinFlags::Orphan) ? VisualScriptErrorColor : pin.Color;
		uint32_t const glyphColor = MultiplyVisualScriptAlpha(typeColor, state.Fade * record.Alpha);
		bool const detailed = frame.Level == VisualScriptDetailLevel::Detail || frame.Level == VisualScriptDetailLevel::Compact;

		// The row-wide hover, where labels are drawn (and pins are hit over their rows).
		if (hovered && frame.LabelsDrawn && node.Shape == VisualScriptNodeShape::Standard)
		{
			VisualScriptRect const row = frame.ToScreen(VisualScriptGeometry::GetPinHitRect(node, pin, 0.0f, state.Offset, true));
			drawList->AddRectFilled(ToIm(row.Min), ToIm(row.Max), MultiplyVisualScriptAlpha(frame.Theme.Text, RowHoverAlpha), 2.0f * unit);
		}
		if (glow > 0.0f && detailed)
		{
			DrawSoftDot(frame, anchor, PinGlowRadius * Metrics::PinGlyphRadius * k * record.Scale,
			            MultiplyVisualScriptAlpha(typeColor, glow));
		}
		DrawPinGlyph(frame, anchor, pin, record.Scale, glyphColor);
		if (ringState != 0 && detailed)
		{
			// Rings keep a gap in the colour behind them, and turn to the text colour where the state's colour is too close to the
			// type's, so no state vanishes on any type.
			float const extent =
				(pin.Shape == VisualScriptPinShape::Data ? Metrics::PinGlyphRadius : WildcardHalfDiagonal) * k * record.Scale;
			record.RingColor = GetVisualScriptRingColor(ringState, typeColor, frame.Theme);
			record.GapColor = frame.Theme.Body;
			drawList->AddCircle(ToIm(anchor), extent + unit * 0.5f, frame.Theme.Body, 0, unit);
			drawList->AddCircle(ToIm(anchor), extent + unit + ringWidth * 0.5f, MultiplyVisualScriptAlpha(record.RingColor, record.Alpha),
			                    0, ringWidth);
		}

		// The label, at full opacity on disabled nodes; in the problem's colour on a pin with one.
		if (frame.LabelsDrawn && !pin.HasFlag(VisualScriptPinFlags::LabelHidden) && pin.Label.Length > 0)
		{
			VisualScriptRect const label = frame.ToScreen(VisualScriptGeometry::GetPinLabelRect(node, pin, state.Offset));
			if (!VisualScriptGeometry::IsEmpty(label))
			{
				uint32_t color = GetVisualScriptPinLabelColor(frame.Theme);
				if (pin.HasFlag(VisualScriptPinFlags::Orphan) || pin.HasFlag(VisualScriptPinFlags::HasError))
				{
					color = VisualScriptErrorColor;
				}
				else if (pin.HasFlag(VisualScriptPinFlags::HasWarning))
				{
					color = VisualScriptWarningColor;
				}
				float const pixels = Metrics::LadderFloor(Metrics::PinLabelTextSize * k);
				DrawText(frame, pixels, glm::floor(glm::vec2(label.Min.x, label.Min.y + (Metrics::PinLabelTextSize * k - pixels) * 0.5f)),
				         MultiplyVisualScriptAlpha(color, record.Alpha), frame.Canvas.GetVisibleText(pin.Label), pin.Label.IsEllipsized());
				item.Elements |= Elements::PinLabels;
			}
		}
		// The value: the live widget, a chip with frames at Detail, its text at Compact.
		if (!VisualScriptGeometry::IsEmpty(pin.ChipRect) && pin.ChipKind != VisualScriptChipKind::None)
		{
			if (frame.LiveNode == state.Index && frame.LivePin == pinIndex)
			{
				DrawLiveChip(frame, node, state, pinIndex, item);
			}
			else if (frame.Level == VisualScriptDetailLevel::Detail ||
			         (frame.Level == VisualScriptDetailLevel::Compact && Metrics::IsTextVisible(Metrics::ChipTextSize, frame.Zoom)))
			{
				DrawChip(frame, node, state, pin, record.Alpha, item);
			}
		}
		if (frame.Input.Record != nullptr && record.States != 0)
		{
			frame.Input.Record->Pins.push_back(record);
		}
	}

	void VisualScriptCanvasRenderer::DrawChip(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                          VisualScriptPinVisual const& pin, float alpha, VisualScriptDrawRecordItem& item)
	{
		ImDrawList* const drawList = frame.DrawList;
		float const k = frame.PixelsPerUnit;
		VisualScriptThemeAnchors const& theme = frame.Theme;
		// The cells the live widget would lay out (MakeChip), so the widget replaces the chip without moving a pixel.
		uint32_t const pinIndex = static_cast<uint32_t>(&pin - frame.Pins.data());
		VisualScriptLiveChip const layout = MakeChip(frame, node, state, pinIndex);
		VisualScriptRect const chip = layout.GetCell(0);
		float const pixels = Metrics::LadderFloor(Metrics::ChipTextSize * k);
		float const textTop = std::floor(chip.Min.y + ((chip.Max.y - chip.Min.y) - pixels) * 0.5f);
		float const padding = Metrics::ChipPaddingX * k;
		float const rounding = frame.Input.Style.FrameRounding * frame.Zoom;
		bool const frames = frame.Level == VisualScriptDetailLevel::Detail;
		uint32_t textColor = theme.Text;
		if (pin.HasFlag(VisualScriptPinFlags::ChipInvalid))
		{
			textColor = VisualScriptErrorColor;
		}
		else if (pin.HasFlag(VisualScriptPinFlags::ChipIsDefault))
		{
			textColor = MultiplyVisualScriptAlpha(theme.Text, VisualScriptDefaultValueAlpha);
		}
		textColor = MultiplyVisualScriptAlpha(textColor, alpha);
		uint32_t const frameColor = MultiplyVisualScriptAlpha(theme.Frame, alpha);
		std::string_view const text = frame.Canvas.GetVisibleText(pin.ChipText);
		bool const ellipsis = pin.ChipText.IsEllipsized();
		auto const textWidth = [&](std::string_view value)
		{
			return frame.Font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, value.data(), value.data() + value.size()).x;
		};

		switch (pin.ChipKind)
		{
			case VisualScriptChipKind::None:
				return;
			case VisualScriptChipKind::Placeholder:
				// "Self" or "None": no frame.
				DrawText(frame, pixels, glm::vec2(chip.Min.x, textTop), MultiplyVisualScriptAlpha(theme.TextDisabled, alpha), text,
				         ellipsis);
				break;
			case VisualScriptChipKind::Check:
			{
				if (frames)
				{
					drawList->AddRectFilled(ToIm(chip.Min), ToIm(chip.Max), frameColor, rounding);
					if (text == "true")
					{
						// As ImGui::Checkbox draws its mark: inset by a sixth of the box, at least a pixel.
						float const size = chip.Max.y - chip.Min.y;
						float const inset = std::max(1.0f, std::floor(size / 6.0f));
						ImGui::RenderCheckMark(drawList, ImVec2(chip.Min.x + inset, chip.Min.y + inset),
						                       MultiplyVisualScriptAlpha(theme.Selection, alpha), size - inset * 2.0f);
					}
				}
				else
				{
					DrawText(frame, pixels, glm::vec2(chip.Min.x, textTop), textColor, text);
				}
				break;
			}
			case VisualScriptChipKind::Color:
			{
				drawList->AddRectFilled(ToIm(chip.Min), ToIm(chip.Max), pin.ChipColor | IM_COL32_A_MASK, rounding);
				break;
			}
			case VisualScriptChipKind::Number:
			{
				// Drags centre their text.
				if (frames)
				{
					drawList->AddRectFilled(ToIm(chip.Min), ToIm(chip.Max), frameColor, rounding);
					float const width = textWidth(text) + (ellipsis ? textWidth(VisualScriptEllipsis) : 0.0f);
					DrawText(frame, pixels, glm::floor(glm::vec2(chip.GetCenter().x - width * 0.5f, textTop)), textColor, text, ellipsis);
				}
				else
				{
					DrawText(frame, pixels, glm::vec2(chip.Min.x, textTop), textColor, text, ellipsis);
				}
				break;
			}
			case VisualScriptChipKind::Cells:
			{
				// One frame per component, each with its axis's stripe at its left edge, the text centred.
				std::string_view const all = frame.Canvas.GetText(pin.ChipText);
				uint32_t const cells = CountLines(all);
				for (uint32_t cell = 0; cell < cells; cell++)
				{
					VisualScriptRect const cellRect = layout.GetCell(cell);
					std::string_view const value = GetLine(all, cell);
					if (frames)
					{
						drawList->AddRectFilled(ToIm(cellRect.Min), ToIm(cellRect.Max), frameColor, rounding);
						drawList->AddRectFilled(ToIm(cellRect.Min),
						                        ImVec2(cellRect.Min.x + Metrics::ChipAxisStripeWidth * k, cellRect.Max.y),
						                        MultiplyVisualScriptAlpha(AxisColors[std::min<size_t>(cell, AxisColors.size() - 1)], alpha),
						                        rounding, ImDrawFlags_RoundCornersLeft);
					}
					float const width = textWidth(value);
					DrawText(frame, pixels, glm::floor(glm::vec2(cellRect.GetCenter().x - width * 0.5f, textTop)), textColor, value);
				}
				break;
			}
			case VisualScriptChipKind::Choice:
			{
				if (frames)
				{
					drawList->AddRectFilled(ToIm(chip.Min), ToIm(chip.Max), frameColor, rounding);
					// The arrow box, as a combo draws it: a square of the frame's height in the button colour.
					float const box = Metrics::ChipArrowBoxWidth * k;
					ImVec2 const boxMin(chip.Max.x - box, chip.Min.y);
					drawList->AddRectFilled(boxMin, ToIm(chip.Max), MultiplyVisualScriptAlpha(theme.Button, alpha), rounding,
					                        ImDrawFlags_RoundCornersRight);
					float const fontSize = drawList->_Data->FontSize;
					if (fontSize > 0.0f && pixels > 0.0f)
					{
						float const inset = ((chip.Max.y - chip.Min.y) - pixels) * 0.5f;
						ImGui::RenderArrow(drawList, ImVec2(boxMin.x + inset, chip.Min.y + inset), textColor, ImGuiDir_Down,
						                   pixels / fontSize);
					}
				}
				DrawText(frame, pixels, glm::vec2(chip.Min.x + (frames ? padding : 0.0f), textTop), textColor, text, ellipsis);
				break;
			}
			case VisualScriptChipKind::Text:
			case VisualScriptChipKind::Asset:
			case VisualScriptChipKind::Matrix:
			case VisualScriptChipKind::Raw:
			{
				// Text fields left-align their text after the frame's padding.
				if (frames)
				{
					drawList->AddRectFilled(ToIm(chip.Min), ToIm(chip.Max), frameColor, rounding);
				}
				DrawText(frame, pixels, glm::vec2(chip.Min.x + (frames ? padding : 0.0f), textTop), textColor, text, ellipsis);
				break;
			}
		}
		item.Elements |= frames ? Elements::Chips : Elements::ChipText;
	}

	VisualScriptLiveChip VisualScriptCanvasRenderer::MakeChip(Frame const& frame, VisualScriptNodeVisual const& node,
	                                                          NodeState const& state, uint32_t pinIndex) const
	{
		VisualScriptPinVisual const& pin = frame.Pins[pinIndex];
		float const k = frame.PixelsPerUnit;
		VisualScriptLiveChip chip;
		chip.Node = node.ID;
		chip.NodeIndex = state.Index;
		chip.Pin = pinIndex;
		chip.Kind = pin.ChipKind;
		chip.Rect = frame.ToScreen(VisualScriptGeometry::GetChipRect(node, pin, state.Offset));
		float const height = chip.Rect.Max.y - chip.Rect.Min.y;
		// Cells are as wide as ImGui makes the widget's items: whole pixels (ImGui::CalcItemWidth truncates widths), except
		// the squares of check boxes and colour buttons, which are the frame's height.
		switch (pin.ChipKind)
		{
			case VisualScriptChipKind::Cells:
				chip.CellCount = std::max(1u, CountLines(frame.Canvas.GetText(pin.ChipText)));
				chip.CellWidth = std::max(1.0f, std::floor(Metrics::ChipVectorCellWidth * k));
				break;
			case VisualScriptChipKind::Check:
			case VisualScriptChipKind::Color:
				chip.CellWidth = height;
				break;
			case VisualScriptChipKind::None:
			case VisualScriptChipKind::Number:
			case VisualScriptChipKind::Text:
			case VisualScriptChipKind::Choice:
			case VisualScriptChipKind::Asset:
			case VisualScriptChipKind::Matrix:
			case VisualScriptChipKind::Placeholder:
			case VisualScriptChipKind::Raw:
				chip.CellWidth = std::max(1.0f, std::floor(chip.Rect.Max.x - chip.Rect.Min.x));
				break;
		}
		chip.CellGap = Metrics::ChipCellGap * k;
		chip.FontPixels = Metrics::LadderFloor(Metrics::ChipTextSize * k);
		chip.FontSizeBase = chip.FontPixels / std::max(frame.Input.Style.FontScale, 1e-3f);
		// The chip's metrics, not the theme's: the theme's frame padding scaled by the zoom would make the widget taller than
		// the chip it replaces.
		chip.FramePadding = ImVec2(Metrics::ChipPaddingX * k, std::max(0.0f, (height - chip.FontPixels) * 0.5f));
		chip.ItemSpacing = ImVec2(chip.CellGap, 0.0f);
		chip.FrameRounding = frame.Input.Style.FrameRounding * frame.Zoom;
		chip.GrabMinSize = frame.Input.Style.GrabMinSize * frame.Zoom;
		return chip;
	}

	void VisualScriptCanvasRenderer::DrawLiveChip(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state,
	                                              uint32_t pinIndex, VisualScriptDrawRecordItem& item)
	{
		VisualScriptLiveChip const chip = MakeChip(frame, node, state, pinIndex);
		CountTextSize(chip.FontPixels);
		frame.Input.LiveChips->DrawLiveChip(chip);
		m_Stats.LiveWidgets++;
		item.Elements |= Elements::LiveWidget;
	}

	void VisualScriptCanvasRenderer::DrawOverlay(Frame& frame)
	{
		ImDrawList* const drawList = frame.DrawList;
		VisualScriptGestureView const& gesture = frame.Gesture;
		float const k = frame.PixelsPerUnit;
		float const s = frame.UIScale;
		float const unit = WholePixels(s);
		auto const record =
			[&](VisualScriptDrawItemKind kind, uint32_t element, uint32_t elements, uint32_t color, VisualScriptRect const& screen)
		{
			if (frame.Input.Record != nullptr)
			{
				VisualScriptDrawRecordItem item;
				item.Layer = VisualScriptDrawLayer::Overlay;
				item.Kind = kind;
				item.Element = element;
				item.Level = frame.Level;
				item.Elements = elements;
				item.Color = color;
				item.Screen = screen;
				item.Order = frame.Order++;
				frame.Input.Record->Items.push_back(std::move(item));
			}
		};
		VisualScriptGeometry::WireFlattening const flattening =
			VisualScriptGeometry::GetWireFlattening(frame.Level, frame.Input.Style.CurveTessellationTolerance);
		auto const flatten = [&](std::array<glm::vec2, 4> points)
		{
			for (glm::vec2& point : points)
			{
				point = frame.ToScreen(point);
			}
			VisualScriptGeometry::FlattenWire(points, flattening.Tolerance, flattening.SegmentCap, m_Polyline);
			return VisualScriptGeometry::GetWireBounds(points);
		};
		auto const wireThickness = [&](bool exec)
		{
			return exec ? std::clamp(std::round(3.0f * k), 1.0f, std::round(5.0f * s))
			            : std::clamp(std::round(2.0f * k), 1.0f, std::round(4.0f * s));
		};

		// The dragged wire: its source type's colour, a gradient onto a converting pin, dashed in the error colour over a pin it
		// cannot connect to.
		if (gesture.GhostWire)
		{
			bool exec = false;
			if (gesture.WireSource != nullptr)
			{
				if (std::optional<uint32_t> const node = frame.Canvas.FindNode(gesture.WireSource->Node))
				{
					if (std::optional<uint32_t> const pin = frame.Canvas.FindPin(*node, gesture.WireSource->Pin, gesture.WireDirection))
					{
						exec = frame.Pins[*pin].Shape == VisualScriptPinShape::Exec;
					}
				}
			}
			VisualScriptRect const bounds = flatten(*gesture.GhostWire);
			float const thickness = wireThickness(exec);
			uint32_t elements = 0;
			if (gesture.GhostInvalid)
			{
				Stroke(frame, m_Polyline, VisualScriptErrorColor, thickness, CutDash * s, CutGap * s);
				elements |= Elements::Dashed;
			}
			else
			{
				std::pair<uint32_t, uint32_t> const vertices =
					Stroke(frame, m_Polyline, MultiplyVisualScriptAlpha(gesture.GhostColorFrom, GhostAlpha), thickness);
				if (gesture.GhostColorFrom != gesture.GhostColorTo)
				{
					elements |= RecolorAlongLength(frame, vertices, m_Polyline, thickness, gesture.GhostColorFrom, gesture.GhostColorTo)
					                ? Elements::Gradient
					                : Elements::GradientFallback;
				}
			}
			record(VisualScriptDrawItemKind::GhostWire, 0, elements, gesture.GhostInvalid ? VisualScriptErrorColor : gesture.GhostColorFrom,
			       bounds);
		}
		// Links picked up with Ctrl+drag, from their far ends to the mouse.
		for (size_t picked = 0; picked < gesture.PickedWires.size(); picked++)
		{
			uint32_t color = frame.Theme.Text;
			bool exec = true;
			if (picked < gesture.PickedLinks.size())
			{
				if (std::optional<uint32_t> const link = FindLinkIndex(frame.Canvas, gesture.PickedLinks[picked]))
				{
					VisualScriptLinkVisual const& visual = frame.Links[*link];
					exec = visual.HasFlag(VisualScriptLinkFlags::Exec);
					color = exec ? frame.Theme.Text : visual.FromColor;
				}
			}
			VisualScriptRect const bounds = flatten(gesture.PickedWires[picked]);
			Stroke(frame, m_Polyline, MultiplyVisualScriptAlpha(color, GhostAlpha), wireThickness(exec));
			record(VisualScriptDrawItemKind::PickedWire, static_cast<uint32_t>(picked), 0, color, bounds);
		}
		if (gesture.Marquee)
		{
			VisualScriptRect const screen = frame.ToScreen(*gesture.Marquee);
			drawList->AddRectFilled(ToIm(screen.Min), ToIm(screen.Max), MultiplyVisualScriptAlpha(frame.Theme.Selection, MarqueeFillAlpha));
			drawList->AddRect(ToIm(screen.Min), ToIm(screen.Max), MultiplyVisualScriptAlpha(frame.Theme.Selection, MarqueeOutlineAlpha),
			                  0.0f, unit);
			record(VisualScriptDrawItemKind::Marquee, 0, Elements::Body | Elements::Outline, frame.Theme.Selection, screen);
		}
		if (gesture.CutLine)
		{
			std::array<glm::vec2, 2> const line = {frame.ToScreen(gesture.CutLine->first), frame.ToScreen(gesture.CutLine->second)};
			Stroke(frame, line, VisualScriptErrorColor, 2.0f * unit, CutDash * s, CutGap * s);
			record(VisualScriptDrawItemKind::CutLine, 0, Elements::Dashed, VisualScriptErrorColor,
			       {glm::min(line[0], line[1]), glm::max(line[0], line[1])});
		}

		// New links' pulses: a soft dot running from the output to the input (a static highlight with Reduce Motion).
		for (ActivePulse const& pulse : m_ActivePulses)
		{
			uint32_t elements = pulse.Highlight > 0.0f ? Elements::PulseHighlight : 0u;
			VisualScriptRect bounds;
			if (pulse.Progress >= 0.0f && frame.Level != VisualScriptDetailLevel::Far)
			{
				bounds = flatten(GetWirePoints(frame, pulse.Link));
				glm::vec2 const point = VisualScriptGeometry::GetPolylinePoint(m_Polyline, pulse.Progress);
				VisualScriptLinkVisual const& visual = frame.Links[pulse.Link];
				uint32_t const color =
					MixVisualScriptColors(visual.HasFlag(VisualScriptLinkFlags::Exec) ? frame.Theme.Text : visual.FromColor,
				                          VisualScriptRgba(255, 255, 255), 0.5f);
				if (DrawSoftDot(frame, point, std::max(PulseDotRadius * k, 4.0f * s), color))
				{
					elements |= Elements::PulseDot;
				}
			}
			if (elements != 0)
			{
				record(VisualScriptDrawItemKind::Pulse, pulse.Link, elements, 0, bounds);
			}
		}

		// The snapped pin's glow, above every node.
		if (frame.Level != VisualScriptDetailLevel::Far && !gesture.PinClasses.empty())
		{
			for (uint32_t const nodeIndex : m_VisibleNodes)
			{
				VisualScriptNodeVisual node = frame.Nodes[nodeIndex];
				if (frame.Preview.Expanded == nodeIndex)
				{
					node = VisualScriptGeometry::GetExpandedNode(node);
				}
				for (uint32_t pin = node.FirstPin; pin < node.FirstPin + node.PinCount && pin < gesture.PinClasses.size(); pin++)
				{
					if (static_cast<VisualScriptPinClass>(gesture.PinClasses[pin]) == VisualScriptPinClass::Snapped)
					{
						glm::vec2 const anchor =
							frame.ToScreen(VisualScriptGeometry::GetPinAnchor(node, frame.Pins[pin], GetNodeOffset(frame, nodeIndex)));
						DrawSoftDot(frame, anchor, PinGlowRadius * Metrics::PinGlyphRadius * k * SnappedPinScale,
						            MultiplyVisualScriptAlpha(frame.Pins[pin].Color, SnappedGlowAlpha));
					}
				}
			}
		}
		DrawPills(frame);
	}

	void VisualScriptCanvasRenderer::AppendTypeName(Frame const& frame, std::string_view type, bool withID)
	{
		if (type == VisualScriptExecType)
		{
			m_PillText.append(VisualScriptCanvasTexts::Exec);
			return;
		}
		VisualScriptTypeInfo const* info =
			frame.Input.Library != nullptr && type != VisualScriptWildcardType ? frame.Input.Library->FindType(type) : nullptr;
		if (info == nullptr || info->Title.empty())
		{
			m_PillText.append(type);
			return;
		}
		m_PillText.append(info->Title);
		if (withID && info->Title != type)
		{
			m_PillText.append(" (");
			m_PillText.append(type);
			m_PillText.append(")");
		}
	}

	void VisualScriptCanvasRenderer::BuildPinText(Frame const& frame, uint32_t nodeIndex, uint32_t pinIndex)
	{
		VisualScriptCanvasModel const& canvas = frame.Canvas;
		VisualScriptNodeVisual const& node = frame.Nodes[nodeIndex];
		VisualScriptPinVisual const& pin = frame.Pins[pinIndex];
		std::string_view const name = canvas.GetText(pin.Name);
		std::string_view const label = pin.Label.Length > 0 ? canvas.GetText(pin.Label) : name;
		m_PillText.assign(label);
		m_PillText.append(VisualScriptCanvasTexts::Separator);
		AppendTypeName(frame, pin.Type, true);

		// A wildcard of the library's template: what it resolved to.
		if (frame.Input.Library != nullptr && !pin.HasFlag(VisualScriptPinFlags::Orphan))
		{
			VisualScriptDocument const& document = frame.Model->GetDisplayDocument();
			VisualScriptGraph const* graph = &document.Graph;
			if (uint32_t const function = canvas.GetFunction(); function != 0)
			{
				auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
				                                [function](VisualScriptFunction const& candidate)
				                                {
													return candidate.ID == function;
												});
				graph = found != document.Functions.end() ? &found->Graph : nullptr;
			}
			VisualScriptNodeTemplate const* nodeTemplate = nullptr;
			if (graph != nullptr)
			{
				auto const stored = std::find_if(graph->Nodes.begin(), graph->Nodes.end(),
				                                 [&node](VisualScriptNode const& candidate)
				                                 {
													 return candidate.ID == node.ID;
												 });
				nodeTemplate = stored != graph->Nodes.end() ? frame.Input.Library->FindNode(stored->Type) : nullptr;
			}
			VisualScriptPin const* templatePin = nodeTemplate != nullptr ? nodeTemplate->FindPin(name, pin.Direction) : nullptr;
			if (templatePin != nullptr && templatePin->Type == VisualScriptWildcardType && pin.Type != VisualScriptExecType)
			{
				m_PillText.append("\n");
				if (pin.Type == VisualScriptWildcardType)
				{
					m_PillText.append(VisualScriptCanvasTexts::Unresolved);
				}
				else
				{
					m_PillText.append(VisualScriptWildcardType);
					m_PillText.append(VisualScriptCanvasTexts::ResolvedTo);
					AppendTypeName(frame, pin.Type, false);
				}
			}
		}
		if (pin.Direction == VisualScriptPinDirection::In && !pin.HasFlag(VisualScriptPinFlags::Linked) && pin.ChipText.Length > 0 &&
		    pin.Type != VisualScriptExecType)
		{
			m_PillText.append("\n");
			m_PillText.append(VisualScriptCanvasTexts::Default);
			std::string_view const value = canvas.GetText(pin.ChipText);
			for (char const c : value)
			{
				// Vector components are one per line in the chip; here they read as a list.
				if (c == '\n')
				{
					m_PillText.append(", ");
				}
				else
				{
					m_PillText.push_back(c);
				}
			}
		}
		if (pin.Direction == VisualScriptPinDirection::In && pin.HasFlag(VisualScriptPinFlags::Linked))
		{
			for (uint32_t const link : canvas.GetIncidentLinks(nodeIndex))
			{
				VisualScriptLinkVisual const& visual = frame.Links[link];
				if (visual.ToPin == pinIndex && visual.HasFlag(VisualScriptLinkFlags::Converted))
				{
					m_PillText.append("\n");
					m_PillText.append(VisualScriptCanvasTexts::ConvertedFrom);
					AppendTypeName(frame, frame.Pins[visual.FromPin].Type, false);
					break;
				}
			}
		}
		if (VisualScriptAnalysis const* analysis = frame.Model->GetAnalysis())
		{
			for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
			{
				if (diagnostic.Node == node.ID && diagnostic.Pin == name &&
				    (!diagnostic.Direction || *diagnostic.Direction == pin.Direction))
				{
					m_PillText.append("\n");
					m_PillText.append(diagnostic.Message);
				}
			}
		}
	}

	void VisualScriptCanvasRenderer::BuildWireText(Frame const& frame, uint32_t link)
	{
		VisualScriptCanvasModel const& canvas = frame.Canvas;
		VisualScriptLinkVisual const& visual = frame.Links[link];
		VisualScriptPinVisual const& from = frame.Pins[visual.FromPin];
		VisualScriptPinVisual const& to = frame.Pins[visual.ToPin];
		auto const appendEnd = [&](VisualScriptPinVisual const& pin)
		{
			m_PillText.append(pin.Label.Length > 0 ? canvas.GetText(pin.Label) : canvas.GetText(pin.Name));
			if (pin.Type != VisualScriptExecType)
			{
				m_PillText.append(" (");
				m_PillText.append(GetShortTypeName(pin.Type));
				m_PillText.append(")");
			}
		};
		m_PillText.clear();
		appendEnd(from);
		m_PillText.append(VisualScriptCanvasTexts::Arrow);
		appendEnd(to);
		if (visual.HasFlag(VisualScriptLinkFlags::Converted))
		{
			m_PillText.append(VisualScriptCanvasTexts::Converted);
		}
		if (VisualScriptAnalysis const* analysis = frame.Model->GetAnalysis();
		    analysis != nullptr && visual.HasFlag(VisualScriptLinkFlags::Error))
		{
			VisualScriptLink const stored = canvas.GetDocumentLink(link);
			for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
			{
				if (diagnostic.Link && *diagnostic.Link == stored)
				{
					m_PillText.append("\n");
					m_PillText.append(diagnostic.Message);
				}
			}
		}
	}

	void VisualScriptCanvasRenderer::DrawPills(Frame& frame)
	{
		VisualScriptHit const& hover = frame.Model->GetHover();
		VisualScriptGestureView const& gesture = frame.Gesture;
		bool const detailed = frame.Level == VisualScriptDetailLevel::Detail || frame.Level == VisualScriptDetailLevel::Compact;
		bool const wiring = gesture.GhostWire.has_value();

		// What the mouse rests on, by names rather than indices, so a layout during the hover does not restart its delay.
		uint64_t key = 0;
		if ((frame.Input.Hovered || wiring) && hover.Kind != VisualScriptHitKind::None)
		{
			key = HashValue(static_cast<uint64_t>(hover.Kind) | (static_cast<uint64_t>(hover.Element) << 8), HashSeed);
			if (hover.Kind == VisualScriptHitKind::Pin && frame.HoveredPin)
			{
				key = HashText(frame.Canvas.GetText(frame.Pins[*frame.HoveredPin].Name), key);
			}
			else if (hover.Kind == VisualScriptHitKind::Link && hover.Link)
			{
				key = HashText(hover.Link->To.Pin,
				               HashText(hover.Link->From.Pin, HashValue(hover.Link->To.Node, HashValue(hover.Link->From.Node, key))));
			}
		}
		if (key != m_HoverKey)
		{
			m_HoverKey = key;
			m_HoverSince = frame.Input.Time;
		}
		if (key == 0)
		{
			return;
		}
		double const rested = frame.Input.Time - m_HoverSince;
		bool const delayed = rested >= frame.Input.Style.HoverDelayShort;
		VisualScriptRect const bounds{frame.CanvasMin, frame.CanvasMax};
		float const fontSize = frame.Input.Style.FontSize;
		uint32_t element = 0;
		glm::vec2 anchor = frame.Input.Mouse;
		bool show = false;

		if (wiring && gesture.GhostInvalid && gesture.WireSource != nullptr && hover.Kind == VisualScriptHitKind::Pin && frame.HoveredPin)
		{
			// Why the wire cannot connect, naming both types, at once.
			m_PillText.assign(VisualScriptCanvasTexts::Cannot);
			std::string_view sourceType = VisualScriptWildcardType;
			if (std::optional<uint32_t> const node = frame.Canvas.FindNode(gesture.WireSource->Node))
			{
				if (std::optional<uint32_t> const pin = frame.Canvas.FindPin(*node, gesture.WireSource->Pin, gesture.WireDirection))
				{
					sourceType = frame.Pins[*pin].Type;
				}
			}
			VisualScriptPinVisual const& target = frame.Pins[*frame.HoveredPin];
			m_PillText.append(gesture.WireSource->Pin);
			m_PillText.append(" (");
			AppendTypeName(frame, sourceType, false);
			m_PillText.append(")");
			m_PillText.append(VisualScriptCanvasTexts::Arrow);
			m_PillText.append(frame.Canvas.GetText(target.Label.Length > 0 ? target.Label : target.Name));
			m_PillText.append(" (");
			AppendTypeName(frame, target.Type, false);
			m_PillText.append(")");
			element = hover.Element;
			show = true;
		}
		else if (hover.Kind == VisualScriptHitKind::Pin && frame.HoveredPin && frame.HoveredNode && detailed)
		{
			bool const snapped = *frame.HoveredPin < gesture.PinClasses.size() &&
			                     static_cast<VisualScriptPinClass>(gesture.PinClasses[*frame.HoveredPin]) == VisualScriptPinClass::Snapped;
			if (delayed || snapped)
			{
				BuildPinText(frame, *frame.HoveredNode, *frame.HoveredPin);
				element = hover.Element;
				show = true;
			}
		}
		else if (hover.Kind == VisualScriptHitKind::Link && frame.HoveredLink && delayed && !wiring &&
		         frame.Level != VisualScriptDetailLevel::Far)
		{
			// At the wire's point nearest the mouse.
			std::array<glm::vec2, 4> points = GetWirePoints(frame, *frame.HoveredLink);
			for (glm::vec2& point : points)
			{
				point = frame.ToScreen(point);
			}
			VisualScriptGeometry::WireFlattening const flattening =
				VisualScriptGeometry::GetWireFlattening(frame.Level, frame.Input.Style.CurveTessellationTolerance);
			VisualScriptGeometry::FlattenWire(points, flattening.Tolerance, flattening.SegmentCap, m_Polyline);
			anchor = VisualScriptGeometry::GetPolylinePoint(m_Polyline, hover.ArcFraction);
			BuildWireText(frame, *frame.HoveredLink);
			show = true;
		}
		else if (hover.Kind == VisualScriptHitKind::Badge && frame.HoveredNode && delayed)
		{
			// The node's problems, one per line.
			m_PillText.clear();
			if (VisualScriptAnalysis const* analysis = frame.Model->GetAnalysis())
			{
				for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
				{
					if (diagnostic.Node == hover.Element)
					{
						if (!m_PillText.empty())
						{
							m_PillText.append("\n");
						}
						m_PillText.append(diagnostic.Message);
					}
				}
			}
			element = hover.Element;
			show = !m_PillText.empty();
		}
		if (!show)
		{
			return;
		}
		VisualScriptRect const rect =
			DrawVisualScriptValuePill(frame.DrawList, frame.Font, fontSize, anchor, bounds, m_PillText, frame.Theme, frame.Theme.Text);
		CountTextSize(fontSize);
		if (frame.Input.Record != nullptr)
		{
			VisualScriptDrawRecordItem item;
			item.Layer = VisualScriptDrawLayer::Overlay;
			item.Kind = VisualScriptDrawItemKind::Pill;
			item.Element = element;
			item.Level = frame.Level;
			item.Elements = Elements::Body;
			item.Screen = rect;
			item.Text = m_PillText;
			item.Order = frame.Order++;
			frame.Input.Record->Items.push_back(std::move(item));
		}
	}

	void VisualScriptCanvasRenderer::RebuildMinimapCells(Frame& frame, VisualScriptMinimapTransform const& transform,
	                                                     uint64_t selectionHash)
	{
		m_MinimapRevision = frame.Model->GetCanvasRevision();
		m_MinimapTransform = transform;
		m_MinimapSelectionHash = selectionHash;
		glm::vec2 const size = transform.Rect.GetSize();
		m_MinimapColumns = static_cast<uint32_t>(std::max(0.0f, std::ceil(size.x / MinimapCell)));
		m_MinimapRows = static_cast<uint32_t>(std::max(0.0f, std::ceil(size.y / MinimapCell)));
		m_MinimapCells.assign(static_cast<size_t>(m_MinimapColumns) * m_MinimapRows, 0u);
		if (m_MinimapColumns == 0 || m_MinimapRows == 0)
		{
			return;
		}
		// Nodes are merged into cells of MinimapCell pixels, so drawing costs the minimap's area whatever the node count.
		// Selected nodes are drawn over the others, and nodes with errors over every other.
		auto const paint = [&](VisualScriptNodeVisual const& node, uint32_t color)
		{
			glm::vec2 const lower = (transform.ToScreen(node.Position) - transform.Rect.Min) / MinimapCell;
			glm::vec2 const upper = (transform.ToScreen(node.Position + node.Size) - transform.Rect.Min) / MinimapCell;
			int const columnStart = std::clamp(static_cast<int>(std::floor(lower.x)), 0, static_cast<int>(m_MinimapColumns) - 1);
			int const columnEnd = std::clamp(static_cast<int>(std::floor(upper.x)), 0, static_cast<int>(m_MinimapColumns) - 1);
			int const rowStart = std::clamp(static_cast<int>(std::floor(lower.y)), 0, static_cast<int>(m_MinimapRows) - 1);
			int const rowEnd = std::clamp(static_cast<int>(std::floor(upper.y)), 0, static_cast<int>(m_MinimapRows) - 1);
			for (int row = rowStart; row <= rowEnd; row++)
			{
				for (int column = columnStart; column <= columnEnd; column++)
				{
					m_MinimapCells[static_cast<size_t>(row) * m_MinimapColumns + static_cast<size_t>(column)] = color;
				}
			}
		};
		for (VisualScriptNodeVisual const& node : frame.Nodes)
		{
			paint(node, node.BoxColor | IM_COL32_A_MASK);
		}
		for (uint32_t index = 0; index < frame.Nodes.size(); index++)
		{
			if (frame.IsNodeSelected(index))
			{
				paint(frame.Nodes[index], frame.Theme.Selection | IM_COL32_A_MASK);
			}
		}
		for (VisualScriptNodeVisual const& node : frame.Nodes)
		{
			if (node.ErrorCount > 0)
			{
				paint(node, VisualScriptErrorColor);
			}
		}
	}

	void VisualScriptCanvasRenderer::DrawMinimap(Frame& frame)
	{
		VisualScriptMinimapState const& minimap = frame.Model->GetMinimap();
		VisualScriptMinimapTransform const& transform = minimap.Transform;
		if (minimap.Alpha <= 0.0f || transform.Scale <= 0.0f || VisualScriptGeometry::IsEmpty(transform.Rect))
		{
			return;
		}
		ImDrawList* const drawList = frame.DrawList;
		float const alpha = std::clamp(minimap.Alpha, 0.0f, 1.0f);
		float const unit = WholePixels(frame.UIScale);
		VisualScriptRect const& rect = transform.Rect;
		drawList->AddRectFilled(ToIm(rect.Min), ToIm(rect.Max), MultiplyVisualScriptAlpha(frame.Theme.Body, MinimapBodyAlpha * alpha),
		                        MinimapRounding * frame.UIScale);

		// Comments in their colour at half opacity, clamped to the minimap's rectangle (no clip rectangle is pushed).
		for (VisualScriptCommentVisual const& comment : frame.Comments)
		{
			ImVec2 lower = ToIm(transform.ToScreen(comment.Rect.Min));
			ImVec2 upper = ToIm(transform.ToScreen(comment.Rect.Max));
			if (ClampRect(lower, upper, rect))
			{
				drawList->AddRectFilled(
					lower, upper,
					MultiplyVisualScriptAlpha((comment.BodyColor & ~IM_COL32_A_MASK) | IM_COL32_A_MASK, MinimapCommentAlpha * alpha));
			}
		}

		uint64_t selectionHash = HashSeed;
		for (uint32_t const element : frame.Model->GetSelection())
		{
			selectionHash = HashValue(element, selectionHash);
		}
		if (m_MinimapRevision != frame.Model->GetCanvasRevision() || !(m_MinimapTransform == transform) ||
		    m_MinimapSelectionHash != selectionHash)
		{
			RebuildMinimapCells(frame, transform, selectionHash);
		}
		for (uint32_t row = 0; row < m_MinimapRows; row++)
		{
			uint32_t column = 0;
			while (column < m_MinimapColumns)
			{
				uint32_t const color = m_MinimapCells[static_cast<size_t>(row) * m_MinimapColumns + column];
				uint32_t end = column + 1;
				while (end < m_MinimapColumns && m_MinimapCells[static_cast<size_t>(row) * m_MinimapColumns + end] == color)
				{
					end++;
				}
				if (color != 0)
				{
					ImVec2 lower(rect.Min.x + static_cast<float>(column) * MinimapCell, rect.Min.y + static_cast<float>(row) * MinimapCell);
					ImVec2 upper(rect.Min.x + static_cast<float>(end) * MinimapCell, lower.y + MinimapCell);
					if (ClampRect(lower, upper, rect))
					{
						drawList->AddRectFilled(lower, upper, MultiplyVisualScriptAlpha(color, alpha));
					}
				}
				column = end;
			}
		}

		// The view.
		ImVec2 viewMin = ToIm(transform.ToScreen(frame.Visible.Min));
		ImVec2 viewMax = ToIm(transform.ToScreen(frame.Visible.Max));
		if (ClampRect(viewMin, viewMax, rect))
		{
			drawList->AddRectFilled(viewMin, viewMax, MultiplyVisualScriptAlpha(frame.Theme.Selection, MinimapViewFillAlpha * alpha));
			drawList->AddRect(viewMin, viewMax, MultiplyVisualScriptAlpha(frame.Theme.Selection, MinimapViewOutlineAlpha * alpha), 0.0f,
			                  unit);
		}
		drawList->AddRect(ToIm(rect.Min), ToIm(rect.Max), MultiplyVisualScriptAlpha(frame.Theme.Border, alpha),
		                  MinimapRounding * frame.UIScale, unit);
		if (frame.Input.Record != nullptr)
		{
			VisualScriptDrawRecordItem item;
			item.Layer = VisualScriptDrawLayer::Minimap;
			item.Kind = VisualScriptDrawItemKind::Minimap;
			item.Level = frame.Level;
			item.Elements = Elements::Body | Elements::Border;
			item.Screen = rect;
			item.Color = frame.Theme.Body;
			item.Order = frame.Order++;
			frame.Input.Record->Items.push_back(std::move(item));
		}
	}
}
