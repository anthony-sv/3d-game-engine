#include "Editor/Panels/VisualScriptEditorPanel.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptThemeStyle.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <glm/gtc/constants.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <utility>
#include <variant>

namespace Strada
{
	namespace
	{
		using Action = VisualScriptEditorAction;

		constexpr ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs |
		                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		// Both scroll flags keep the wheel in the canvas (spec §8.1 rule 1).
		constexpr ImGuiWindowFlags CanvasFlags =
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoMove;
		// The text edited in place over the canvas is at most this many font sizes wide.
		constexpr float TextEditWidth = 18.0f;
		constexpr float TextEditLines = 4.0f;
		// The tooltips wrap at this many font sizes, as the content browser's.
		constexpr float TooltipWrap = 32.0f;
		// The gesture hint's background: the theme's Body at 92 %.
		constexpr float HintBackgroundAlpha = 0.92f;
		// The focus frame: Selection at 60 %.
		constexpr float FocusFrameAlpha = 0.6f;
		// The play state's colours of the viewport's frame (ViewportPanel.cpp's PlayFrameColor and SimulateFrameColor, which
		// are file-local there); a game that runs the last working version takes the notices' amber.
		constexpr ImU32 PlayColor = IM_COL32(64, 160, 255, 255);
		constexpr ImU32 SimulateColor = IM_COL32(120, 200, 120, 255);
		// The notices' accents: the content browser's notice colours (ContentBrowserPanel.cpp's NoticeWarningColor), an
		// information blue and the neutral grey of read-only banners.
		constexpr ImU32 NoticeWarningColor = IM_COL32(232, 164, 56, 255);
		constexpr ImU32 NoticeInfoColor = IM_COL32(86, 156, 230, 255);
		constexpr ImU32 NoticeNeutralColor = IM_COL32(140, 146, 156, 255);
		// How much of its accent a notice's background takes, and while it blinks.
		constexpr float NoticeTint = 0.16f;
		constexpr float NoticeBlinkTint = 0.5f;
		// The shortcut sheet's width and the cards' widths, in font sizes.
		constexpr float ShortcutSheetWidth = 34.0f;
		constexpr float CardWidth = 26.0f;
		// The canvas border kept around elements a [Show] frames, in screen pixels at UI scale 1.
		constexpr float FrameMargin = 48.0f;
		namespace Texts = VisualScriptPanelTexts;

		ImU32 MixColor(ImU32 from, ImU32 to, float amount)
		{
			ImVec4 const a = ImGui::ColorConvertU32ToFloat4(from);
			ImVec4 const b = ImGui::ColorConvertU32ToFloat4(to);
			float const t = std::clamp(amount, 0.0f, 1.0f);
			return ImGui::ColorConvertFloat4ToU32(
				ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
		}

		// "1 node", "3 nodes and 1 comment".
		std::string CountElements(size_t nodes, size_t comments)
		{
			auto const count = [](size_t value, std::string_view one, std::string_view many)
			{
				return fmt::format("{} {}", value, value == 1 ? one : many);
			};
			if (comments == 0)
			{
				return count(nodes, "node", "nodes");
			}
			if (nodes == 0)
			{
				return count(comments, "comment", "comments");
			}
			return count(nodes, "node", "nodes") + " and " + count(comments, "comment", "comments");
		}

		// Glyphs of the toolbar's buttons, drawn as shapes (the editor has no icon font).
		enum class Glyph : uint8_t
		{
			Sidebar = 0,
			Details,
			Back,
			Forward,
			Arrange,
			Find,
			Grid,
			Minimap,
			Dots,
			Saved,
			NotSaved,
			ListArrow,
			Close,
			Bookmark
		};

		ImU32 WithAlpha(ImU32 color, float alpha)
		{
			uint32_t const a =
				static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFu));
			return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
		}

		void DrawGlyph(ImDrawList* drawList, Glyph glyph, ImVec2 center, float size, ImU32 color)
		{
			float const h = size * 0.5f;
			float const thickness = std::max(1.0f, size * 0.12f);
			ImVec2 const lower(center.x - h, center.y - h * 0.8f);
			ImVec2 const upper(center.x + h, center.y + h * 0.8f);
			switch (glyph)
			{
				case Glyph::Sidebar:
				case Glyph::Details:
				{
					drawList->AddRect(lower, upper, color, 1.0f, thickness);
					float const third = (upper.x - lower.x) / 3.0f;
					if (glyph == Glyph::Sidebar)
					{
						drawList->AddRectFilled(lower, ImVec2(lower.x + third, upper.y), color);
					}
					else
					{
						drawList->AddRectFilled(ImVec2(upper.x - third, lower.y), upper, color);
					}
					break;
				}
				case Glyph::Back:
				case Glyph::Forward:
				{
					float const direction = glyph == Glyph::Back ? -1.0f : 1.0f;
					std::array<ImVec2, 3> const points = {ImVec2(center.x - direction * h * 0.35f, center.y - h * 0.7f),
					                                      ImVec2(center.x + direction * h * 0.35f, center.y),
					                                      ImVec2(center.x - direction * h * 0.35f, center.y + h * 0.7f)};
					drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, thickness * 1.3f);
					break;
				}
				case Glyph::Arrange:
				{
					// Three bars of different lengths, aligned left: nodes laid out in columns.
					for (int row = 0; row < 3; row++)
					{
						float const y = center.y + (static_cast<float>(row) - 1.0f) * h * 0.6f;
						float const length = row == 1 ? 1.4f : 2.0f - static_cast<float>(row) * 0.3f;
						drawList->AddLine(ImVec2(lower.x, y), ImVec2(lower.x + h * length, y), color, thickness * 1.3f);
					}
					break;
				}
				case Glyph::Find:
				{
					ImVec2 const lens(center.x - h * 0.2f, center.y - h * 0.2f);
					drawList->AddCircle(lens, h * 0.5f, color, 0, thickness);
					drawList->AddLine(ImVec2(lens.x + h * 0.36f, lens.y + h * 0.36f), ImVec2(center.x + h * 0.75f, center.y + h * 0.75f),
					                  color, thickness * 1.4f);
					break;
				}
				case Glyph::Grid:
				{
					for (int line = 1; line <= 2; line++)
					{
						float const offset = -h + static_cast<float>(line) * size / 3.0f;
						drawList->AddLine(ImVec2(center.x + offset, center.y - h), ImVec2(center.x + offset, center.y + h), color,
						                  thickness);
						drawList->AddLine(ImVec2(center.x - h, center.y + offset), ImVec2(center.x + h, center.y + offset), color,
						                  thickness);
					}
					break;
				}
				case Glyph::Minimap:
				{
					drawList->AddRect(lower, upper, color, 1.0f, thickness);
					drawList->AddRectFilled(ImVec2(center.x + h * 0.1f, center.y + h * 0.1f),
					                        ImVec2(upper.x - thickness, upper.y - thickness), color);
					break;
				}
				case Glyph::Dots:
				{
					for (int dot = -1; dot <= 1; dot++)
					{
						drawList->AddCircleFilled(ImVec2(center.x + static_cast<float>(dot) * h * 0.6f, center.y),
						                          std::max(1.0f, size * 0.09f), color);
					}
					break;
				}
				case Glyph::Saved:
				{
					std::array<ImVec2, 3> const points = {ImVec2(center.x - h * 0.6f, center.y),
					                                      ImVec2(center.x - h * 0.15f, center.y + h * 0.45f),
					                                      ImVec2(center.x + h * 0.65f, center.y - h * 0.5f)};
					drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, thickness * 1.3f);
					break;
				}
				case Glyph::NotSaved:
				{
					drawList->AddCircle(center, h * 0.8f, color, 0, thickness);
					drawList->AddLine(ImVec2(center.x, center.y - h * 0.45f), ImVec2(center.x, center.y + h * 0.1f), color,
					                  thickness * 1.3f);
					drawList->AddCircleFilled(ImVec2(center.x, center.y + h * 0.38f), thickness * 0.8f, color);
					break;
				}
				case Glyph::ListArrow:
				{
					drawList->AddTriangleFilled(ImVec2(center.x - h * 0.5f, center.y - h * 0.25f),
					                            ImVec2(center.x + h * 0.5f, center.y - h * 0.25f), ImVec2(center.x, center.y + h * 0.3f),
					                            color);
					break;
				}
				case Glyph::Close:
				{
					drawList->AddLine(ImVec2(center.x - h * 0.5f, center.y - h * 0.5f), ImVec2(center.x + h * 0.5f, center.y + h * 0.5f),
					                  color, thickness);
					drawList->AddLine(ImVec2(center.x + h * 0.5f, center.y - h * 0.5f), ImVec2(center.x - h * 0.5f, center.y + h * 0.5f),
					                  color, thickness);
					break;
				}
				case Glyph::Bookmark:
				{
					// A ribbon with a notch at its foot.
					std::array<ImVec2, 5> const points = {
						ImVec2(center.x - h * 0.45f, center.y - h * 0.75f), ImVec2(center.x + h * 0.45f, center.y - h * 0.75f),
						ImVec2(center.x + h * 0.45f, center.y + h * 0.75f), ImVec2(center.x, center.y + h * 0.35f),
						ImVec2(center.x - h * 0.45f, center.y + h * 0.75f)};
					drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, thickness, ImDrawFlags_Closed);
					break;
				}
			}
		}

		// The problems pill's counts at the pill's top-left (screen pixels): an error glyph and its count, a warning glyph and its
		// count, each glyph badge pixels wide and grey when its count is 0.
		void DrawProblemCounts(ImDrawList* drawList, glm::vec2 pillMin, float badge, uint32_t errors, uint32_t warnings)
		{
			ImGuiStyle const& style = ImGui::GetStyle();
			float const fontSize = ImGui::GetFontSize();
			ImU32 const textColor = ImGui::GetColorU32(ImGuiCol_Text);
			std::string const errorCount = fmt::format("{}", errors);
			std::string const warningCount = fmt::format("{}", warnings);
			float const radius = badge * 0.5f;
			float x = pillMin.x + style.FramePadding.x + radius;
			float const y = pillMin.y + ImGui::GetFrameHeight() * 0.5f;
			ImU32 const errorColor = errors > 0 ? VisualScriptErrorColor : ImGui::GetColorU32(ImGuiCol_TextDisabled);
			drawList->AddCircleFilled(ImVec2(x, y), radius, errorColor);
			DrawGlyph(drawList, Glyph::Close, ImVec2(x, y), radius * 1.4f, IM_COL32(255, 255, 255, 255));
			drawList->AddText(ImVec2(x + radius + style.ItemInnerSpacing.x, y - fontSize * 0.5f), textColor, errorCount.c_str());
			x += radius + style.ItemInnerSpacing.x + ImGui::CalcTextSize(errorCount.c_str()).x + style.ItemSpacing.x + radius;
			ImU32 const warningColor = warnings > 0 ? VisualScriptWarningColor : ImGui::GetColorU32(ImGuiCol_TextDisabled);
			drawList->AddTriangleFilled(ImVec2(x, y - radius), ImVec2(x + radius, y + radius * 0.8f), ImVec2(x - radius, y + radius * 0.8f),
			                            warningColor);
			drawList->AddText(ImVec2(x + radius + style.ItemInnerSpacing.x, y - fontSize * 0.5f), textColor, warningCount.c_str());
		}

		// A square button of the frame's height showing a glyph; active buttons are highlighted. Its tooltip names what it does.
		bool GlyphButton(char const* id, Glyph glyph, bool active, bool enabled, std::string const& tooltip)
		{
			float const size = ImGui::GetFrameHeight();
			ImGui::BeginDisabled(!enabled);
			if (active)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			bool const clicked = ImGui::Button(id, ImVec2(size, size));
			if (active)
			{
				ImGui::PopStyleColor();
			}
			ImVec2 const lower = ImGui::GetItemRectMin();
			ImVec2 const upper = ImGui::GetItemRectMax();
			DrawGlyph(ImGui::GetWindowDrawList(), glyph, ImVec2((lower.x + upper.x) * 0.5f, (lower.y + upper.y) * 0.5f), size * 0.55f,
			          ImGui::GetColorU32(ImGuiCol_Text));
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("%s", tooltip.c_str());
			return clicked && enabled;
		}

		// "Back (Ctrl+[)": what a button does and the key that does the same.
		std::string WithShortcut(std::string_view text, Action action)
		{
			VisualScriptShortcut const* shortcut = FindVisualScriptShortcut(action);
			if (shortcut == nullptr)
			{
				return std::string(text);
			}
			return fmt::format("{} ({})", text, FormatVisualScriptChord(shortcut->Chord, ImGui::GetIO().ConfigMacOSXBehaviors));
		}

		std::string GetShortcutText(Action action)
		{
			VisualScriptShortcut const* shortcut = FindVisualScriptShortcut(action);
			return shortcut != nullptr ? FormatVisualScriptChord(shortcut->Chord, ImGui::GetIO().ConfigMacOSXBehaviors) : std::string();
		}

		std::string GetGestureText(VisualScriptMouseGestureId id)
		{
			VisualScriptMouseGesture const* gesture = FindVisualScriptMouseGesture(id);
			return gesture != nullptr ? FormatVisualScriptMouseGesture(*gesture, ImGui::GetIO().ConfigMacOSXBehaviors) : std::string();
		}

		// The longest start of the text that fits the width with an ellipsis after it (whole code points), or the text itself
		// when it fits.
		std::string FitText(std::string_view text, float width)
		{
			if (ImGui::CalcTextSize(text.data(), text.data() + text.size()).x <= width)
			{
				return std::string(text);
			}
			std::string const ellipsis = UI::GetEllipsis();
			float const available = width - ImGui::CalcTextSize(ellipsis.c_str()).x;
			size_t length = 0;
			size_t next = 0;
			while (next < text.size())
			{
				size_t end = next + 1;
				while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u)
				{
					end++;
				}
				if (ImGui::CalcTextSize(text.data(), text.data() + end).x > available)
				{
					break;
				}
				length = end;
				next = end;
			}
			return std::string(text.substr(0, length)) + ellipsis;
		}

		ImVec2 ToImVec2(glm::vec2 value)
		{
			return ImVec2(value.x, value.y);
		}
		glm::vec2 ToVec2(ImVec2 value)
		{
			return glm::vec2(value.x, value.y);
		}

		ImGuiMouseCursor GetResizeCursor(uint8_t edges)
		{
			using namespace VisualScriptGeometry;
			bool const left = (edges & LeftEdge) != 0;
			bool const right = (edges & RightEdge) != 0;
			bool const top = (edges & TopEdge) != 0;
			bool const bottom = (edges & BottomEdge) != 0;
			if ((left && top) || (right && bottom))
			{
				return ImGuiMouseCursor_ResizeNWSE;
			}
			if ((right && top) || (left && bottom))
			{
				return ImGuiMouseCursor_ResizeNESW;
			}
			if (left || right)
			{
				return ImGuiMouseCursor_ResizeEW;
			}
			return top || bottom ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_Arrow;
		}

		// A node of the shown sub-graph; null when that graph does not hold it.
		VisualScriptNode const* FindShownNode(VisualScriptDocument const& document, uint32_t function, uint32_t node)
		{
			VisualScriptGraph const* graph = FindVisualScriptGraph(document, function);
			if (graph == nullptr)
			{
				return nullptr;
			}
			for (VisualScriptNode const& candidate : graph->Nodes)
			{
				if (candidate.ID == node)
				{
					return &candidate;
				}
			}
			return nullptr;
		}

		// The problems the analysis reports in a sub-graph: errors, then warnings.
		std::pair<uint32_t, uint32_t> CountProblems(VisualScriptAnalysis const* analysis, std::optional<uint32_t> function)
		{
			uint32_t errors = 0;
			uint32_t warnings = 0;
			if (analysis == nullptr)
			{
				return {0, 0};
			}
			for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
			{
				if (function && diagnostic.Function != *function)
				{
					continue;
				}
				(diagnostic.Severity == VisualScriptSeverity::Error ? errors : warnings)++;
			}
			return {errors, warnings};
		}

		// ImGui's text events of the frame as UTF-8 (the model filters spaces and control characters itself).
		std::string ReadTypedCharacters()
		{
			std::string text;
			for (ImWchar const character : ImGui::GetIO().InputQueueCharacters)
			{
				std::array<char, 5> buffer{};
				int const length = ImTextCharToUtf8(buffer.data(), character);
				text.append(buffer.data(), static_cast<size_t>(length));
			}
			return text;
		}

		// A link of the visual model by the document's link (pins by the names the document stores or the canvas shows).
		std::optional<uint32_t> FindLinkVisual(VisualScriptCanvasModel const& canvas, VisualScriptLink const& link)
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

		// Whether a diagnostic is about a link: the same nodes and pins, as the document stores them.
		bool IsDiagnosticOfLink(VisualScriptDiagnostic const& diagnostic, VisualScriptLink const& link)
		{
			return diagnostic.Link && diagnostic.Link->From.Node == link.From.Node && diagnostic.Link->To.Node == link.To.Node &&
			       diagnostic.Link->From.Pin == link.From.Pin && diagnostic.Link->To.Pin == link.To.Pin;
		}

		// The keys My Script's row takes (spec §11.3 rule 5): what Details showing a row does with them.
		bool IsRowAction(Action action)
		{
			return action == Action::Delete || action == Action::DeleteWithoutRelink || action == Action::Rename ||
			       action == Action::Duplicate || action == Action::OpenNode;
		}

		// Whether a window is a popup begun inside root: by root, one of its child windows or another such popup. ImGui makes the
		// window a popup was begun in its parent (imgui.cpp:8002), but a popup is its own root window, outside root's focus route
		// (imgui.cpp:7850-7856, :8031).
		bool IsPopupOf(ImGuiWindow const* popup, ImGuiWindow const* root)
		{
			if (popup == nullptr || (popup->Flags & ImGuiWindowFlags_Popup) == 0)
			{
				return false;
			}
			for (ImGuiWindow const* parent = popup->ParentWindow; parent != nullptr; parent = parent->ParentWindow)
			{
				if (parent == root)
				{
					return true;
				}
			}
			return false;
		}

		uint64_t HashBits(uint64_t seed, uint64_t value)
		{
			// The 64-bit FNV-1a step over the value's bytes: a stable signature of what the measurer measures with.
			for (int byte = 0; byte < 8; byte++)
			{
				seed ^= (value >> (byte * 8)) & 0xFFu;
				seed *= 0x100000001B3ull;
			}
			return seed;
		}
	}

	VisualScriptWindowLayout ComputeVisualScriptWindowLayout(float width, float fontSize, float splitter,
	                                                         VisualScriptPaneSettings const& panes)
	{
		VisualScriptWindowLayout layout;
		float const maximum = std::max(width, 0.0f) * VisualScriptPaneMaxShare;
		auto const paneWidth = [&](float widthInFontSizes, float minimum)
		{
			return std::min(std::max(widthInFontSizes, minimum) * fontSize, maximum);
		};
		float sidebar = panes.ShowSidebar ? paneWidth(panes.Sidebar, VisualScriptSidebarMinWidth) : 0.0f;
		float details = panes.ShowDetails ? paneWidth(panes.Details, VisualScriptDetailsMinWidth) : 0.0f;
		layout.SidebarState = panes.ShowSidebar ? VisualScriptPaneState::Shown : VisualScriptPaneState::Hidden;
		layout.DetailsState = panes.ShowDetails ? VisualScriptPaneState::Shown : VisualScriptPaneState::Hidden;
		auto const canvasWidth = [&]
		{
			return width - (sidebar > 0.0f ? sidebar + splitter : 0.0f) - (details > 0.0f ? details + splitter : 0.0f);
		};
		float const canvasMinimum = VisualScriptCanvasMinWidth * fontSize;
		if (sidebar > 0.0f && details > 0.0f && canvasWidth() < canvasMinimum)
		{
			// Too narrow side by side: one column for both panes when the canvas leaves it Details' minimum width.
			float const room = width - splitter - canvasMinimum;
			float const columnMinimum = VisualScriptDetailsMinWidth * fontSize;
			if (room >= columnMinimum)
			{
				layout.Stacked = true;
				layout.Column = std::clamp(room, columnMinimum, std::max({sidebar, details, columnMinimum}));
				layout.Canvas = width - splitter - layout.Column;
				return layout;
			}
		}
		if (details > 0.0f && canvasWidth() < canvasMinimum)
		{
			details = 0.0f;
			layout.DetailsState = VisualScriptPaneState::AutoHidden;
		}
		if (sidebar > 0.0f && canvasWidth() < canvasMinimum)
		{
			sidebar = 0.0f;
			layout.SidebarState = VisualScriptPaneState::AutoHidden;
		}
		layout.Sidebar = sidebar;
		layout.Details = details;
		layout.Canvas = std::max(canvasWidth(), 0.0f);
		return layout;
	}

	VisualScriptEditorPanel::VisualScriptEditorPanel(AssetHandle graph, EditorOperations& operations, VisualScriptSemantics& semantics,
	                                                 VisualScriptViewSettings& settings, EditSession& session,
	                                                 VisualScriptEditorServices const& services)
		: m_Graph(graph),
		  m_Operations(operations),
		  m_Semantics(semantics),
		  m_Settings(settings),
		  m_Session(session),
		  m_Services(services),
		  m_Model(graph, settings)
	{
		m_SceneContext.EntityName = [this](UUID entity) -> std::optional<std::string>
		{
			Scene& scene = m_Operations.GetContext().GetScene();
			Entity const found = scene.GetEntityByUUID(entity);
			if (!found)
			{
				return std::nullopt;
			}
			return found.GetName();
		};
	}

	VisualScriptEditorPanel::~VisualScriptEditorPanel() = default;

	VisualScriptEditorPanel::LiveChipData::LiveChipData() = default;

	VisualScriptEditorPanel::NodePreview::NodePreview() = default;

	UUID VisualScriptEditorPanel::GetContextEntity() const
	{
		return GetPresentContextEntity();
	}

	UUID VisualScriptEditorPanel::GetPresentContextEntity() const
	{
		// The choice is kept while its entity is gone (deleted, or not in the scene shown), so undoing the deletion brings it
		// back; meanwhile there is none.
		if (!m_ContextEntity.IsValid() || !m_Operations.GetContext().GetScene().HasEntity(m_ContextEntity))
		{
			return UUID::Invalid();
		}
		return m_ContextEntity;
	}

	std::string VisualScriptEditorPanel::GetWindowName(AssetHandle graph, std::string_view title)
	{
		return fmt::format("{}###VisualScript{}", title, graph.GetUUID().GetValue());
	}

	std::string VisualScriptEditorPanel::GetTitle() const
	{
		if (!m_Title.empty())
		{
			return m_Title;
		}
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(m_Graph);
		if (!metadata)
		{
			return "Visual Script";
		}
		if (metadata->Path.empty())
		{
			return metadata->Name.empty() ? std::string("Visual Script") : metadata->Name;
		}
		return FileSystem::PathToUtf8(FileSystem::PathFromUtf8(metadata->Path).stem());
	}

	void VisualScriptEditorPanel::Apply(VisualScriptOpenRequest const& request)
	{
		m_PendingOpen = request;
		m_FocusRequested = m_FocusRequested || request.Focus;
		if (request.ContextEntity.IsValid())
		{
			m_RequestedContext = request.ContextEntity;
		}
	}

	std::optional<VisualScriptRect> VisualScriptEditorPanel::GetItemRect(VisualScriptPanelItem item) const
	{
		size_t const index = static_cast<size_t>(item);
		return index < m_ItemRects.size() ? m_ItemRects[index] : std::nullopt;
	}

	void VisualScriptEditorPanel::RecordItem(VisualScriptPanelItem item)
	{
		ImVec2 const lower = ImGui::GetItemRectMin();
		ImVec2 const upper = ImGui::GetItemRectMax();
		m_ItemRects[static_cast<size_t>(item)] = VisualScriptRect{ToVec2(lower), ToVec2(upper)};
	}

	bool VisualScriptEditorPanel::RequestAction(VisualScriptEditorAction action)
	{
		// A hidden window takes no actions: one taken now would run whenever the window is shown again.
		if (!m_Visible)
		{
			return false;
		}
		m_RequestedActions.push_back(action);
		return true;
	}

	void VisualScriptEditorPanel::NotifyHistoryStep(bool undone, std::string const& description, AssetHandle editedAsset)
	{
		AddToast(fmt::format("{}: {}", undone ? "Undone" : "Redone", description));
		Toast& toast = m_Toasts.back();
		if (!editedAsset.IsValid())
		{
			// A step of the scene: [Show] focuses its view.
			toast.HasShow = true;
		}
		else if (editedAsset != m_Graph && AssetManager::GetAssetType(editedAsset) == AssetType::VisualScript)
		{
			toast.HasShow = true;
			toast.ShowAsset = editedAsset;
		}
	}

	bool VisualScriptEditorPanel::HasPendingWidgetEdit() const
	{
		return (m_TextEdit.has_value() && m_TextEdit->Text != m_TextEdit->Original) || m_Details.HasPendingEdit() ||
		       m_Sidebar.HasPendingEdit() || m_Drawer.HasPendingEdit() || m_ProblemCommits.HasPendingEdit() ||
		       m_LiveCommits.HasPendingEdit();
	}

	void VisualScriptEditorPanel::FlushWidgetEdit()
	{
		// Called between draws, also for hidden windows, which keep no library: the edits are resolved with the current one.
		m_Library = m_Semantics.GetLibrary();
		if (m_TextEdit)
		{
			CommitTextEdit();
		}
		// One at a time: each commit syncs the model the next pending value was computed against.
		auto const flush = [this](std::optional<VisualScriptCommitRequest> pending)
		{
			if (pending)
			{
				Commit(std::move(*pending), {});
			}
		};
		flush(m_Details.TakePendingEdit());
		flush(m_Sidebar.TakePendingEdit());
		flush(m_Drawer.TakePendingEdit());
		flush(m_ProblemCommits.TakePendingEdit());
		flush(m_LiveCommits.TakePendingEdit());
		if (!m_Visible)
		{
			m_Library.reset();
		}
	}

	Result<VisualScriptPaletteState> VisualScriptEditorPanel::OpenPalette(VisualScriptPaletteRequest const& request)
	{
		if (m_Semantics.GetLibrary() == nullptr)
		{
			return Error{"the node palette needs the scripting runtime"};
		}
		if (request.From)
		{
			VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
			std::optional<uint32_t> const node = canvas.FindNode(request.From->Node);
			if (!node)
			{
				return MakeError("the shown graph has no node {}", request.From->Node);
			}
			if (!canvas.FindPin(*node, request.From->Pin, request.Direction))
			{
				return MakeError("node {} has no {} pin {}", request.From->Node,
				                 request.Direction == VisualScriptPinDirection::In ? "input" : "output", request.From->Pin);
			}
		}
		m_Palette.Open(request);
		VisualScriptPaletteState state;
		state.Open = true;
		state.Query = request.Query;
		state.From = request.From;
		return state;
	}

	void VisualScriptEditorPanel::ClosePalette()
	{
		m_Palette.Close();
	}

	VisualScriptPaletteState VisualScriptEditorPanel::GetPaletteState() const
	{
		// The last draw's state, while the palette is still open: right after OpenPalette it is closed until it is drawn.
		if (!m_Palette.IsOpen() || !m_Palette.GetState().Open)
		{
			return {};
		}
		return m_Palette.GetState();
	}

	VisualScriptEditorState VisualScriptEditorPanel::GetState() const
	{
		VisualScriptEditorState state = m_Model.GetState();
		state.Focused = m_Focused;
		state.Visible = m_Visible;
		// As GetPaletteState gives it, from the palette's last draw.
		VisualScriptPaletteState const palette = GetPaletteState();
		state.PaletteOpen = palette.Open;
		state.PaletteQuery = palette.Query;
		if (m_HintShown)
		{
			state.Notices.push_back(VisualScriptNotice::GestureHint);
		}
		for (Toast const& toast : m_Toasts)
		{
			state.Toasts.push_back(toast.Text);
		}
		state.Save = m_SaveState;
		state.Drawer = m_DrawerMode == VisualScriptDrawerMode::Closed ? VisualScriptDrawerMode::Closed : m_Drawer.GetMode();
		state.FindOpen = m_FindBar.IsOpen();
		state.FindText = m_FindBar.GetText();
		state.FindIndex = m_FindBar.GetIndex();
		state.FindCount = m_FindBar.GetCount();
		// A pane over the canvas is an overlay, auto-hidden or turned off (My Script opened for a key or a rename).
		auto const paneState = [](VisualScriptPaneState laidOut, bool overlay)
		{
			return laidOut != VisualScriptPaneState::Shown && overlay ? VisualScriptPaneState::Overlay : laidOut;
		};
		state.Sidebar = paneState(m_Layout.SidebarState, m_SidebarOverlay);
		state.Details = paneState(m_Layout.DetailsState, m_DetailsOverlay);
		state.PanesStacked = m_Layout.Stacked;
		state.ShortcutsOpen = m_ShortcutsOpen;
		state.HiddenToolbarActions = m_HiddenToolbarActions;
		state.ContextEntity = GetPresentContextEntity();
		state.Live = m_Live;
		state.Card = m_Card;
		// Top to bottom: the notices under the toolbar, then the hints over the canvas.
		std::vector<VisualScriptNotice> notices;
		for (NoticeKind const notice : m_Notices)
		{
			switch (notice)
			{
				case NoticeKind::Degraded:
					notices.push_back(VisualScriptNotice::Degraded);
					break;
				case NoticeKind::ReadOnlyMemory:
					notices.push_back(VisualScriptNotice::ReadOnlyMemory);
					break;
				case NoticeKind::ReadOnlyFile:
					notices.push_back(VisualScriptNotice::ReadOnlyFile);
					break;
				case NoticeKind::Repaired:
					notices.push_back(VisualScriptNotice::Repaired);
					break;
				case NoticeKind::Normalization:
					notices.push_back(VisualScriptNotice::Normalization);
					break;
				case NoticeKind::NewFunction:
					notices.push_back(VisualScriptNotice::NewFunction);
					break;
			}
		}
		if (m_CanvasHintShown)
		{
			notices.push_back(VisualScriptNotice::FirstCanvas);
		}
		notices.insert(notices.end(), state.Notices.begin(), state.Notices.end());
		state.Notices = std::move(notices);
		return state;
	}

	VisualScriptTextMeasurer VisualScriptEditorPanel::MakeMeasurer() const
	{
		// Measured once at the window's font size and scaled linearly to the size asked for, which is exact for the editor's
		// fonts. The captures are a pointer and a float, so the copy the model makes at every Sync does not allocate.
		ImFont* const font = ImGui::GetFont();
		float const base = ImGui::GetFontSize();
		VisualScriptTextMeasurer measure;
		measure.Measure = [font, base](std::string_view text, float sizeCu)
		{
			if (text.empty() || base <= 0.0f)
			{
				return 0.0f;
			}
			return font->CalcTextSizeA(base, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x * sizeCu / base;
		};
		uint64_t signature = 0xCBF29CE484222325ull;
		signature = HashBits(signature, reinterpret_cast<uintptr_t>(font));
		signature = HashBits(signature, std::bit_cast<uint32_t>(base));
		signature = HashBits(signature, std::bit_cast<uint32_t>(ImGui::GetStyle().FontScaleDpi));
		measure.Signature = signature;
		return measure;
	}

	void VisualScriptEditorPanel::SyncModel()
	{
		// While a widget of the panes or the live widget is in use, the analysis keeps its pace (spec §6.3).
		m_Model.Sync(m_Semantics, m_Measure, m_Theme, m_WidgetActive, m_Time);
	}

	void VisualScriptEditorPanel::OnHidden()
	{
		m_Visible = false;
		m_Focused = false;
		m_FocusedPart = VisualScriptWindowPart::None;
		m_CanvasHovered = false;
		m_HintShown = false;
		m_CanvasHintShown = false;
		m_ItemRects.fill(std::nullopt);
		m_Model.CancelGesture();
		m_Renderer.StopAnimations();
		m_Menu = {};
		m_ActiveChip.reset();
		m_LivePopup.reset();
		m_Problems.reset();
		m_PopupProblems.clear();
		m_BadgePress.reset();
		m_WidgetActive = false;
		m_RequestedActions.clear();
		// A hidden window keeps no node library alive (a rebuilt library's old copy goes), nor a preview laid out with it.
		m_Library.reset();
		m_NodePreview.reset();
		m_LiveChipData.reset();
		m_LivePopupData.reset();
	}

	void VisualScriptEditorPanel::OnImGuiRender(bool& open, std::optional<ImGuiID> dockTarget)
	{
		std::string const title = GetTitle();
		m_ScriptName = title;
		std::string const name = GetWindowName(m_Graph, title);
		if (dockTarget)
		{
			ImGui::SetNextWindowDockID(*dockTarget, ImGuiCond_FirstUseEver);
		}
		// The window is focused in the frame after it exists (spec §8.1 rule 6).
		if (m_FocusRequested && ImGui::FindWindowByName(name.c_str()) != nullptr)
		{
			ImGui::SetNextWindowFocus();
			m_FocusRequested = false;
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool const visible = ImGui::Begin(name.c_str(), &open, WindowFlags);
		ImGui::PopStyleVar();
		if (!visible)
		{
			ImGui::End();
			OnHidden();
			return;
		}
		m_Visible = true;
		m_Time = ImGui::GetTime();
		m_ItemRects.fill(std::nullopt);
		std::erase_if(m_Toasts,
		              [this](Toast const& toast)
		              {
						  return toast.Expires && *toast.Expires <= m_Time;
					  });

		m_FrameActions.clear();
		m_PanesWidgetActive = false;
		ClaimKeys();
		// The Edit menu's actions are routed as their keys would be.
		for (VisualScriptEditorAction const action : std::exchange(m_RequestedActions, {}))
		{
			if (IsRowAction(action) && m_PaneShared.Element)
			{
				RouteRowAction(action);
			}
			else
			{
				m_FrameActions.push_back(action);
			}
		}

		m_Library = m_Semantics.GetLibrary();
		m_Measure = MakeMeasurer();
		m_Theme = MakeVisualScriptThemeAnchors(ImGui::GetStyle());
		m_SceneContext.Playing = m_Operations.GetContext().IsPlaying();
		SyncModel();
		if (m_PendingOpen)
		{
			VisualScriptOpenRequest const request = std::move(*m_PendingOpen);
			m_PendingOpen.reset();
			m_Model.ApplyOpenRequest(request);
			if (request.ShowProblems)
			{
				ShowDrawer(VisualScriptDrawerMode::Problems);
			}
			if (!request.Find.empty())
			{
				// The first match becomes the selection at the bar's draw (spec §13.2).
				m_FindBar.Open(request.Find, true);
			}
		}
		// A run of a creation and its name ends when another change replaced the document its last commit wrote.
		if (m_NameRun && m_Model.GetStoredDocument() != m_NameRun->Written)
		{
			m_NameRun.reset();
		}
		UpdateWindowState();

		DrawToolbar();
		DrawNotices();
		DrawBody();
		DrawMenus();
		DrawShortcutSheet();
		// The links of commits made after the canvas was drawn (a menu pick, the palette, a pane) pulse from the next draw: the
		// model reports them only until the next frame's first Sync, before that draw.
		if (std::span<VisualScriptLink const> const committed = m_Model.CommittedLinks(); committed.size() > m_PulsedLinks)
		{
			m_Renderer.StartPulses(committed.subspan(m_PulsedLinks), m_Time);
		}
		ImGui::End();
		// The analysis of the next frame's Sync keeps its pace while a widget is in use.
		m_WidgetActive = m_PanesWidgetActive || m_LiveActive;
		if (m_CloseRequested)
		{
			m_CloseRequested = false;
			open = false;
		}
	}

	void VisualScriptEditorPanel::ClaimKeys()
	{
		ImGuiContext const& g = *GImGui;
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		ImGuiWindow const* nav = g.NavWindow;
		m_Focused = nav != nullptr && nav->RootWindow == window;
		m_FocusedPart = VisualScriptWindowPart::None;
		if (m_Focused)
		{
			// The window itself counts as the canvas (spec §8.1 rule 4): a focused graph window's keys go to its canvas.
			if (nav == window || nav->ID == m_CanvasWindow)
			{
				m_FocusedPart = VisualScriptWindowPart::Canvas;
			}
			else if (nav->ID == m_SidebarWindow)
			{
				m_FocusedPart = VisualScriptWindowPart::Sidebar;
			}
			else if (nav->ID == m_DetailsWindow)
			{
				m_FocusedPart = VisualScriptWindowPart::Details;
			}
		}
		m_WindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
		m_CanvasOwner = ImGui::GetID("##canvasKeys");

		ImGuiIO const& io = ImGui::GetIO();
		bool const canvasKeys = m_FocusedPart == VisualScriptWindowPart::Canvas && !io.WantTextInput;
		// While a popup of the window has the keyboard (a menu, a combo, the palette, My Script's delete modal), the window's
		// focused route scores nothing (imgui.cpp:10402-10412) and the editor's global one would win: the window keeps its keys
		// with a global route over the focused ones (imgui.cpp:10420-10428), which an active text field still beats, and does
		// nothing with them, as menus do nothing with the keys their items show.
		bool const popupFocused = IsPopupOf(nav, window);
		ImGuiInputFlags const route =
			popupFocused ? ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverFocused : ImGuiInputFlags_RouteFocused;
		// Class 1 (spec §8.3): claimed every frame the window is drawn, even with nothing selected, because routes decide the
		// next frame: a frame without them would let Delete reach the editor's editing keys, which act on the Edit menu's target.
		for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
		{
			if (shortcut.Route != VisualScriptKeyRoute::Focused)
			{
				continue;
			}
			ImGuiInputFlags const flags = route | (shortcut.Repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None);
			if (ImGui::Shortcut(shortcut.Chord, flags) && m_Focused && !io.WantTextInput)
			{
				RouteFocusedAction(shortcut.Action);
			}
		}
		// An overlay pane closes on Escape while it has the keyboard too (spec §10.1 rule 2): Escape is owned there as on the
		// canvas, else ImGui's navigation cancel would only move the keyboard to the window (imgui.cpp:15190-15198).
		bool const overlayKeys = ((m_FocusedPart == VisualScriptWindowPart::Sidebar && m_SidebarOverlay) ||
		                          (m_FocusedPart == VisualScriptWindowPart::Details && m_DetailsOverlay)) &&
		                         !io.WantTextInput;
		if (overlayKeys)
		{
			ImGui::SetKeyOwner(ImGuiKey_Escape, m_CanvasOwner);
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, m_CanvasOwner))
			{
				m_SidebarOverlay = false;
				m_DetailsOverlay = false;
				// The keyboard goes back to the canvas.
				if (ImGuiWindow* canvas = ImGui::FindWindowByID(m_CanvasWindow); canvas != nullptr)
				{
					ImGui::FocusWindow(canvas);
				}
			}
			return;
		}
		if (!canvasKeys)
		{
			return;
		}

		// Class 2: matched exactly, modifiers included, under the canvas's key owner. While Space is held, single keys type the
		// palette's query instead (the model reads the characters).
		bool const spaceDown = ImGui::IsKeyDown(ImGuiKey_Space);
		for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
		{
			if (shortcut.Route != VisualScriptKeyRoute::Canvas)
			{
				continue;
			}
			if (spaceDown && (shortcut.Chord & ImGuiMod_Mask_) == 0)
			{
				continue;
			}
			if (ImGui::IsKeyChordPressed(shortcut.Chord, shortcut.Repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None, m_CanvasOwner))
			{
				m_FrameActions.push_back(shortcut.Action);
			}
		}

		// Escape belongs to the canvas: with keyboard navigation ImGui's cancel acts on every Escape nobody owns, which would
		// end the surface's capture during a gesture or move the focus from the canvas to the window (spec §8.1 rule 5).
		// Ownership set now is current at the next frame's navigation update.
		ImGui::SetKeyOwner(ImGuiKey_Escape, m_CanvasOwner);
		if (!ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, m_CanvasOwner))
		{
			return;
		}
		// The order of spec §8.3: the gesture; else an overlay pane; else the find bar; else the selection.
		if (m_Model.GetGesture().Kind != VisualScriptGesture::Idle)
		{
			m_FrameActions.push_back(Action::Cancel);
		}
		else if (m_SidebarOverlay || m_DetailsOverlay)
		{
			m_SidebarOverlay = false;
			m_DetailsOverlay = false;
		}
		else if (m_FindBar.IsOpen())
		{
			m_FindBar.Close();
		}
		else
		{
			m_FrameActions.push_back(Action::Cancel);
		}
	}

	void VisualScriptEditorPanel::RouteFocusedAction(VisualScriptEditorAction action)
	{
		// Find opens the bar or the drawer whichever part has the keyboard.
		if (action == Action::Find || action == Action::FindInProject)
		{
			HandlePanelAction(action);
			return;
		}
		// The row keys act on the row My Script selected, which Details shows, wherever the window's keyboard is (ruling R7.3):
		// the canvas has nothing selected then. A key My Script does not take (a read-only document) is still consumed.
		if (IsRowAction(action) && m_PaneShared.Element)
		{
			RouteRowAction(action);
			return;
		}
		switch (m_FocusedPart)
		{
			case VisualScriptWindowPart::Sidebar:
				// The other keys mean nothing in My Script.
				break;
			case VisualScriptWindowPart::Details:
				// The element Details shows: a My Script row's (whose other keys mean nothing), else the canvas's selection.
				if (!m_PaneShared.Element)
				{
					m_FrameActions.push_back(action);
				}
				break;
			case VisualScriptWindowPart::Canvas:
			case VisualScriptWindowPart::None:
				// The canvas, or a part of the window that has no keys of its own (the find bar, the drawer): the keys act on the
				// canvas's selection, which a find or a jump made (spec §11.6 rule 1).
				m_FrameActions.push_back(action);
				break;
		}
	}

	void VisualScriptEditorPanel::RouteRowAction(VisualScriptEditorAction action)
	{
		// My Script does it at its next Draw, in this frame: a My Script that is not beside the canvas (turned off, or
		// auto-hidden) opens over it, where its rename field or its delete modal shows.
		if (m_Sidebar.HandleAction(action, m_PaneShared))
		{
			OpenSidebarOverlay();
		}
	}

	void VisualScriptEditorPanel::OpenSidebarOverlay()
	{
		if (m_Layout.SidebarState != VisualScriptPaneState::Shown)
		{
			m_SidebarOverlay = true;
		}
	}

	void VisualScriptEditorPanel::ShowDrawer(VisualScriptDrawerMode mode)
	{
		if (mode == VisualScriptDrawerMode::Problems)
		{
			m_Drawer.ShowProblems();
		}
		m_DrawerMode = mode;
	}

	void VisualScriptEditorPanel::UpdateWindowState()
	{
		// Ctrl+S saves the scene (the editor's own chord, which graph windows never claim, so it is read here without taking
		// it): edits save themselves, and the save status flashes to say so.
		if (m_Focused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
		{
			m_SaveFlashStart = m_Time;
			m_Activity.SaveFlashes++;
		}
		// A gesture or key that needs the scripting runtime was refused: the degraded notice blinks once.
		if (m_Model.GetRefusedCount() > m_SeenRefusals)
		{
			m_BlinkStart = m_Time;
			m_Activity.DegradedBlinks++;
		}
		m_SeenRefusals = m_Model.GetRefusedCount();
		UpdateContextEntity();
		UpdateLiveState();
		UpdateNotices();
		ReportOutsideChange();
	}

	bool VisualScriptEditorPanel::RunsGraph(UUID entity) const
	{
		if (!entity.IsValid())
		{
			return false;
		}
		Scene& scene = m_Operations.GetContext().GetScene();
		if (!scene.HasEntity(entity))
		{
			return false;
		}
		Entity const found = scene.GetEntityByUUID(entity);
		return found.HasComponent<VisualScriptComponent>() && found.GetComponent<VisualScriptComponent>().Graph == m_Graph;
	}

	void VisualScriptEditorPanel::SetContextEntity(UUID entity, bool chosen)
	{
		m_ContextEntity = entity;
		m_ContextChosen = chosen;
		if (chosen)
		{
			// Window state, kept per graph in the views file (spec §6.13).
			m_Settings.SetContextEntity(m_Graph, entity);
		}
	}

	void VisualScriptEditorPanel::UpdateContextEntity()
	{
		// The entities of the scene shown (the running copy while playing, which has the same IDs) that run this graph.
		Scene& scene = m_Operations.GetContext().GetScene();
		uint32_t running = 0;
		UUID only = UUID::Invalid();
		for (entt::entity const handle : scene.GetAllEntitiesWith<VisualScriptComponent>())
		{
			Entity const entity(handle, &scene);
			if (entity.GetComponent<VisualScriptComponent>().Graph == m_Graph)
			{
				running++;
				only = entity.GetUUID();
			}
		}
		// The open request's entity, else the one the views file kept for the graph, else the only entity that runs it.
		if (m_RequestedContext)
		{
			SetContextEntity(*m_RequestedContext, true);
			m_RequestedContext.reset();
			m_ContextResolved = true;
		}
		else if (!m_ContextResolved)
		{
			m_ContextResolved = true;
			VisualScriptGraphViews const* views = m_Settings.FindGraph(m_Graph);
			if (views != nullptr && RunsGraph(views->ContextEntity))
			{
				SetContextEntity(views->ContextEntity, true);
			}
		}
		if (!m_ContextChosen)
		{
			m_ContextEntity = running == 1 ? only : UUID::Invalid();
		}
		// The combo is shown while it has something to choose: an entity runs the graph, or one is chosen.
		m_InstanceShown = running > 0 || m_ContextEntity.IsValid();
	}

	void VisualScriptEditorPanel::UpdateLiveState()
	{
		EditorContext const& context = m_Operations.GetContext();
		switch (context.GetPlayState())
		{
			case EditorPlayState::Edit:
				m_Live = VisualScriptLiveState::None;
				break;
			case EditorPlayState::Play:
			{
				// With errors, the game keeps running the last version that compiled, when it has one.
				uint32_t const errors = CountProblems(m_Model.GetAnalysis(), std::nullopt).first;
				m_Live = errors > 0 && VisualScriptEngine::HasWorkingProgram(m_Graph) ? VisualScriptLiveState::LastWorking
				                                                                      : VisualScriptLiveState::Live;
				break;
			}
			case EditorPlayState::Simulate:
				m_Live = VisualScriptLiveState::Simulating;
				break;
		}
		// The context entity's running values, read at most VisualScriptLiveValueInterval apart: My Script shows them.
		UUID const entity = GetPresentContextEntity();
		if (m_Live == VisualScriptLiveState::None || m_Live == VisualScriptLiveState::Simulating || !entity.IsValid() ||
		    !VisualScriptEngine::HasInstance(entity))
		{
			m_LiveValues.reset();
			m_LiveRead = -1.0e30;
			return;
		}
		if (m_Time - m_LiveRead < VisualScriptLiveValueInterval)
		{
			return;
		}
		m_LiveRead = m_Time;
		m_Activity.LiveValueReads++;
		Result<ScriptFieldMap> values = VisualScriptEngine::GetInstanceVariables(entity);
		if (values)
		{
			m_LiveValues = values.TakeValue();
		}
		else
		{
			m_LiveValues.reset();
		}
	}

	void VisualScriptEditorPanel::UpdateNotices()
	{
		m_Notices.clear();
		m_Card = VisualScriptCard::None;
		VisualScriptDocumentState const state = m_Model.GetDocumentState();
		if (state == VisualScriptDocumentState::Missing)
		{
			m_Card = VisualScriptCard::Missing;
		}
		else if (state == VisualScriptDocumentState::Unloadable)
		{
			// A failed load is remembered by the asset manager, so asking again costs nothing and logs nothing.
			Result<Ref<VisualScriptAsset>> const asset = AssetManager::TryGetAsset<VisualScriptAsset>(m_Graph);
			m_Card = VisualScriptCard::Unloadable;
			m_LoadError = asset ? std::string() : asset.GetError();
		}
		if (state == VisualScriptDocumentState::Missing || state == VisualScriptDocumentState::Unloadable ||
		    state == VisualScriptDocumentState::Gone)
		{
			m_FunctionHint.reset();
			return;
		}

		if (m_Library == nullptr)
		{
			m_Notices.push_back(NoticeKind::Degraded);
		}
		if (state == VisualScriptDocumentState::ReadOnly)
		{
			m_Notices.push_back(NoticeKind::ReadOnlyMemory);
		}
		else if (state == VisualScriptDocumentState::ReadOnlyFile)
		{
			m_Notices.push_back(NoticeKind::ReadOnlyFile);
		}
		// A file that was repaired while loading, said once per load: a reload makes a new asset object. The asset loaded is
		// known by identity only, never kept alive (an expired one is another asset).
		if (Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(m_Graph); asset != m_RepairAsset.lock())
		{
			m_RepairAsset = asset;
			m_Repairs = asset != nullptr ? asset->GetLoadRepairCount() : 0;
			m_RepairNotice = m_Repairs > 0;
		}
		if (m_RepairNotice)
		{
			m_Notices.push_back(NoticeKind::Repaired);
		}
		UpdateNormalizationCount();
		if (m_Normalizations > 0 && !m_Model.IsReadOnly())
		{
			m_Notices.push_back(NoticeKind::Normalization);
		}
		// The new function's hint: once per function and window, from when a function holding only its Entry and Return is
		// shown until it is dismissed or the window shows another sub-graph.
		uint32_t const function = m_Model.GetFunction();
		if (m_FunctionHint && *m_FunctionHint != function)
		{
			m_FunctionHintsSeen.push_back(*m_FunctionHint);
			m_FunctionHint.reset();
		}
		if (!m_FunctionHint && function != 0 &&
		    std::find(m_FunctionHintsSeen.begin(), m_FunctionHintsSeen.end(), function) == m_FunctionHintsSeen.end() &&
		    IsShownFunctionNew())
		{
			m_FunctionHint = function;
		}
		if (m_FunctionHint)
		{
			m_Notices.push_back(NoticeKind::NewFunction);
		}
		if (IsShownGraphEmpty())
		{
			m_Card = VisualScriptCard::EmptyGraph;
		}
	}

	void VisualScriptEditorPanel::UpdateNormalizationCount()
	{
		// Counted again only when the document or the analysis changed.
		Ref<VisualScriptDocument const> const& document = m_Model.GetStoredDocument();
		VisualScriptAnalysis const* analysis = m_Model.GetAnalysis();
		uint64_t const analysisKey = analysis != nullptr ? m_Model.GetAnalysisCount() + 1 : 0;
		if (document == m_NormalizedDocument && analysisKey == m_NormalizedAnalysis)
		{
			return;
		}
		m_NormalizedDocument = document;
		m_NormalizedAnalysis = analysisKey;
		m_Normalizations = 0;
		if (document == nullptr)
		{
			return;
		}
		// Nodes the analysis resolved through a redirect or as the only fitting overload, and nodes without a position: the
		// next commit writes their canonical types and positions (spec §4.2). Node IDs are unique in a document.
		std::vector<uint32_t> inexact;
		if (analysis != nullptr)
		{
			for (VisualScriptAnalyzedNode const& node : analysis->Nodes)
			{
				if (node.Resolved == VisualScriptResolution::Redirect || node.Resolved == VisualScriptResolution::Unique)
				{
					inexact.push_back(node.ID);
				}
			}
			std::sort(inexact.begin(), inexact.end());
		}
		m_Normalizations = static_cast<uint32_t>(inexact.size());
		auto const count = [&](VisualScriptGraph const& graph)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				if (!node.Position && !std::binary_search(inexact.begin(), inexact.end(), node.ID))
				{
					m_Normalizations++;
				}
			}
		};
		count(document->Graph);
		for (VisualScriptFunction const& function : document->Functions)
		{
			count(function.Graph);
		}
	}

	void VisualScriptEditorPanel::ReportOutsideChange()
	{
		// The elements an outside change made or changed flash where they are; when none of them is in view, a toast says so
		// and its [Show] frames them (spec §10.7).
		std::span<uint32_t const> const changed = m_Model.ChangedElements();
		if (changed.empty() || m_CanvasSize.x <= 0.0f || m_CanvasSize.y <= 0.0f)
		{
			return;
		}
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		VisualScriptRect const visible = m_Model.GetView().GetVisibleRect();
		size_t nodes = 0;
		size_t comments = 0;
		for (uint32_t const element : changed)
		{
			std::optional<VisualScriptRect> rect;
			if (std::optional<uint32_t> const node = canvas.FindNode(element))
			{
				rect = VisualScriptGeometry::GetNodeRect(canvas.GetNodes()[*node]);
				nodes++;
			}
			else if (std::optional<uint32_t> const comment = canvas.FindComment(element))
			{
				rect = canvas.GetComments()[*comment].Rect;
				comments++;
			}
			if (rect && VisualScriptGeometry::Intersects(*rect, visible))
			{
				return;
			}
		}
		if (nodes + comments == 0)
		{
			return;
		}
		AddToast(fmt::format("{} changed outside the view", CountElements(nodes, comments)));
		Toast& toast = m_Toasts.back();
		toast.HasShow = true;
		toast.ShowElements.assign(changed.begin(), changed.end());
		toast.ShowFunction = m_Model.GetFunction();
	}

	void VisualScriptEditorPanel::FrameElements(std::span<uint32_t const> elements)
	{
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		std::optional<VisualScriptRect> bounds;
		auto const include = [&bounds](VisualScriptRect const& rect)
		{
			bounds = bounds ? VisualScriptRect{glm::min(bounds->Min, rect.Min), glm::max(bounds->Max, rect.Max)} : rect;
		};
		for (uint32_t const element : elements)
		{
			if (std::optional<uint32_t> const node = canvas.FindNode(element))
			{
				include(VisualScriptGeometry::GetNodeRect(canvas.GetNodes()[*node]));
			}
			else if (std::optional<uint32_t> const comment = canvas.FindComment(element))
			{
				include(canvas.GetComments()[*comment].Rect);
			}
		}
		if (!bounds)
		{
			return;
		}
		// Centred and, when they do not fit at the current zoom, zoomed out until they do; the selection stays.
		float const scale = std::max(ImGui::GetStyle().FontScaleDpi, 0.01f);
		glm::vec2 const room = glm::max(m_CanvasSize - glm::vec2(2.0f * FrameMargin * scale), glm::vec2(1.0f));
		glm::vec2 const size = glm::max(bounds->GetSize(), glm::vec2(1.0f));
		float const fit = std::min(room.x / size.x, room.y / size.y) / scale;
		VisualScriptOpenRequest request;
		request.Graph = m_Graph;
		request.Function = m_Model.GetFunction();
		request.Frame = VisualScriptFrame::Keep;
		request.Zoom = std::clamp(std::min(fit, m_Model.GetViewState().Zoom), VisualScriptMinZoom, VisualScriptMaxZoom);
		request.Center = bounds->GetCenter();
		request.Focus = false;
		m_Model.ApplyOpenRequest(request);
	}

	bool VisualScriptEditorPanel::IsShownGraphEmpty() const
	{
		if (m_Model.GetFunction() != 0 || m_Model.GetStoredDocument() == nullptr)
		{
			return false;
		}
		VisualScriptGraph const& graph = m_Model.GetDisplayDocument().Graph;
		return graph.Nodes.empty() && graph.Comments.empty();
	}

	bool VisualScriptEditorPanel::IsShownFunctionNew() const
	{
		VisualScriptGraph const* graph = FindVisualScriptGraph(m_Model.GetDisplayDocument(), m_Model.GetFunction());
		if (graph == nullptr || !graph->Comments.empty())
		{
			return false;
		}
		return std::all_of(graph->Nodes.begin(), graph->Nodes.end(),
		                   [](VisualScriptNode const& node)
		                   {
							   return node.Type == "Function.Entry" || node.Type == "Function.Return";
						   });
	}

	void VisualScriptEditorPanel::DrawToolbar()
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const fontSize = ImGui::GetFontSize();
		float const frame = ImGui::GetFrameHeight();
		float const spacing = style.ItemSpacing.x;
		ImVec2 const origin = ImGui::GetCursorScreenPos();
		float const inner = ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f;
		float const toolbarHeight = frame + style.WindowPadding.y * 2.0f;
		float const left = origin.x + style.WindowPadding.x;
		float const top = origin.y + style.WindowPadding.y;

		VisualScriptDocument const& document = m_Model.GetDisplayDocument();
		uint32_t const function = m_Model.GetFunction();
		std::string const scriptText = m_ScriptName;
		std::string const graphText = function == 0 ? std::string("Event Graph") : [&]
		{
			for (VisualScriptFunction const& candidate : document.Functions)
			{
				if (candidate.ID == function)
				{
					return candidate.Name;
				}
			}
			return std::string("Event Graph");
		}();
		auto const textWidth = [](std::string_view text)
		{
			return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
		};
		auto const textButtonWidth = [&](std::string_view text)
		{
			return textWidth(text) + style.FramePadding.x * 2.0f;
		};
		float const chevron = fontSize * 0.8f;
		float const breadcrumbFull = textButtonWidth(scriptText) + chevron + textButtonWidth(graphText) + frame;
		float const minimumText = VisualScriptBreadcrumbMinWidth * fontSize;
		std::string const instanceText = [&]
		{
			if (!m_ContextEntity.IsValid())
			{
				return std::string("Instance: None");
			}
			Scene& scene = m_Operations.GetContext().GetScene();
			return scene.HasEntity(m_ContextEntity) ? "Instance: " + scene.GetEntityByUUID(m_ContextEntity).GetName()
			                                        : std::string("Instance: None");
		}();
		float const instanceFull = m_InstanceShown ? textWidth(instanceText) + style.FramePadding.x * 2.0f + frame : 0.0f;
		VisualScriptEditorState const modelState = m_Model.GetState();
		std::string const zoomText = fmt::format("{}%", static_cast<int>(std::lround(modelState.View.Zoom * 100.0f)));
		std::string const saveText = m_SaveState == VisualScriptSaveState::Saved ? "Saved" : "Not saved";
		auto const [errors, warnings] = CountProblems(m_Model.GetAnalysis(), std::nullopt);
		std::string const errorCount = fmt::format("{}", errors);
		std::string const warningCount = fmt::format("{}", warnings);
		float const badge = fontSize * 0.72f;
		// The counts with their glyphs; "No problems" when there are none, "Not analyzed" without the scripting runtime (spec
		// §11.5).
		std::string_view const problemsText = !modelState.Analyzed           ? Texts::NotAnalyzed
		                                      : errors == 0 && warnings == 0 ? Texts::NoProblems
		                                                                     : std::string_view();
		float const problemsWidth = !problemsText.empty() ? textButtonWidth(problemsText)
		                                                  : style.FramePadding.x * 2.0f + badge * 2.0f + style.ItemInnerSpacing.x * 2.0f +
		                                                        textWidth(errorCount) + textWidth(warningCount) + style.ItemSpacing.x;
		std::string liveText;
		std::string liveShort;
		switch (m_Live)
		{
			case VisualScriptLiveState::None:
				break;
			case VisualScriptLiveState::Live:
				liveText = Texts::Live;
				liveShort = Texts::Live;
				break;
			case VisualScriptLiveState::LastWorking:
				liveText = Texts::LastWorking;
				liveShort = Texts::Live;
				break;
			case VisualScriptLiveState::Simulating:
				liveText = Texts::SimulatingFull;
				liveShort = Texts::Simulating;
				break;
		}

		// The overflow of spec §10.1 rule 1, by stages: 0 everything as text; 1 the text buttons become glyphs (and the pill
		// keeps its first word, its tooltip the rest); 2 the zoom, Bookmarks, Snap to Grid and the minimap move into the view
		// menu; then the breadcrumb and the instance combo shrink (the script's name first), each to at least 8 font sizes. The
		// pane toggles, Back and Forward, the pills, the view menu and "?" never hide.
		auto const rightWidth = [&](int stage)
		{
			float width = problemsWidth + frame * 3.0f;
			int items = 4;
			if (!liveText.empty())
			{
				width += textButtonWidth(stage >= 1 ? liveShort : liveText);
				items++;
			}
			if (stage >= 1)
			{
				width += frame * 3.0f;
			}
			else
			{
				width += textButtonWidth(saveText) + textButtonWidth("Arrange") + textButtonWidth("Find");
			}
			items += 3;
			if (stage < 2)
			{
				width += (stage >= 1 ? frame : textButtonWidth("Bookmarks")) + frame * 2.0f + textButtonWidth("200%");
				items += 4;
			}
			return width + spacing * static_cast<float>(items - 1);
		};
		float const leftWidth = frame * 3.0f + spacing * 3.0f;
		float const middleFull = breadcrumbFull + (m_InstanceShown ? spacing + instanceFull : 0.0f);
		int stage = 0;
		while (stage < 2 && leftWidth + middleFull + spacing + rightWidth(stage) > inner)
		{
			stage++;
		}
		bool const glyphs = stage >= 1;
		bool const viewInMenu = stage >= 2;
		float const right = rightWidth(stage);
		float const middle = std::max(inner - leftWidth - spacing - right - (m_InstanceShown ? spacing : 0.0f), 0.0f);
		float breadcrumbWidth = breadcrumbFull;
		float instanceWidth = instanceFull;
		if (breadcrumbWidth + instanceWidth > middle)
		{
			// The breadcrumb shrinks first, then the instance combo, each to its minimum; below that both share what is left.
			float const instanceMinimum = m_InstanceShown ? std::min(instanceFull, minimumText) : 0.0f;
			breadcrumbWidth = std::max(middle - instanceWidth, std::min(breadcrumbFull, minimumText));
			instanceWidth = std::max(middle - breadcrumbWidth, instanceMinimum);
			if (breadcrumbWidth + instanceWidth > middle)
			{
				float const share = middle / std::max(breadcrumbWidth + instanceWidth, 1.0f);
				breadcrumbWidth *= share;
				instanceWidth *= share;
			}
		}
		m_HiddenToolbarActions.clear();
		if (viewInMenu)
		{
			m_HiddenToolbarActions = {Action::ToggleSnapToGrid, Action::ToggleMinimap, Action::ZoomReset};
			// The Bookmarks menu, in the view menu: going to each stored bookmark and storing the next free slot.
			VisualScriptGraphViews const* views = m_Settings.FindGraph(m_Graph);
			std::optional<uint8_t> freeSlot;
			for (uint8_t slot = 1; slot <= VisualScriptViewSettings::MaxBookmarkSlot; slot++)
			{
				bool const stored = views != nullptr && views->FindBookmark(slot) != nullptr;
				if (stored)
				{
					m_HiddenToolbarActions.push_back(static_cast<Action>(static_cast<uint8_t>(Action::GoToBookmark1) + slot - 1));
				}
				else if (!freeSlot)
				{
					freeSlot = slot;
				}
			}
			if (freeSlot)
			{
				m_HiddenToolbarActions.push_back(static_cast<Action>(static_cast<uint8_t>(Action::StoreBookmark1) + *freeSlot - 1));
			}
		}
		m_ViewItemsInMenu = viewInMenu;

		VisualScriptPaneSettings panes = m_Settings.GetPanes();
		ImU32 const textColor = ImGui::GetColorU32(ImGuiCol_Text);
		ImGui::PushID("##toolbar");
		ImGui::SetCursorScreenPos(ImVec2(left, top));
		if (GlyphButton("##sidebar", Glyph::Sidebar, m_Layout.SidebarState != VisualScriptPaneState::Hidden, true, "My Script"))
		{
			if (m_Layout.SidebarState == VisualScriptPaneState::AutoHidden)
			{
				m_SidebarOverlay = !m_SidebarOverlay;
			}
			else
			{
				panes.ShowSidebar = !panes.ShowSidebar;
				m_Settings.SetPanes(panes);
			}
		}
		RecordItem(VisualScriptPanelItem::SidebarToggle);
		ImGui::SameLine();
		if (GlyphButton("##back", Glyph::Back, false, m_Model.CanGoBack(), WithShortcut("Back", Action::Back)))
		{
			m_FrameActions.push_back(Action::Back);
		}
		RecordItem(VisualScriptPanelItem::Back);
		ImGui::SameLine();
		if (GlyphButton("##forward", Glyph::Forward, false, m_Model.CanGoForward(), WithShortcut("Forward", Action::Forward)))
		{
			m_FrameActions.push_back(Action::Forward);
		}
		RecordItem(VisualScriptPanelItem::Forward);
		ImGui::SameLine();

		// The breadcrumb: the script (its event graph), the sub-graph shown, and the list of every sub-graph. Shortened from
		// the script's name first.
		{
			ImVec2 const breadcrumbMin = ImGui::GetCursorScreenPos();
			float const listWidth = frame;
			float const graphNeeded = textButtonWidth(graphText);
			float const textRoom = std::max(breadcrumbWidth - chevron - listWidth, 0.0f);
			float const scriptRoom = std::max(textRoom - graphNeeded, std::min(textRoom * 0.5f, fontSize * 2.0f));
			std::string const scriptShown = FitText(scriptText, std::max(scriptRoom - style.FramePadding.x * 2.0f, 0.0f));
			float const graphRoom = std::max(textRoom - textButtonWidth(scriptShown), 0.0f);
			std::string const graphShown = FitText(graphText, std::max(graphRoom - style.FramePadding.x * 2.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
			if (ImGui::Button(fmt::format("{}##script", scriptShown).c_str()))
			{
				m_Model.ShowFunction(0);
			}
			ImGui::SetItemTooltip("%s", fmt::format("{}: show its event graph", scriptText).c_str());
			ImGui::SameLine(0.0f, 0.0f);
			ImVec2 const chevronMin = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(chevron, frame));
			DrawGlyph(ImGui::GetWindowDrawList(), Glyph::Forward, ImVec2(chevronMin.x + chevron * 0.5f, chevronMin.y + frame * 0.5f),
			          fontSize * 0.5f, ImGui::GetColorU32(ImGuiCol_TextDisabled));
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::Button(fmt::format("{}##graph", graphShown).c_str());
			ImGui::SetItemTooltip("%s", graphText.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, 0.0f);
			if (GlyphButton("##subGraphs", Glyph::ListArrow, false, true, "Sub-graphs of the script"))
			{
				ImGui::OpenPopup("##subGraphs");
			}
			m_ItemRects[static_cast<size_t>(VisualScriptPanelItem::Breadcrumb)] =
				VisualScriptRect{ToVec2(breadcrumbMin), ToVec2(ImGui::GetItemRectMax())};
			DrawSubGraphMenu();
		}
		if (m_InstanceShown)
		{
			ImGui::SameLine();
			DrawInstanceCombo(instanceWidth);
		}

		// The right group, laid out from the window's right edge.
		ImGui::SameLine();
		float const rightStart = left + inner - right;
		ImGui::SetCursorScreenPos(ImVec2(std::max(rightStart, ImGui::GetCursorScreenPos().x), top));
		if (!liveText.empty())
		{
			DrawLivePill(textButtonWidth(glyphs ? liveShort : liveText), glyphs ? liveShort : liveText);
			ImGui::SameLine();
		}

		// The problems pill: the analysis' errors and warnings in the whole script; a click opens the problems drawer.
		{
			ImVec2 const pillMin = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton("##problems", ImVec2(problemsWidth, frame)))
			{
				bool const shown = m_DrawerMode != VisualScriptDrawerMode::Closed && m_Drawer.GetMode() == VisualScriptDrawerMode::Problems;
				ShowDrawer(shown ? VisualScriptDrawerMode::Closed : VisualScriptDrawerMode::Problems);
			}
			RecordItem(VisualScriptPanelItem::Problems);
			ImGui::SetItemTooltip("%s", modelState.Analyzed ? fmt::format("Problems: {} errors, {} warnings", errors, warnings).c_str()
			                                                : "Problems are found with the scripting runtime");
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			if (ImGui::IsItemHovered())
			{
				drawList->AddRectFilled(pillMin, ImVec2(pillMin.x + problemsWidth, pillMin.y + frame),
				                        ImGui::GetColorU32(ImGuiCol_ButtonHovered), style.FrameRounding);
			}
			if (!problemsText.empty())
			{
				drawList->AddText(ImVec2(pillMin.x + style.FramePadding.x, pillMin.y + style.FramePadding.y),
				                  ImGui::GetColorU32(ImGuiCol_TextDisabled), problemsText.data(),
				                  problemsText.data() + problemsText.size());
			}
			else
			{
				DrawProblemCounts(drawList, ToVec2(pillMin), badge, errors, warnings);
			}
		}
		ImGui::SameLine();

		// The save status: edits save themselves, so it reports a write that failed; Ctrl+S makes it flash.
		{
			bool const saved = m_SaveState == VisualScriptSaveState::Saved;
			double const flash = m_Time - m_SaveFlashStart;
			bool const flashing = flash >= 0.0 && flash < VisualScriptBlinkSeconds;
			ImU32 color = saved ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : VisualScriptErrorColor;
			std::string const tooltip = saved ? "Edits save themselves" : "The last edit could not be saved: see the console";
			ImVec2 const lower = ImGui::GetCursorScreenPos();
			float const width = glyphs ? frame : textButtonWidth(saveText);
			ImGui::InvisibleButton("##save", ImVec2(width, frame));
			RecordItem(VisualScriptPanelItem::SaveStatus);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			if (flashing)
			{
				// A highlight that fades out (held, with Reduce Motion), and the text at full strength meanwhile.
				float const fade = m_Settings.IsReduceMotion() ? 1.0f : 1.0f - static_cast<float>(flash / VisualScriptBlinkSeconds);
				drawList->AddRectFilled(lower, ImVec2(lower.x + width, lower.y + frame), WithAlpha(m_Theme.Selection, 0.45f * fade),
				                        style.FrameRounding);
				color = saved ? textColor : color;
			}
			if (glyphs)
			{
				DrawGlyph(drawList, saved ? Glyph::Saved : Glyph::NotSaved, ImVec2(lower.x + frame * 0.5f, lower.y + frame * 0.5f),
				          frame * 0.55f, color);
			}
			else
			{
				drawList->AddText(ImVec2(lower.x + style.FramePadding.x, lower.y + style.FramePadding.y), color, saveText.c_str());
			}
			ImGui::SetItemTooltip("%s", (glyphs ? saveText + ": " + tooltip : tooltip).c_str());
		}
		ImGui::SameLine();
		std::string const arrangeTooltip = "Arrange the selected nodes (with two or more), else the whole graph";
		if (glyphs)
		{
			if (GlyphButton("##arrange", Glyph::Arrange, false, !m_Model.IsReadOnly(), arrangeTooltip))
			{
				m_FrameActions.push_back(Action::Arrange);
			}
		}
		else
		{
			ImGui::BeginDisabled(m_Model.IsReadOnly());
			if (ImGui::Button("Arrange"))
			{
				m_FrameActions.push_back(Action::Arrange);
			}
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("%s", arrangeTooltip.c_str());
		}
		RecordItem(VisualScriptPanelItem::Arrange);
		ImGui::SameLine();
		std::string const findTooltip = WithShortcut("Find in this script", Action::Find);
		if (glyphs ? GlyphButton("##find", Glyph::Find, m_FindBar.IsOpen(), true, findTooltip) : ImGui::Button("Find"))
		{
			m_FrameActions.push_back(Action::Find);
		}
		RecordItem(VisualScriptPanelItem::Find);
		if (!glyphs)
		{
			ImGui::SetItemTooltip("%s", findTooltip.c_str());
		}
		if (!viewInMenu)
		{
			ImGui::SameLine();
			bool const mac = ImGui::GetIO().ConfigMacOSXBehaviors;
			std::string const bookmarksTooltip = fmt::format("Bookmarks: {}+1..9 stores the view, {}+1..9 goes back to it",
			                                                 mac ? "Cmd+Option" : "Ctrl+Alt", mac ? "Cmd" : "Ctrl");
			if (glyphs ? GlyphButton("##bookmarks", Glyph::Bookmark, false, true, bookmarksTooltip) : ImGui::Button("Bookmarks"))
			{
				ImGui::OpenPopup("##bookmarksMenu");
			}
			RecordItem(VisualScriptPanelItem::Bookmarks);
			if (!glyphs)
			{
				ImGui::SetItemTooltip("%s", bookmarksTooltip.c_str());
			}
			if (ImGui::BeginPopup("##bookmarksMenu"))
			{
				DrawBookmarksMenu();
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			if (GlyphButton("##snap", Glyph::Grid, m_Settings.IsSnapToGrid(), true, "Snap to Grid (Alt inverts it while moving)"))
			{
				m_FrameActions.push_back(Action::ToggleSnapToGrid);
			}
			RecordItem(VisualScriptPanelItem::SnapToGrid);
			ImGui::SameLine();
			if (GlyphButton("##minimap", Glyph::Minimap, panes.ShowMinimap, true, WithShortcut("Minimap", Action::ToggleMinimap)))
			{
				m_FrameActions.push_back(Action::ToggleMinimap);
			}
			RecordItem(VisualScriptPanelItem::Minimap);
			ImGui::SameLine();
			if (ImGui::Button(fmt::format("{}##zoom", zoomText).c_str(), ImVec2(textButtonWidth("200%"), 0.0f)))
			{
				m_FrameActions.push_back(Action::ZoomReset);
			}
			RecordItem(VisualScriptPanelItem::Zoom);
			ImGui::SetItemTooltip("%s", WithShortcut("Zoom to 100%", Action::ZoomReset).c_str());
		}
		ImGui::SameLine();
		if (GlyphButton("##view", Glyph::Dots, false, true, "View options"))
		{
			ImGui::OpenPopup("##viewMenu");
		}
		RecordItem(VisualScriptPanelItem::ViewMenu);
		if (ImGui::BeginPopup("##viewMenu"))
		{
			DrawViewMenuItems();
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		// "?": the shortcut sheet. A press while the sheet is open only closes it (the press outside the sheet closes it first).
		{
			if (ImGui::Button("?##shortcuts", ImVec2(frame, frame)))
			{
				if (!m_ShortcutsKeepClosed)
				{
					m_OpenShortcuts = true;
				}
			}
			if (ImGui::IsItemActivated())
			{
				m_ShortcutsKeepClosed = m_ShortcutsOpen;
			}
			RecordItem(VisualScriptPanelItem::Shortcuts);
			ImGui::SetItemTooltip("%s", WithShortcut("Keyboard and mouse shortcuts", Action::ShowShortcuts).c_str());
		}
		ImGui::SameLine();
		if (GlyphButton("##details", Glyph::Details, m_Layout.DetailsState != VisualScriptPaneState::Hidden, true, "Details"))
		{
			if (m_Layout.DetailsState == VisualScriptPaneState::AutoHidden)
			{
				m_DetailsOverlay = !m_DetailsOverlay;
			}
			else
			{
				panes.ShowDetails = !panes.ShowDetails;
				m_Settings.SetPanes(panes);
			}
		}
		RecordItem(VisualScriptPanelItem::DetailsToggle);
		ImGui::PopID();
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + toolbarHeight));
	}

	void VisualScriptEditorPanel::DrawInstanceCombo(float width)
	{
		// "Instance: <entity>": the entities of the scene shown that run this graph, and None (spec §10.5).
		Scene& scene = m_Operations.GetContext().GetScene();
		auto const nameOf = [&scene](UUID entity)
		{
			return scene.HasEntity(entity) ? scene.GetEntityByUUID(entity).GetName() : std::string("None");
		};
		std::string const full = "Instance: " + (m_ContextEntity.IsValid() ? nameOf(m_ContextEntity) : std::string("None"));
		ImGuiStyle const& style = ImGui::GetStyle();
		std::string const shown = FitText(full, std::max(width - ImGui::GetFrameHeight() - style.FramePadding.x * 2.0f, 0.0f));
		float const comboWidth = std::max(width, ImGui::GetFrameHeight());
		ImVec2 const lower = ImGui::GetCursorScreenPos();
		// Recorded from the layout: an open combo's popup is the last item once BeginCombo returns.
		m_ItemRects[static_cast<size_t>(VisualScriptPanelItem::Instance)] =
			VisualScriptRect{ToVec2(lower), glm::vec2(lower.x + comboWidth, lower.y + ImGui::GetFrameHeight())};
		ImGui::SetNextItemWidth(comboWidth);
		if (!ImGui::BeginCombo("##instance", shown.c_str()))
		{
			ImGui::SetItemTooltip(
				"%s", (full + ": entities dropped on the canvas set their values on it, and My Script shows its values while the game runs")
						  .c_str());
			return;
		}
		if (ImGui::Selectable("None", !m_ContextEntity.IsValid()))
		{
			SetContextEntity(UUID::Invalid(), true);
		}
		for (entt::entity const handle : scene.GetAllEntitiesWith<VisualScriptComponent>())
		{
			Entity const entity(handle, &scene);
			if (entity.GetComponent<VisualScriptComponent>().Graph != m_Graph)
			{
				continue;
			}
			UUID const id = entity.GetUUID();
			if (ImGui::Selectable(fmt::format("{}##{}", entity.GetName(), id.GetValue()).c_str(), id == m_ContextEntity))
			{
				SetContextEntity(id, true);
			}
		}
		ImGui::EndCombo();
	}

	void VisualScriptEditorPanel::DrawLivePill(float width, std::string const& text)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const frame = ImGui::GetFrameHeight();
		ImVec2 const lower = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##live", ImVec2(width, frame));
		RecordItem(VisualScriptPanelItem::Live);
		ImU32 const color = m_Live == VisualScriptLiveState::Simulating    ? SimulateColor
		                    : m_Live == VisualScriptLiveState::LastWorking ? NoticeWarningColor
		                                                                   : PlayColor;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(lower, ImVec2(lower.x + width, lower.y + frame), color, frame * 0.5f);
		drawList->AddText(ImVec2(lower.x + style.FramePadding.x, lower.y + style.FramePadding.y), IM_COL32(255, 255, 255, 255),
		                  text.c_str());
		std::string tooltip;
		switch (m_Live)
		{
			case VisualScriptLiveState::Live:
				tooltip = std::string(Texts::LiveTooltip);
				break;
			case VisualScriptLiveState::LastWorking:
			{
				uint32_t const errors = CountProblems(m_Model.GetAnalysis(), std::nullopt).first;
				tooltip = fmt::format("{}: this version has {} {}: the game keeps running the last version that worked.",
				                      Texts::LastWorking, errors, errors == 1 ? "error" : "errors");
				break;
			}
			case VisualScriptLiveState::Simulating:
				tooltip = Texts::SimulatingTooltip;
				break;
			case VisualScriptLiveState::None:
				break;
		}
		ImGui::SetItemTooltip("%s", tooltip.c_str());
	}

	void VisualScriptEditorPanel::DrawBookmarksMenu()
	{
		// The graph's bookmarks with their keys, then Add Bookmark (the next free slot), Rename and Delete (spec §10.3).
		VisualScriptGraphViews const* views = m_Settings.FindGraph(m_Graph);
		std::vector<VisualScriptBookmark> const bookmarks = views != nullptr ? views->Bookmarks : std::vector<VisualScriptBookmark>();
		bool const mac = ImGui::GetIO().ConfigMacOSXBehaviors;
		auto const slotAction = [](Action first, uint8_t slot)
		{
			return static_cast<Action>(static_cast<uint8_t>(first) + slot - 1);
		};
		auto const chordText = [mac](Action action)
		{
			VisualScriptShortcut const* shortcut = FindVisualScriptShortcut(action);
			return shortcut != nullptr ? FormatVisualScriptChord(shortcut->Chord, mac) : std::string();
		};
		if (bookmarks.empty())
		{
			ImGui::TextDisabled("%s", Texts::NoBookmarks.data());
		}
		for (VisualScriptBookmark const& bookmark : bookmarks)
		{
			std::string const keys = chordText(slotAction(Action::GoToBookmark1, bookmark.Slot));
			if (ImGui::MenuItem(fmt::format("{}##goTo{}", bookmark.Name, bookmark.Slot).c_str(), keys.c_str()))
			{
				m_FrameActions.push_back(slotAction(Action::GoToBookmark1, bookmark.Slot));
			}
		}
		ImGui::Separator();
		std::optional<uint8_t> freeSlot;
		for (uint8_t slot = 1; slot <= VisualScriptViewSettings::MaxBookmarkSlot && !freeSlot; slot++)
		{
			if (views == nullptr || views->FindBookmark(slot) == nullptr)
			{
				freeSlot = slot;
			}
		}
		std::string const addKeys = freeSlot ? chordText(slotAction(Action::StoreBookmark1, *freeSlot)) : std::string();
		if (ImGui::MenuItem("Add Bookmark", addKeys.c_str(), false, freeSlot.has_value()))
		{
			m_FrameActions.push_back(slotAction(Action::StoreBookmark1, *freeSlot));
		}
		if (!freeSlot)
		{
			ImGui::SetItemTooltip("Every slot holds a bookmark: delete one first");
		}
		if (ImGui::BeginMenu("Rename", !bookmarks.empty()))
		{
			for (VisualScriptBookmark const& bookmark : bookmarks)
			{
				std::string& name = m_BookmarkNames[static_cast<size_t>(bookmark.Slot - 1)];
				ImGui::PushID(bookmark.Slot);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(fmt::format("{}", bookmark.Slot).c_str());
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
				bool const done =
					ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
				bool const active = ImGui::IsItemActive();
				if ((done || ImGui::IsItemDeactivatedAfterEdit()) && name != bookmark.Name)
				{
					VisualScriptBookmark renamed = bookmark;
					renamed.Name = name;
					m_Settings.SetBookmark(m_Graph, std::move(renamed));
				}
				else if (!active)
				{
					name = bookmark.Name;
				}
				if (done)
				{
					ImGui::CloseCurrentPopup();
				}
				ImGui::PopID();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Delete", !bookmarks.empty()))
		{
			for (VisualScriptBookmark const& bookmark : bookmarks)
			{
				if (ImGui::MenuItem(fmt::format("{}##delete{}", bookmark.Name, bookmark.Slot).c_str()))
				{
					m_Settings.RemoveBookmark(m_Graph, bookmark.Slot);
				}
			}
			ImGui::EndMenu();
		}
	}

	void VisualScriptEditorPanel::DrawSubGraphMenu()
	{
		if (!ImGui::BeginPopup("##subGraphs"))
		{
			return;
		}
		VisualScriptDocument const& document = m_Model.GetDisplayDocument();
		VisualScriptAnalysis const* analysis = m_Model.GetAnalysis();
		auto const item = [&](std::string const& name, uint32_t function)
		{
			auto const [errors, warnings] = CountProblems(analysis, function);
			std::string const badge = errors > 0     ? fmt::format("{} errors", errors)
			                          : warnings > 0 ? fmt::format("{} warnings", warnings)
			                                         : "";
			if (ImGui::MenuItem(fmt::format("{}##{}", name, function).c_str(), badge.empty() ? nullptr : badge.c_str(),
			                    m_Model.GetFunction() == function))
			{
				m_Model.ShowFunction(function);
			}
		};
		item("Event Graph", 0);
		for (VisualScriptFunction const& function : document.Functions)
		{
			item(function.Name, function.ID);
		}
		ImGui::EndPopup();
	}

	void VisualScriptEditorPanel::DrawViewMenuItems()
	{
		VisualScriptScrollMode const scroll = m_Settings.GetScrollMode();
		if (ImGui::MenuItem("Scroll: Zoom", nullptr, scroll == VisualScriptScrollMode::Zoom))
		{
			m_Settings.SetScrollMode(VisualScriptScrollMode::Zoom);
		}
		ImGui::SetItemTooltip("The wheel zooms to the cursor; Ctrl+wheel and Shift+wheel pan");
		if (ImGui::MenuItem("Scroll: Pan", nullptr, scroll == VisualScriptScrollMode::Pan))
		{
			m_Settings.SetScrollMode(VisualScriptScrollMode::Pan);
		}
		ImGui::SetItemTooltip("The wheel and trackpads pan; Ctrl+wheel zooms to the cursor");
		ImGui::Separator();
		if (ImGui::MenuItem("Reduce Motion", nullptr, m_Settings.IsReduceMotion()))
		{
			m_Settings.SetReduceMotion(!m_Settings.IsReduceMotion());
		}
		if (ImGui::MenuItem("Context Palette", nullptr, m_Settings.IsContextPalette()))
		{
			m_Settings.SetContextPalette(!m_Settings.IsContextPalette());
		}
		ImGui::SetItemTooltip("A wire released on empty canvas lists only the nodes it connects to");
		if (m_ViewItemsInMenu)
		{
			ImGui::Separator();
			if (ImGui::BeginMenu("Bookmarks"))
			{
				DrawBookmarksMenu();
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Snap to Grid", nullptr, m_Settings.IsSnapToGrid()))
			{
				m_FrameActions.push_back(Action::ToggleSnapToGrid);
			}
			if (ImGui::MenuItem("Minimap", GetShortcutText(Action::ToggleMinimap).c_str(), m_Settings.GetPanes().ShowMinimap))
			{
				m_FrameActions.push_back(Action::ToggleMinimap);
			}
			if (ImGui::MenuItem(
					fmt::format("Zoom to 100% (now {}%)", static_cast<int>(std::lround(m_Model.GetViewState().Zoom * 100.0f))).c_str(),
					GetShortcutText(Action::ZoomReset).c_str()))
			{
				m_FrameActions.push_back(Action::ZoomReset);
			}
		}
	}

	void VisualScriptEditorPanel::DrawNotices()
	{
		if (m_Notices.empty())
		{
			return;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		ImVec2 const origin = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(origin.x + style.WindowPadding.x, origin.y));
		ImGui::BeginGroup();
		// The notices' buttons change flags and commit, never the list, which only the next sync makes again.
		for (NoticeKind const kind : m_Notices)
		{
			DrawNotice(kind);
		}
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(origin.x, ImGui::GetItemRectMax().y + style.ItemSpacing.y));
	}

	void VisualScriptEditorPanel::DrawNotice(NoticeKind kind)
	{
		ImGuiStyle const& style = ImGui::GetStyle();
		float const width = std::max(ImGui::GetContentRegionAvail().x - style.WindowPadding.x, ImGui::GetFontSize());
		ImU32 accent = NoticeInfoColor;
		std::string text;
		char const* badge = "i";
		switch (kind)
		{
			case NoticeKind::Degraded:
				accent = NoticeWarningColor;
				text = std::string(Texts::Degraded);
				badge = "!";
				break;
			case NoticeKind::ReadOnlyMemory:
				accent = NoticeNeutralColor;
				text = std::string(Texts::ReadOnlyMemory);
				break;
			case NoticeKind::ReadOnlyFile:
			{
				accent = NoticeNeutralColor;
				std::filesystem::path const file = AssetManager::GetAbsolutePath(m_Graph);
				text = MakeVisualScriptReadOnlyFileNote(FileSystem::PathToUtf8(file.filename()));
				break;
			}
			case NoticeKind::Repaired:
				accent = NoticeWarningColor;
				badge = "!";
				text = fmt::format("This file was repaired while loading: {} {} (see the console)", m_Repairs,
				                   m_Repairs == 1 ? "problem" : "problems");
				break;
			case NoticeKind::Normalization:
				text = m_Normalizations == 1
				           ? std::string("1 node uses a renamed member or has no position; it is updated with your next edit.")
				           : fmt::format("{} nodes use renamed members or have no position; they are updated with your next edit.",
				                         m_Normalizations);
				break;
			case NoticeKind::NewFunction:
				text = std::string(Texts::NewFunction);
				break;
		}
		// The degraded notice blinks once when a gesture that needs the runtime was refused (held, with Reduce Motion).
		float tint = NoticeTint;
		double const blink = m_Time - m_BlinkStart;
		if (kind == NoticeKind::Degraded && blink >= 0.0 && blink < VisualScriptBlinkSeconds)
		{
			float const wave =
				m_Settings.IsReduceMotion() ? 1.0f : std::sin(static_cast<float>(blink / VisualScriptBlinkSeconds) * glm::pi<float>());
			tint = NoticeTint + (NoticeBlinkTint - NoticeTint) * wave;
		}

		// The buttons, right to left of the text: [Show Console], [Retry], [Update Nodes], and a dismiss cross.
		bool const showConsole = kind == NoticeKind::Degraded && static_cast<bool>(m_Services.ShowConsole);
		bool const retry = kind == NoticeKind::ReadOnlyFile;
		bool const updateNodes = kind == NoticeKind::Normalization;
		bool const dismiss = kind == NoticeKind::Repaired || kind == NoticeKind::NewFunction;
		auto const buttonWidth = [&style](char const* label)
		{
			return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
		};
		float const buttonsWidth = (showConsole ? buttonWidth("Show Console") : 0.0f) + (retry ? buttonWidth("Retry") : 0.0f) +
		                           (updateNodes ? buttonWidth("Update Nodes") : 0.0f) +
		                           (dismiss ? ImGui::GetFrameHeight() + style.ItemSpacing.x : 0.0f);

		ImGui::PushID(static_cast<int>(kind));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, MixColor(ImGui::GetColorU32(ImGuiCol_WindowBg), accent, tint));
		ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(accent, 0.85f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x, style.FramePadding.y));
		if (ImGui::BeginChild("##notice", ImVec2(width, 0.0f),
		                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
		                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			float const top = ImGui::GetCursorPosY();
			float const right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
			float const frame = ImGui::GetFrameHeight();
			// A round badge in the accent: "!" for what limits editing, "i" for information.
			ImVec2 const cursor = ImGui::GetCursorScreenPos();
			ImVec2 const center(cursor.x + frame * 0.5f, cursor.y + frame * 0.5f);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			drawList->AddCircleFilled(center, frame * 0.32f, accent);
			float const badgeSize = ImGui::GetFontSize() * 0.75f;
			ImVec2 const badgeText = ImGui::GetFont()->CalcTextSizeA(badgeSize, FLT_MAX, 0.0f, badge);
			drawList->AddText(ImGui::GetFont(), badgeSize, ImVec2(center.x - badgeText.x * 0.5f, center.y - badgeText.y * 0.5f),
			                  IM_COL32(255, 255, 255, 255), badge);
			ImGui::Dummy(ImVec2(frame, frame));
			ImGui::SameLine();
			float const textLeft = ImGui::GetCursorPosX();
			ImGui::SetCursorPosY(top + (frame - ImGui::GetTextLineHeight()) * 0.5f);
			ImGui::PushTextWrapPos(std::max(right - buttonsWidth, textLeft + ImGui::GetFontSize() * 4.0f));
			ImGui::TextUnformatted(text.c_str());
			ImGui::PopTextWrapPos();

			if (buttonsWidth > 0.0f)
			{
				ImGui::SetCursorPos(ImVec2(right - buttonsWidth + style.ItemSpacing.x, top));
			}
			if (showConsole)
			{
				if (ImGui::Button("Show Console"))
				{
					m_Services.ShowConsole();
				}
				RecordItem(VisualScriptPanelItem::ShowConsole);
				ImGui::SameLine();
			}
			if (retry)
			{
				if (ImGui::Button("Retry"))
				{
					// The permission is checked again at the next sync.
					m_Model.CheckWritable();
				}
				RecordItem(VisualScriptPanelItem::Retry);
				ImGui::SetItemTooltip("Check again whether the file can be written");
				ImGui::SameLine();
			}
			if (updateNodes)
			{
				ImGui::BeginDisabled(m_Library == nullptr);
				if (ImGui::Button("Update Nodes"))
				{
					// One step "Update Nodes": the commit writes canonical types and positions (spec §4.2).
					VisualScriptActionIntent intent;
					intent.Action = Action::UpdateNode;
					intent.Selection.assign(m_Model.GetSelection().begin(), m_Model.GetSelection().end());
					intent.Position = m_Model.GetViewState().Center;
					ExecuteIntent(intent);
				}
				ImGui::EndDisabled();
				RecordItem(VisualScriptPanelItem::UpdateNodes);
				ImGui::SetItemTooltip("%s",
				                      m_Library != nullptr ? "Write them now, as one step" : "Updating nodes needs the scripting runtime");
				ImGui::SameLine();
			}
			if (dismiss)
			{
				if (GlyphButton("##dismiss", Glyph::Close, false, true, "Dismiss"))
				{
					if (kind == NoticeKind::Repaired)
					{
						m_RepairNotice = false;
					}
					else if (m_FunctionHint)
					{
						m_FunctionHintsSeen.push_back(*m_FunctionHint);
						m_FunctionHint.reset();
					}
				}
				RecordItem(kind == NoticeKind::Repaired ? VisualScriptPanelItem::DismissRepaired
				                                        : VisualScriptPanelItem::DismissNewFunction);
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
		ImGui::PopID();
	}

	void VisualScriptEditorPanel::DrawCard(ImVec2 canvasMin, ImVec2 canvasMax)
	{
		if (m_Card == VisualScriptCard::None)
		{
			return;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		float const fontSize = ImGui::GetFontSize();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		bool const replacesCanvas = m_Card != VisualScriptCard::EmptyGraph;
		if (replacesCanvas)
		{
			// A missing or refused file has no canvas: the card stands in its place.
			drawList->AddRectFilled(canvasMin, canvasMax, m_Theme.Canvas);
		}
		float const width = std::min(CardWidth * fontSize, std::max(canvasMax.x - canvasMin.x - fontSize * 2.0f, fontSize * 4.0f));
		// Centred with the height it had at the last draw (it sizes itself to its contents).
		float const height = m_CardHeight > 0.0f ? m_CardHeight : ImGui::GetFrameHeight() * 4.0f;
		ImGui::SetCursorScreenPos(
			ImVec2((canvasMin.x + canvasMax.x - width) * 0.5f, std::max((canvasMin.y + canvasMax.y - height) * 0.5f, canvasMin.y)));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding * 2.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * 1.5f, style.WindowPadding.y * 1.5f));
		if (ImGui::BeginChild("##card", ImVec2(width, 0.0f),
		                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
		                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			ImGui::PushTextWrapPos(0.0f);
			bool const editable = !m_Model.IsReadOnly();
			switch (m_Card)
			{
				case VisualScriptCard::Missing:
				{
					std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(m_Graph);
					ImGui::TextUnformatted(fmt::format("The file is missing: {}.", metadata ? metadata->Path : std::string()).c_str());
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::TextUnformatted("The canvas comes back when the file does (File > Refresh, or the content browser).");
					ImGui::PopStyleColor();
					break;
				}
				case VisualScriptCard::Unloadable:
					ImGui::TextUnformatted(fmt::format("The file could not be read: {}.", m_LoadError).c_str());
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::TextUnformatted("graph.set can write it again; the window then shows it.");
					ImGui::PopStyleColor();
					break;
				case VisualScriptCard::EmptyGraph:
				{
					ImGui::TextUnformatted(Texts::EmptyGraph.data(), Texts::EmptyGraph.data() + Texts::EmptyGraph.size());
					ImGui::Spacing();
					// Each button adds its node at the view's centre, one step each.
					glm::vec2 const center = m_Model.GetViewState().Center;
					glm::vec2 const position = glm::round((center - glm::vec2(64.0f, 24.0f)) / VisualScriptCanvasMetrics::GridSize) *
					                           VisualScriptCanvasMetrics::GridSize;
					ImGui::BeginDisabled(!editable);
					if (ImGui::Button("Add Create Event"))
					{
						ExecuteEdit(VisualScriptAddNodeEdit{"Event.Create", Json::object(), position});
					}
					RecordItem(VisualScriptPanelItem::AddCreateEvent);
					ImGui::SameLine();
					if (ImGui::Button("Add Update Event"))
					{
						ExecuteEdit(VisualScriptAddNodeEdit{"Event.Update", Json::object(), position});
					}
					RecordItem(VisualScriptPanelItem::AddUpdateEvent);
					ImGui::EndDisabled();
					ImGui::SameLine();
					ImGui::BeginDisabled(!editable || m_Library == nullptr);
					if (ImGui::Button(WithShortcut("Add Node...", Action::OpenPalette).c_str()))
					{
						VisualScriptPaletteIntent palette;
						palette.Position = center;
						m_Palette.Open(palette);
					}
					ImGui::EndDisabled();
					RecordItem(VisualScriptPanelItem::AddNode);
					ImGui::Spacing();
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::TextUnformatted(Texts::EmptyGraphHelp.data(), Texts::EmptyGraphHelp.data() + Texts::EmptyGraphHelp.size());
					ImGui::PopStyleColor();
					break;
				}
				case VisualScriptCard::None:
					break;
			}
			ImGui::PopTextWrapPos();
			m_CardHeight = ImGui::GetWindowSize().y;
			if (replacesCanvas)
			{
				ImGui::Spacing();
				if (ImGui::Button("Close"))
				{
					m_CloseRequested = true;
				}
				RecordItem(VisualScriptPanelItem::CloseCard);
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	void VisualScriptEditorPanel::DrawCanvasHint(ImVec2 canvasMin, ImVec2 canvasMax)
	{
		// Once per user, over the first canvas: the gestures that start everything. Left out while a gesture shows its own hint.
		m_CanvasHintShown = !m_Settings.IsHintSeen(VisualScriptCanvasHint) && m_Card != VisualScriptCard::Missing &&
		                    m_Card != VisualScriptCard::Unloadable && !m_HintShown;
		if (!m_CanvasHintShown)
		{
			m_RibbonHeight = 0.0f;
			return;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		std::string_view const text = m_Settings.GetScrollMode() == VisualScriptScrollMode::Pan ? Texts::CanvasHintPan : Texts::CanvasHint;
		float const width = canvasMax.x - canvasMin.x;
		float const height = m_RibbonHeight > 0.0f ? m_RibbonHeight : ImGui::GetFrameHeightWithSpacing() * 2.0f;
		ImGui::SetCursorScreenPos(ImVec2(canvasMin.x, canvasMax.y - height));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(m_Theme.Body, HintBackgroundAlpha));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x * 2.0f, style.FramePadding.y));
		if (ImGui::BeginChild("##canvasHint", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
		                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			float const buttonWidth = ImGui::CalcTextSize("Got it").x + style.FramePadding.x * 2.0f;
			float const right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
			float const top = ImGui::GetCursorPosY();
			ImGui::PushTextWrapPos(right - buttonWidth - style.ItemSpacing.x);
			ImGui::SetCursorPosY(top + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
			ImGui::TextUnformatted(text.data(), text.data() + text.size());
			ImGui::PopTextWrapPos();
			ImGui::SetCursorPos(ImVec2(right - buttonWidth, top));
			if (ImGui::Button("Got it"))
			{
				m_Settings.SetHintSeen(VisualScriptCanvasHint);
			}
			RecordItem(VisualScriptPanelItem::GotIt);
			m_RibbonHeight = ImGui::GetWindowSize().y;
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}

	void VisualScriptEditorPanel::DrawShortcutSheet()
	{
		constexpr char const* SheetId = "Visual Script Shortcuts###VisualScriptShortcuts";
		ImGuiWindow const* window = ImGui::GetCurrentWindow();
		if (m_OpenShortcuts)
		{
			m_OpenShortcuts = false;
			ImGui::OpenPopup(SheetId);
		}
		// At the UI's normal size, centred on the window, 34 font sizes wide; it scrolls when the window is shorter.
		float const fontSize = ImGui::GetFontSize();
		float const width = ShortcutSheetWidth * fontSize;
		ImGui::SetNextWindowPos(ImVec2(window->Pos.x + window->Size.x * 0.5f, window->Pos.y + window->Size.y * 0.5f), ImGuiCond_Appearing,
		                        ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, std::max(window->Size.y * 0.9f, fontSize * 10.0f)));
		// BeginPopup adds ImGuiWindowFlags_NoSavedSettings (imgui.cpp:13368): the sheet writes nothing to imgui.ini.
		m_ShortcutsOpen = ImGui::BeginPopup(SheetId, ImGuiWindowFlags_NoSavedSettings);
		if (!m_ShortcutsOpen)
		{
			return;
		}
		ImGui::TextUnformatted(Texts::ShortcutSheet.data(), Texts::ShortcutSheet.data() + Texts::ShortcutSheet.size());
		ImGui::Separator();
		bool const mac = ImGui::GetIO().ConfigMacOSXBehaviors;
		ImFont* const keysFont = m_Services.MonospaceFont;
		if (ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 0.38f);
			ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.62f);
			auto const row = [&](std::string const& keys, std::string_view label)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (keysFont != nullptr)
				{
					ImGui::PushFont(keysFont, 0.0f);
				}
				ImGui::TextUnformatted(keys.c_str());
				if (keysFont != nullptr)
				{
					ImGui::PopFont();
				}
				ImGui::TableNextColumn();
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(label.data(), label.data() + label.size());
				ImGui::PopTextWrapPos();
			};
			auto const heading = [](std::string_view name)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::TextUnformatted(name.data(), name.data() + name.size());
				ImGui::PopStyleColor();
				ImGui::TableNextColumn();
			};
			std::optional<VisualScriptShortcutGroup> group;
			for (VisualScriptShortcut const& shortcut : VisualScriptShortcuts)
			{
				if (shortcut.Group != group)
				{
					group = shortcut.Group;
					heading(GetVisualScriptShortcutGroupName(shortcut.Group));
				}
				row(FormatVisualScriptChord(shortcut.Chord, mac), shortcut.Label);
			}
			heading("Mouse");
			for (VisualScriptMouseGesture const& gesture : VisualScriptMouseGestures)
			{
				row(FormatVisualScriptMouseGesture(gesture, mac), fmt::format("{}: {}", gesture.Target, gesture.Label));
			}
			ImGui::EndTable();
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	bool VisualScriptEditorPanel::DrawSplitter(char const* id, ImVec2 size, bool vertical, float& delta)
	{
		ImGui::InvisibleButton(id, ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
		bool const active = ImGui::IsItemActive();
		if (ImGui::IsItemHovered() || active)
		{
			ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
			ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
			                                          ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered));
		}
		delta = active ? (vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y) : 0.0f;
		return active && delta != 0.0f;
	}

	void VisualScriptEditorPanel::DrawPane(char const* id, VisualScriptWindowPart part, ImVec2 size, bool border)
	{
		// The children My Script and Details draw in. They are not resizable by ImGui (the window's splitters size them), so
		// ImGui keeps no settings for them.
		ImGui::BeginChild(id, ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
		                  border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None, ImGuiWindowFlags_None);
		ImGuiID const window = ImGui::GetCurrentWindow()->ID;
		(part == VisualScriptWindowPart::Sidebar ? m_SidebarWindow : m_DetailsWindow) = window;
		DrawPaneContents(part);
		DrawFocusFrame(part);
		ImGui::EndChild();
	}

	void VisualScriptEditorPanel::DrawPaneContents(VisualScriptWindowPart part)
	{
		VisualScriptPaneContext context = MakePaneContext();
		VisualScriptPaneResult result = part == VisualScriptWindowPart::Sidebar ? m_Sidebar.Draw(context) : m_Details.Draw(context);
		ApplyPaneResult(std::move(result));
	}

	void VisualScriptEditorPanel::DrawDrawer(ImVec2 size)
	{
		// The drawer's contents, and its close button at the right.
		float const frame = ImGui::GetFrameHeight();
		float const spacing = ImGui::GetStyle().ItemSpacing.x;
		ImGui::BeginChild("##drawer", size, ImGuiChildFlags_None, ImGuiWindowFlags_None);
		ImVec2 const available = ImGui::GetContentRegionAvail();
		ImGui::BeginChild("##drawerContents", ImVec2(std::max(available.x - frame - spacing, 1.0f), std::max(available.y, 1.0f)),
		                  ImGuiChildFlags_None, ImGuiWindowFlags_None);
		VisualScriptPaneContext context = MakePaneContext();
		VisualScriptPaneResult result = m_Drawer.Draw(context);
		ImGui::EndChild();
		ImGui::SameLine(0.0f, spacing);
		if (GlyphButton("##closeDrawer", Glyph::Close, false, true, "Close"))
		{
			ShowDrawer(VisualScriptDrawerMode::Closed);
		}
		ImGui::EndChild();
		ApplyPaneResult(std::move(result));
	}

	void VisualScriptEditorPanel::DrawFocusFrame(VisualScriptWindowPart part)
	{
		if (!m_Focused || m_FocusedPart != part)
		{
			return;
		}
		ImGuiWindow const* window = ImGui::GetCurrentWindow();
		float const thickness = ImGui::GetStyle().FontScaleDpi;
		ImVec2 const lower(window->Pos.x + thickness * 0.5f, window->Pos.y + thickness * 0.5f);
		ImVec2 const upper(window->Pos.x + window->Size.x - thickness * 0.5f, window->Pos.y + window->Size.y - thickness * 0.5f);
		ImGui::GetWindowDrawList()->PushClipRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y),
		                                         false);
		ImGui::GetWindowDrawList()->AddRect(lower, upper, WithAlpha(m_Theme.Selection, FocusFrameAlpha), 0.0f, thickness);
		ImGui::GetWindowDrawList()->PopClipRect();
	}

	void VisualScriptEditorPanel::DrawBody()
	{
		float const fontSize = ImGui::GetFontSize();
		float const splitter = std::round(VisualScriptSplitterLogicalPixels * ImGui::GetStyle().FontScaleDpi);
		ImVec2 const available = ImGui::GetContentRegionAvail();
		VisualScriptPaneSettings panes = m_Settings.GetPanes();
		m_Layout = ComputeVisualScriptWindowLayout(available.x, fontSize, splitter, panes);
		// My Script also opens over the canvas while turned off, for a row key or a rename that Details or the canvas asked
		// for (RouteRowAction); Details only while auto-hidden.
		if (m_Layout.SidebarState == VisualScriptPaneState::Shown)
		{
			m_SidebarOverlay = false;
		}
		if (m_Layout.DetailsState != VisualScriptPaneState::AutoHidden)
		{
			m_DetailsOverlay = false;
		}
		// A press on the canvas can open an overlay (a double-click on a node header shows it in Details): only a press while
		// it was already open closes it.
		bool const sidebarWasOpen = m_SidebarOverlay;
		bool const detailsWasOpen = m_DetailsOverlay;

		bool const drawerOpen = m_DrawerMode != VisualScriptDrawerMode::Closed;
		float const drawerHeight = drawerOpen ? std::clamp(m_DrawerHeight * fontSize, VisualScriptDrawerMinHeight * fontSize,
		                                                   std::max(available.y * 0.6f, fontSize))
		                                      : 0.0f;
		float const rowHeight = std::max(available.y - (drawerOpen ? drawerHeight + splitter : 0.0f), 1.0f);

		float delta = 0.0f;
		if (m_Layout.Stacked)
		{
			// My Script above Details in one column on the sidebar's side, a horizontal splitter between them at the window's
			// share. The column follows the panes' stored widths (the wider of them, at most what the canvas leaves), which only
			// the side-by-side splitters change, so its edge is a plain gap: dragging it would rewrite both widths, which every
			// graph window reads, and narrowing it would switch the arrangement under the drag.
			float const paneHeights = std::max(rowHeight - splitter, 2.0f);
			float const minimum = std::min(VisualScriptStackedPaneMinHeight * fontSize, paneHeights * 0.5f);
			float const top = std::round(std::clamp(m_StackShare * paneHeights, minimum, paneHeights - minimum));
			ImVec2 const origin = ImGui::GetCursorScreenPos();
			ImGui::BeginGroup();
			DrawPane("##sidebar", VisualScriptWindowPart::Sidebar, ImVec2(m_Layout.Column, top));
			ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top));
			if (DrawSplitter("##stackSplitter", ImVec2(m_Layout.Column, splitter), false, delta))
			{
				m_StackShare = std::clamp((top + delta) / paneHeights, 0.0f, 1.0f);
			}
			ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top + splitter));
			DrawPane("##details", VisualScriptWindowPart::Details, ImVec2(m_Layout.Column, paneHeights - top));
			ImGui::EndGroup();
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::Dummy(ImVec2(splitter, rowHeight));
			ImGui::SameLine(0.0f, 0.0f);
		}
		else if (m_Layout.Sidebar > 0.0f)
		{
			DrawPane("##sidebar", VisualScriptWindowPart::Sidebar, ImVec2(m_Layout.Sidebar, rowHeight));
			ImGui::SameLine(0.0f, 0.0f);
			if (DrawSplitter("##sidebarSplitter", ImVec2(splitter, rowHeight), true, delta))
			{
				panes.Sidebar = std::max((m_Layout.Sidebar + delta) / fontSize, VisualScriptSidebarMinWidth);
				m_Settings.SetPanes(panes);
			}
			ImGui::SameLine(0.0f, 0.0f);
		}
		DrawCanvas(ImVec2(m_Layout.Canvas, rowHeight));
		if (m_Layout.Details > 0.0f)
		{
			ImGui::SameLine(0.0f, 0.0f);
			if (DrawSplitter("##detailsSplitter", ImVec2(splitter, rowHeight), true, delta))
			{
				panes.Details = std::max((m_Layout.Details - delta) / fontSize, VisualScriptDetailsMinWidth);
				m_Settings.SetPanes(panes);
			}
			ImGui::SameLine(0.0f, 0.0f);
			DrawPane("##details", VisualScriptWindowPart::Details, ImVec2(m_Layout.Details, rowHeight));
		}

		// A pane opened over its edge of the canvas, at its usual width, at most most of the canvas. Escape or a click outside
		// closes it, but not the click that closes a popup over it or answers its modal (My Script's delete): a popup is not
		// one of its child windows, so it is never hovered then, and a pane that is not drawn would drop its modal.
		auto const drawOverlay =
			[&](char const* id, VisualScriptWindowPart part, float widthInFontSizes, float minimum, bool left, bool wasOpen, bool& open)
		{
			if (!open)
			{
				return;
			}
			float const width = std::min(std::max(widthInFontSizes, minimum) * fontSize, m_CanvasSize.x * VisualScriptOverlayMaxShare);
			ImVec2 const position(left ? m_CanvasMin.x : m_CanvasMin.x + m_CanvasSize.x - width, m_CanvasMin.y);
			ImGui::SetCursorScreenPos(position);
			ImGui::BeginChild(id, ImVec2(std::max(width, 1.0f), rowHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
			ImGuiID const window = ImGui::GetCurrentWindow()->ID;
			(part == VisualScriptWindowPart::Sidebar ? m_SidebarWindow : m_DetailsWindow) = window;
			DrawPaneContents(part);
			bool const hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			DrawFocusFrame(part);
			ImGui::EndChild();
			if (wasOpen && !hovered && GImGui->OpenPopupStack.Size == 0 &&
			    (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
			{
				open = false;
			}
		};
		drawOverlay("##sidebarOverlay", VisualScriptWindowPart::Sidebar, panes.Sidebar, VisualScriptSidebarMinWidth, true, sidebarWasOpen,
		            m_SidebarOverlay);
		drawOverlay("##detailsOverlay", VisualScriptWindowPart::Details, panes.Details, VisualScriptDetailsMinWidth, false, detailsWasOpen,
		            m_DetailsOverlay);
		if (!drawerOpen)
		{
			return;
		}
		float const leftOfCanvas =
			m_Layout.Stacked ? m_Layout.Column + splitter : m_Layout.Sidebar + (m_Layout.Sidebar > 0.0f ? splitter : 0.0f);
		ImGui::SetCursorScreenPos(ImVec2(m_CanvasMin.x - leftOfCanvas, m_CanvasMin.y + rowHeight));
		if (DrawSplitter("##drawerSplitter", ImVec2(available.x, splitter), false, delta))
		{
			m_DrawerHeight = std::max((drawerHeight - delta) / fontSize, VisualScriptDrawerMinHeight);
		}
		DrawDrawer(ImVec2(available.x, drawerHeight));
	}

	void VisualScriptEditorPanel::DrawCanvas(ImVec2 size)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImVec2 const childSize(std::max(size.x, 1.0f), std::max(size.y, 1.0f));
		ImGui::BeginChild("##canvas", childSize, ImGuiChildFlags_None, CanvasFlags);
		ImGui::PopStyleVar();
		m_CanvasWindow = ImGui::GetCurrentWindow()->ID;
		ImVec2 const canvasMin = ImGui::GetCursorScreenPos();
		m_CanvasMin = ToVec2(canvasMin);
		m_CanvasSize = ToVec2(childSize);

		// The canvas's first item covers it: a press of any button makes it the active item until the release, so a gesture
		// keeps the mouse when it leaves the canvas. Items drawn later over it (the toasts) take the mouse where they are.
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##surface", childSize,
		                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		ImGuiID const surface = ImGui::GetItemID();
		// The surface itself is not hovered while an item drawn over it is (the live value widget; AllowOverlap,
		// imgui.cpp:5096-5100): the canvas still has the mouse there, for the wheel, the right and middle buttons and the hover.
		bool const surfaceHovered = ImGui::IsItemHovered();
		bool const hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
		bool active = ImGui::IsItemActive();
		m_CanvasHovered = hovered;

		ImGuiIO const& io = ImGui::GetIO();
		// The live widget takes the left button only: a right or middle press over it is the canvas's, which becomes the active
		// item and takes the keyboard as a press on the surface makes it (imgui_widgets.cpp:626-636), so a pan keeps the mouse.
		if (hovered && !surfaceHovered && !active)
		{
			for (ImGuiMouseButton const button : {ImGuiMouseButton_Right, ImGuiMouseButton_Middle})
			{
				if (io.MouseClicked[button])
				{
					ImGuiWindow* canvas = ImGui::GetCurrentWindow();
					ImGui::SetKeyOwner(ImGui::MouseButtonToKey(button), surface);
					ImGui::SetActiveID(surface, canvas);
					GImGui->ActiveIdMouseButton = static_cast<ImS8>(button);
					ImGui::SetFocusID(surface, canvas);
					ImGui::FocusWindow(canvas);
					active = true;
					break;
				}
			}
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		VisualScriptCanvasInput input;
		input.Mouse = ImGui::IsMousePosValid() ? ToVec2(io.MousePos) : m_LastMouse;
		m_LastMouse = input.Mouse;
		input.CanvasMin = m_CanvasMin;
		input.CanvasSize = m_CanvasSize;
		input.UIScale = style.FontScaleDpi;
		input.FontSize = ImGui::GetFontSize();
		input.CurveTessellationTolerance = style.CurveTessellationTol;
		input.Hovered = hovered;
		input.Focused = m_FocusedPart == VisualScriptWindowPart::Canvas;
		for (int button = 0; button < 3; button++)
		{
			size_t const index = static_cast<size_t>(button);
			input.Down[index] = io.MouseDown[button];
			input.Pressed[index] = io.MouseClicked[button] && (surfaceHovered || active);
			input.Released[index] = io.MouseReleased[button];
			input.DoubleClicked[index] = input.Pressed[index] && io.MouseDoubleClicked[button];
		}
		input.ExtraPressed = {io.MouseClicked[3] && m_WindowHovered, io.MouseClicked[4] && m_WindowHovered};
		if (hovered)
		{
			input.Wheel = glm::vec2(io.MouseWheelH, io.MouseWheel);
		}
		input.Ctrl = io.KeyCtrl;
		input.Shift = io.KeyShift;
		input.Alt = io.KeyAlt;
		if (input.Focused && !io.WantTextInput)
		{
			input.SpaceDown = ImGui::IsKeyDown(ImGuiKey_Space);
			input.SpacePressed = ImGui::IsKeyPressed(ImGuiKey_Space, false);
			input.SpaceReleased = ImGui::IsKeyReleased(ImGuiKey_Space);
			input.Characters = ReadTypedCharacters();
		}
		input.TextInput = io.WantTextInput;
		input.Time = m_Time;
		input.Actions = std::move(m_FrameActions);
		m_FrameActions.clear();
		input.WidgetActive = m_ActiveChip.has_value();
		AcceptDrops(input);
		UpdateCanvas(input);
		TrackProblemBadges(input);

		// A value of the live widget held still commits within the interval (spec §4.4).
		{
			VisualScriptPaneContext context = MakePaneContext();
			VisualScriptPaneResult result;
			m_LiveCommits.BeginFrame(context, result);
			ApplyPaneResult(std::move(result));
		}
		std::span<uint32_t const> const outlined = m_FindBar.GetOutlined();
		m_Outlined.assign(outlined.begin(), outlined.end());
		std::sort(m_Outlined.begin(), m_Outlined.end());
		m_LiveChange.reset();
		m_LiveActive = false;
		m_LiveDeactivated = false;
		m_LiveDrawn = false;
		// The widget under the mouse keeps its chip live: over it, the canvas's surface is not hovered, and the renderer, which
		// is told whether the surface is, would not draw it. While Space is held no chip is live (unless one is in use), so a
		// left press over a chip is the surface's and pans.
		bool const panKey = input.SpaceDown && !m_ActiveChip;
		m_HoveredChip.reset();
		if (!panKey && m_LiveWidget && m_LiveChip && m_Model.GetGesture().Kind == VisualScriptGesture::Idle && ImGui::IsWindowHovered() &&
		    ImGui::IsMouseHoveringRect(ToImVec2(m_LiveWidget->Frame.Min), ToImVec2(m_LiveWidget->Frame.Max), false))
		{
			m_HoveredChip = m_LiveChip;
		}
		m_LiveWidget.reset();
		m_LiveChip.reset();

		VisualScriptCanvasDrawInput draw;
		draw.DrawList = ImGui::GetWindowDrawList();
		draw.Style = MakeVisualScriptCanvasStyle(style, ImGui::GetFontSize());
		draw.Library = m_Library.get();
		draw.Time = m_Time;
		draw.Mouse = input.Mouse;
		draw.Hovered = surfaceHovered;
		draw.ReduceMotion = m_Settings.IsReduceMotion();
		draw.ActiveChip = m_ActiveChip ? &*m_ActiveChip : m_HoveredChip ? &*m_HoveredChip : nullptr;
		// Values are edited in place with the node library's types; without it they are only shown.
		draw.LiveChips = m_Library != nullptr && !panKey ? static_cast<VisualScriptLiveChipHost*>(this) : nullptr;
		draw.Outlined = m_Outlined;
		draw.Brushes = &m_Brushes;
		m_Renderer.Draw(m_Model, draw);
		m_PulsedLinks = m_Model.CommittedLinks().size();
		if (!m_LiveDrawn)
		{
			m_ActiveChip.reset();
		}
		CommitLiveChange();
		DrawLivePopup();

		UpdateCursor();
		ImVec2 const canvasMax(canvasMin.x + childSize.x, canvasMin.y + childSize.y);
		DrawCanvasOverlays(canvasMin, canvasMax);
		DrawFindBar(canvasMin, canvasMax);
		DrawProblemsPopup();
		DrawPalette();
		DrawFocusFrame(VisualScriptWindowPart::Canvas);
		ImGui::EndChild();
	}

	VisualScriptEditorPanel::LiveChipData const* VisualScriptEditorPanel::ResolveLiveChip(std::optional<LiveChipData>& slot, uint32_t node,
	                                                                                      std::string_view pin)
	{
		// What a chip shows and edits, found again only when the chip, the document shown (a pending widget value or the asset's
		// document, by identity, never kept alive), the analysis, the layout or the library changed: a widget held under the
		// mouse costs no lookups in the document from frame to frame (spec §7.12).
		Ref<VisualScriptDocument const> const pending = m_LiveCommits.GetPendingDocument(m_Model);
		Ref<VisualScriptDocument const> const& stored = m_Model.GetStoredDocument();
		VisualScriptAnalysis const* analysis = m_Model.GetAnalysis();
		uint64_t const analysisKey = analysis != nullptr ? m_Model.GetAnalysisCount() + 1 : 0;
		VisualScriptLibrary const* library = m_Library.get();
		if (slot && slot->Node == node && slot->Pin == pin && slot->Pending.lock() == pending && slot->Stored.lock() == stored &&
		    slot->Analysis == analysisKey && slot->Revision == m_Model.GetCanvasRevision() && slot->Library == library)
		{
			return slot->Editable ? &*slot : nullptr;
		}
		LiveChipData& data = slot.emplace();
		data.Node = node;
		data.Pin = pin;
		data.Pending = pending;
		data.Stored = stored;
		data.Analysis = analysisKey;
		data.Revision = m_Model.GetCanvasRevision();
		data.Library = library;
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		std::optional<uint32_t> const nodeIndex = canvas.FindNode(node);
		std::optional<uint32_t> const pinIndex = nodeIndex ? canvas.FindPin(*nodeIndex, pin, VisualScriptPinDirection::In) : std::nullopt;
		VisualScriptDocument const& document = m_LiveCommits.GetDocument(m_Model);
		VisualScriptNode const* found = FindShownNode(document, m_Model.GetFunction(), node);
		if (!pinIndex || found == nullptr)
		{
			return nullptr;
		}
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
		VisualScriptPinVisual const& visual = pins[*pinIndex];
		data.Type = visual.Type;
		data.Editable = VisualScriptLiteralEditor::CanEdit(data.Type, library);
		std::string_view const shown = canvas.GetText(visual.Label);
		data.Label = shown.empty() ? std::string(pin) : std::string(shown);
		VisualScriptAnalyzedNode const* analyzed = analysis != nullptr ? analysis->FindNode(node) : nullptr;
		data.StoredPin = VisualScriptEditing::ToStoredVisualScriptPin(analyzed, VisualScriptPinDirection::In, data.Pin);
		auto const entry = found->Inputs.is_object() ? found->Inputs.find(data.StoredPin) : found->Inputs.end();
		data.Literal = found->Inputs.is_object() && entry != found->Inputs.end() ? *entry : Json();
		std::optional<VisualScriptPin> const libraryPin =
			FindVisualScriptCanvasPin(m_Model, library, node, pin, VisualScriptPinDirection::In);
		data.Fallback = libraryPin ? libraryPin->Default : Json();
		data.Title = GetVisualScriptNodeTitle(document, analysis, *found);
		return data.Editable ? &data : nullptr;
	}

	void VisualScriptEditorPanel::DrawLiveChip(VisualScriptLiveChip const& chip)
	{
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
		if (chip.Pin >= pins.size())
		{
			return;
		}
		std::string_view const name = canvas.GetText(pins[chip.Pin].Name);
		// What the widget shows: the value not committed yet while one is pending, else the document's.
		LiveChipData const* data = ResolveLiveChip(m_LiveChipData, chip.Node, name);
		if (data == nullptr)
		{
			return;
		}
		VisualScriptLibrary const* library = m_Library.get();

		std::optional<Json> changed;
		bool popupRequested = false;
		bool active = false;
		bool deactivated = false;
		{
			VisualScriptLiveChipScope const scope(chip);
			ImGui::PushID("##liveChip");
			ImGui::PushID(static_cast<int>(chip.Node));
			ImGui::PushID(data->StoredPin.c_str());
			ImGui::BeginGroup();
			// The cells of a vector or quaternion follow one another CellGap apart (the scope's item spacing), each CellWidth
			// wide, so the widget's frames are the chip's cells. ImGui truncates item widths (CalcItemWidth, imgui.cpp:12485)
			// and the literal editor divides the width among the cells: rounding the whole up keeps each cell CellWidth wide
			// (a whole number) after both, where a fractional gap would make every cell a pixel narrower. A single cell is
			// rounded to the nearest pixel: a colour square is the frame's height, which may be fractional.
			uint32_t const cells = std::max(chip.CellCount, 1u);
			float const width = cells > 1
			                        ? std::ceil(chip.CellWidth * static_cast<float>(cells) + chip.CellGap * static_cast<float>(cells - 1))
			                        : std::round(chip.CellWidth);
			ImGui::SetNextItemWidth(width);
			changed = m_LiveLiteral.Draw(data->Type, data->Literal, data->Fallback, library, FieldInlineMode::Compact, &popupRequested);
			VisualScriptRect const last{ToVec2(ImGui::GetItemRectMin()), ToVec2(ImGui::GetItemRectMax())};
			ImGui::EndGroup();
			active = ImGui::IsItemActive();
			deactivated = ImGui::IsItemDeactivated();
			m_LiveWidget = VisualScriptLiveWidgetFrames{chip, {ToVec2(ImGui::GetItemRectMin()), ToVec2(ImGui::GetItemRectMax())}, last};
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
		}
		m_LiveDrawn = true;
		VisualScriptPinReference const reference{chip.Node, std::string(name)};
		m_LiveChip = reference;
		if (active)
		{
			m_ActiveChip = reference;
		}
		else if (m_ActiveChip == reference)
		{
			m_ActiveChip.reset();
		}
		m_LiveActive = active;
		m_LiveDeactivated = deactivated;
		if (changed)
		{
			m_LiveChange = LiveChange{chip.Node, data->StoredPin, data->Label, data->Title, std::move(*changed)};
		}
		if (popupRequested)
		{
			// Opened after the scope, at the window's normal size.
			m_LivePopup = LivePopup{reference};
		}
	}

	void VisualScriptEditorPanel::CommitLiveChange()
	{
		if (!m_LiveChange && !m_LiveDeactivated)
		{
			return;
		}
		VisualScriptPaneContext context = MakePaneContext();
		VisualScriptPaneResult result;
		if (m_LiveChange)
		{
			LiveChange const change = std::move(*m_LiveChange);
			m_LiveChange.reset();
			VisualScriptDocument edited = m_LiveCommits.GetDocument(m_Model);
			if (Result<void> const set = VisualScriptEditing::SetInput(edited, change.Node, change.Stored, change.Literal); !set)
			{
				AddToast(fmt::format("Could not set {}: {}", change.Label, set.GetError()));
			}
			else
			{
				m_LiveCommits.Change(context, std::move(edited), fmt::format("Set {} on {}", change.Label, change.Title),
				                     m_LiveActive && !m_LiveDeactivated, result);
			}
		}
		if (m_LiveDeactivated)
		{
			m_LiveCommits.End(context, result);
		}
		ApplyPaneResult(std::move(result));
	}

	void VisualScriptEditorPanel::DrawLivePopup()
	{
		if (!m_LivePopup)
		{
			return;
		}
		char const* const id = "##liveValue";
		if (m_LivePopup->OpenPending)
		{
			ImGui::OpenPopup(id);
			m_LivePopup->OpenPending = false;
		}
		if (!ImGui::BeginPopup(id))
		{
			m_LivePopup.reset();
			return;
		}
		// The chip's pin, found again by name: the popup stays open over frames that lay the canvas out again.
		VisualScriptPinReference const reference = m_LivePopup->Pin;
		LiveChipData const* data = ResolveLiveChip(m_LivePopupData, reference.Node, reference.Pin);
		if (data == nullptr || m_Model.IsReadOnly())
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			m_LivePopup.reset();
			return;
		}
		ImGui::BeginGroup();
		std::optional<Json> changed = m_LiveLiteral.DrawPopup(data->Type, data->Literal, data->Fallback, m_Library.get());
		ImGui::EndGroup();
		bool const active = ImGui::IsItemActive();
		ImGui::EndPopup();
		if (changed)
		{
			m_LiveChange = LiveChange{reference.Node, data->StoredPin, data->Label, data->Title, std::move(*changed)};
			// A pick in a list commits at once; a colour dragged in the picker merges until it is let go.
			m_LiveActive = active;
			m_LiveDeactivated = !active;
			CommitLiveChange();
		}
	}

	void VisualScriptEditorPanel::DrawFindBar(ImVec2 canvasMin, ImVec2 canvasMax)
	{
		if (!m_FindBar.IsOpen())
		{
			return;
		}
		// At the canvas's top-right, never wider than the canvas less a margin (spec §10.1 rule 4).
		ImGuiStyle const& style = ImGui::GetStyle();
		float const fontSize = ImGui::GetFontSize();
		float const width = VisualScriptFindBar::GetWidth(canvasMax.x - canvasMin.x, fontSize);
		if (width <= 0.0f)
		{
			return;
		}
		ImGuiWindow const* nav = GImGui->NavWindow;
		ImGui::SetCursorScreenPos(ImVec2(canvasMax.x - width - fontSize * 0.5f, canvasMin.y + fontSize * 0.5f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
		ImGui::BeginChild("##findBar", ImVec2(width, ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f),
		                  ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
		                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleColor();
		ImGuiID const bar = ImGui::GetCurrentWindow()->ID;
		VisualScriptPaneContext context = MakePaneContext();
		VisualScriptPaneResult result = m_FindBar.Draw(context);
		ImGui::EndChild();
		if (!m_FindBar.IsOpen() && nav != nullptr && nav->ID == bar)
		{
			// Closing the bar from its own field gives the keyboard back to the canvas.
			ImGui::SetWindowFocus();
		}
		ApplyPaneResult(std::move(result));
	}

	void VisualScriptEditorPanel::DrawPalette()
	{
		// Drawn every frame in the canvas's ID scope, where it opens its popup; it draws nothing while closed.
		VisualScriptPaneContext context = MakePaneContext();
		ApplyPaneResult(m_Palette.Draw(context));
	}

	void VisualScriptEditorPanel::DrawNodePreview(VisualScriptPaletteEntry const& entry, ImDrawList* drawList, ImVec2 position, float scale)
	{
		VisualScriptLibrary const* library = m_Library.get();
		if (library == nullptr || drawList == nullptr || scale <= 0.0f)
		{
			return;
		}
		// The node alone, laid out once per entry (and document, sub-graph, library and measurer) and drawn from that layout
		// while its tooltip shows.
		Ref<VisualScriptDocument const> const& stored = m_Model.GetStoredDocument();
		uint32_t const shown = m_Model.GetFunction();
		if (!m_NodePreview || m_NodePreview->NodeType != entry.NodeType || m_NodePreview->Settings != entry.Settings ||
		    m_NodePreview->Document.lock() != stored || m_NodePreview->Function != shown || m_NodePreview->Library != library ||
		    m_NodePreview->Signature != m_Measure.Signature)
		{
			m_NodePreview.emplace();
			NodePreview& preview = *m_NodePreview;
			preview.NodeType = entry.NodeType;
			preview.Settings = entry.Settings;
			preview.Document = stored;
			preview.Function = shown;
			preview.Library = library;
			preview.Signature = m_Measure.Signature;
			// With what its pins come from (the variables, the functions' signatures and locals), never the graphs, laid out as
			// the canvas lays out a node it has no analysis for.
			VisualScriptDocument const& display = m_Model.GetDisplayDocument();
			VisualScriptDocument scratch;
			scratch.Variables = display.Variables;
			scratch.Functions.reserve(display.Functions.size());
			for (VisualScriptFunction const& function : display.Functions)
			{
				VisualScriptFunction& copy = scratch.Functions.emplace_back();
				copy.ID = function.ID;
				copy.Name = function.Name;
				copy.Pure = function.Pure;
				copy.Inputs = function.Inputs;
				copy.Outputs = function.Outputs;
				copy.Locals = function.Locals;
			}
			uint32_t function = shown;
			VisualScriptGraph* graph = FindVisualScriptGraph(scratch, function);
			if (graph == nullptr)
			{
				function = 0;
				graph = &scratch.Graph;
			}
			VisualScriptNode node;
			node.ID = NodePreview::Node;
			node.Type = entry.NodeType;
			node.Settings = entry.Settings.is_object() ? entry.Settings : Json::object();
			node.Position = glm::vec2(0.0f);
			graph->Nodes.push_back(std::move(node));

			VisualScriptCanvasSource source;
			source.Display = &scratch;
			source.Function = function;
			source.Library = library;
			source.EnumLookup = [library](std::string_view type)
			{
				return library->FindEnum(type);
			};
			preview.Canvas.Sync(source, m_Measure, m_Theme);
			preview.Index = preview.Canvas.FindNode(NodePreview::Node);
			preview.ShownFunction = function;
		}
		NodePreview const& preview = *m_NodePreview;
		if (!preview.Index)
		{
			return;
		}
		std::span<VisualScriptNodeVisual const> const nodes = preview.Canvas.GetNodes();
		glm::vec2 const size = nodes[*preview.Index].Size;
		// A view whose canvas is the reserved area: the node's top-left at its top-left, scale pixels per canvas unit.
		float const uiScale = std::max(ImGui::GetStyle().FontScaleDpi, 0.01f);
		VisualScriptViewState state;
		state.Function = preview.ShownFunction;
		state.Center = size * 0.5f;
		state.Zoom = scale / uiScale;
		VisualScriptView view(state);
		view.SetCanvas(ToVec2(position), size * scale, uiScale);
		VisualScriptCanvasDrawInput input;
		input.DrawList = drawList;
		input.Style = MakeVisualScriptCanvasStyle(ImGui::GetStyle(), ImGui::GetFontSize());
		input.Library = library;
		input.Time = m_Time;
		input.ReduceMotion = true;
		input.Brushes = &m_Brushes;
		m_PreviewRenderer.DrawNodePreview(preview.Canvas, *preview.Index, view, input);
	}

	void VisualScriptEditorPanel::TrackProblemBadges(VisualScriptCanvasInput const& input)
	{
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		VisualScriptView const& view = m_Model.GetView();
		if (input.Pressed[0])
		{
			m_BadgePress.reset();
			m_BadgePressPoint = input.Mouse;
			VisualScriptHit const& hover = m_Model.GetHover();
			if (input.Ctrl || input.Shift || input.Alt || m_Model.GetAnalysis() == nullptr)
			{
				return;
			}
			if (hover.Kind == VisualScriptHitKind::Badge)
			{
				m_BadgePress = ProblemsPopup{hover.Element, std::nullopt};
			}
			else if (hover.Kind == VisualScriptHitKind::Link && hover.Link && view.GetDetailLevel() != VisualScriptDetailLevel::Far)
			{
				// The badge at half the wire's length, where the renderer draws it.
				std::optional<uint32_t> const link = FindLinkVisual(canvas, *hover.Link);
				std::span<VisualScriptLinkVisual const> const links = canvas.GetLinks();
				if (link && links[*link].HasFlag(VisualScriptLinkFlags::Error))
				{
					std::array<glm::vec2, 4> points = VisualScriptGeometry::GetLinkControlPoints(canvas, *link, m_Model.GetNodePreview());
					for (glm::vec2& point : points)
					{
						point = view.ToScreen(point);
					}
					VisualScriptGeometry::WireFlattening const flattening =
						VisualScriptGeometry::GetWireFlattening(view.GetDetailLevel(), input.CurveTessellationTolerance);
					std::vector<glm::vec2> polyline;
					VisualScriptGeometry::FlattenWire(points, flattening.Tolerance, flattening.SegmentCap, polyline);
					float const k = view.GetPixelsPerUnit();
					float const radius = VisualScriptGeometry::GetBadgeRadius(k, view.GetUIScale()) * k;
					if (!polyline.empty() && glm::distance(VisualScriptGeometry::GetPolylinePoint(polyline, 0.5f), input.Mouse) <= radius)
					{
						m_BadgePress = ProblemsPopup{std::nullopt, *hover.Link};
					}
				}
			}
			return;
		}
		if (input.Released[0] && m_BadgePress)
		{
			// A click, not the end of a drag that started on the badge.
			float const threshold = 4.0f * input.UIScale;
			if (glm::distance(input.Mouse, m_BadgePressPoint) <= threshold && m_Model.GetGesture().Kind == VisualScriptGesture::Idle)
			{
				m_Problems = *m_BadgePress;
				m_Problems->OpenPending = true;
			}
			m_BadgePress.reset();
		}
	}

	void VisualScriptEditorPanel::DrawProblemsPopup()
	{
		if (!m_Problems)
		{
			return;
		}
		char const* const id = "##problemsPopup";
		if (m_Problems->OpenPending)
		{
			ImGui::OpenPopup(id);
			m_Problems->OpenPending = false;
		}
		if (!ImGui::BeginPopup(id))
		{
			m_Problems.reset();
			m_PopupProblems.clear();
			return;
		}
		VisualScriptAnalysis const* analysis = m_Model.GetAnalysis();
		std::vector<VisualScriptDiagnostic>& problems = m_PopupProblems;
		problems.clear();
		if (m_Problems->Node)
		{
			problems = GetVisualScriptNodeProblems(analysis, *m_Problems->Node);
		}
		else if (m_Problems->Link && analysis != nullptr)
		{
			for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
			{
				if (IsDiagnosticOfLink(diagnostic, *m_Problems->Link))
				{
					problems.push_back(diagnostic);
				}
			}
		}
		VisualScriptPaneContext context = MakePaneContext();
		VisualScriptPaneResult result;
		m_ProblemCommits.BeginFrame(context, result);
		if (problems.empty())
		{
			ImGui::TextDisabled("No problems");
		}
		else
		{
			DrawVisualScriptProblems(context, problems, m_ProblemCommits, result);
		}
		// A fix done or a jump made: the popup has served.
		if (result.Commit || result.Open || !result.SelectAndFrame.empty() || result.ShowFunction)
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		ApplyPaneResult(std::move(result));
	}

	void VisualScriptEditorPanel::AcceptDrops(VisualScriptCanvasInput& input)
	{
		ImGuiPayload const* dragged = ImGui::GetDragDropPayload();
		if (dragged == nullptr || !ImGui::BeginDragDropTarget())
		{
			return;
		}
		// Every drop is validated (spec §9.4 rule 5): assets that are not registered, entities that no longer exist and what
		// the pin under the mouse cannot take are refused with the not-allowed cursor while they hover, and change nothing.
		bool refused = false;
		if (dragged->IsDataType(DragDropPayload::Asset))
		{
			AssetHandle const asset = UI::AcceptAssetDrop();
			if (asset.IsValid())
			{
				input.Drop = VisualScriptCanvasDrop{VisualScriptDropKind::Asset, asset.GetUUID().GetValue(), {}};
			}
			uint64_t value = 0;
			if (dragged->DataSize == static_cast<int>(sizeof(value)))
			{
				std::memcpy(&value, dragged->Data, sizeof(value));
			}
			refused =
				!AssetManager::IsValid(AssetHandle(UUID(value))) || IsDropRefusedByPin(VisualScriptDropKind::Asset, value, input.Mouse);
		}
		else if (dragged->IsDataType(DragDropPayload::VisualScriptElement))
		{
			// A row of My Script: this script's only (spec §9.4 rule 4).
			VisualScriptElementPayload element;
			bool const valid = dragged->DataSize == static_cast<int>(sizeof(element));
			if (valid)
			{
				std::memcpy(&element, dragged->Data, sizeof(element));
			}
			uint64_t const packed = valid ? PackVisualScriptElement(static_cast<VisualScriptElementKind>(element.Kind), element.ID) : 0;
			bool const ours = valid && element.Graph == m_Graph.GetUUID().GetValue() && UnpackVisualScriptElement(packed).has_value();
			refused = !ours || IsDropRefusedByPin(VisualScriptDropKind::Element, packed, input.Mouse);
			ImGuiDragDropFlags const flags =
				ours ? ImGuiDragDropFlags_None : ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::VisualScriptElement, flags))
			{
				if (ours)
				{
					input.Drop = VisualScriptCanvasDrop{VisualScriptDropKind::Element, packed, {}};
				}
				else if (payload->IsDelivery())
				{
					AddToast(std::string(VisualScriptRefusals::OtherScriptRow));
				}
			}
		}
		else if (dragged->IsDataType(DragDropPayload::Entities))
		{
			std::vector<UUID> const entities = UI::ReadEntityPayload(*dragged);
			Scene& scene = m_Operations.GetContext().GetScene();
			bool const missing = entities.empty() || std::any_of(entities.begin(), entities.end(),
			                                                     [&scene](UUID entity)
			                                                     {
																	 return !scene.HasEntity(entity);
																 });
			// A pin that cannot take entities shows the cursor, and its drop is still delivered: the controller says why.
			refused = missing || IsDropRefusedByPin(VisualScriptDropKind::Entities, 0, input.Mouse);
			if (!missing)
			{
				if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::Entities))
				{
					VisualScriptCanvasDrop drop;
					drop.Kind = VisualScriptDropKind::Entities;
					for (UUID const entity : UI::ReadEntityPayload(*payload))
					{
						drop.Entities.push_back(entity.GetValue());
					}
					input.Drop = std::move(drop);
				}
			}
		}
		else
		{
			// Folders, files and scripts are nothing a graph can hold.
			refused = true;
		}
		if (refused)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
		}
		ImGui::EndDragDropTarget();
	}

	bool VisualScriptEditorPanel::IsDropRefusedByPin(VisualScriptDropKind kind, uint64_t payload, glm::vec2 mouse)
	{
		// What the model's drop will hit (it hit-tests the drop point at delivery), with what hit testing needs of the frame drawn
		// last: the payload hovers, so no gesture is under way.
		VisualScriptHitContext context;
		VisualScriptMinimapState const& minimap = m_Model.GetMinimap();
		if (minimap.Alpha > 0.0f && minimap.Transform.Scale > 0.0f)
		{
			context.Minimap = minimap.Transform.Rect;
		}
		context.Preview = m_Model.GetNodePreview();
		context.DynamicLinks = m_Model.GetDynamicLinks();
		context.CurveTessellationTolerance = ImGui::GetStyle().CurveTessellationTol;
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		HitTestCanvas(canvas, m_Model.GetIndex(), m_Model.GetView(), context, mouse, m_DropHit);
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins();
		return m_DropHit.Kind == VisualScriptHitKind::Pin && m_DropHit.Pin < pins.size() &&
		       !CanDropOnVisualScriptPin(kind, payload, pins[m_DropHit.Pin]);
	}

	void VisualScriptEditorPanel::UpdateCanvas(VisualScriptCanvasInput& input)
	{
		std::vector<VisualScriptEditorIntent> const intents = m_Model.Update(input);
		for (VisualScriptEditorIntent const& intent : intents)
		{
			ExecuteIntent(intent);
		}
	}

	void VisualScriptEditorPanel::UpdateCursor()
	{
		VisualScriptGestureView const gesture = m_Model.GetGesture();
		if (!m_CanvasHovered && gesture.Kind == VisualScriptGesture::Idle)
		{
			return;
		}
		if (gesture.SpaceHeld || gesture.Kind == VisualScriptGesture::Panning)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}
		else if ((gesture.Kind == VisualScriptGesture::Wiring || gesture.Kind == VisualScriptGesture::Relinking) && gesture.GhostInvalid)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
		}
		else if (gesture.Kind == VisualScriptGesture::ResizingComment)
		{
			ImGui::SetMouseCursor(GetResizeCursor(gesture.ResizeEdges));
		}
		else if (gesture.Kind == VisualScriptGesture::Idle && m_Model.GetHover().Kind == VisualScriptHitKind::CommentEdge &&
		         !m_Model.IsReadOnly())
		{
			ImGui::SetMouseCursor(GetResizeCursor(m_Model.GetHover().Edges));
		}
	}

	void VisualScriptEditorPanel::DrawCanvasOverlays(ImVec2 canvasMin, ImVec2 canvasMax)
	{
		DrawCard(canvasMin, canvasMax);
		DrawTextEdit();
		DrawHeaderTooltip();
		DrawGestureHint(canvasMin, canvasMax);
		DrawCanvasHint(canvasMin, canvasMax);
		DrawToasts(canvasMin, canvasMax);
	}

	void VisualScriptEditorPanel::DrawHeaderTooltip()
	{
		VisualScriptHit const& hover = m_Model.GetHover();
		if (!m_CanvasHovered || hover.Kind != VisualScriptHitKind::NodeHeader || m_Model.GetGesture().Kind != VisualScriptGesture::Idle ||
		    ImGui::IsAnyItemActive())
		{
			m_HoveredHeader.reset();
			return;
		}
		if (m_HoveredHeader != hover.Element)
		{
			m_HoveredHeader = hover.Element;
			m_HoveredHeaderSince = m_Time;
		}
		// The header waits ImGui's normal delay, longer than the pins' short one, so it never covers a pin being aimed at.
		if (m_Time - m_HoveredHeaderSince < static_cast<double>(ImGui::GetStyle().HoverDelayNormal))
		{
			return;
		}
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		std::optional<uint32_t> const index = canvas.FindNode(hover.Element);
		VisualScriptNode const* node = FindShownNode(m_Model.GetDisplayDocument(), m_Model.GetFunction(), hover.Element);
		if (!index || node == nullptr)
		{
			return;
		}
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		VisualScriptNodeVisual const& visual = nodes[*index];
		VisualScriptNodeTemplate const* entry = m_Library != nullptr ? m_Library->FindNode(node->Type) : nullptr;
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * TooltipWrap);
		std::string const titleText(canvas.GetText(visual.Title));
		ImGui::TextUnformatted(titleText.c_str());
		std::string_view const subtitle = canvas.GetText(visual.Subtitle);
		if (!subtitle.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(subtitle.data(), subtitle.data() + subtitle.size());
			ImGui::PopStyleColor();
		}
		if (entry != nullptr && !entry->Description.empty())
		{
			ImGui::Separator();
			ImGui::TextUnformatted(entry->Description.c_str());
		}
		// The member it stands for, in the fixed-width font when the editor gave one.
		if (m_Services.MonospaceFont != nullptr)
		{
			ImGui::PushFont(m_Services.MonospaceFont, 0.0f);
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextUnformatted(node->Type.c_str());
		ImGui::PopStyleColor();
		if (m_Services.MonospaceFont != nullptr)
		{
			ImGui::PopFont();
		}
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	void VisualScriptEditorPanel::DrawGestureHint(ImVec2 canvasMin, ImVec2 canvasMax)
	{
		VisualScriptGestureView const gesture = m_Model.GetGesture();
		std::string_view hint;
		switch (gesture.Kind)
		{
			case VisualScriptGesture::Wiring:
			case VisualScriptGesture::Relinking:
				hint = Texts::WireHint;
				break;
			case VisualScriptGesture::MovingElements:
				hint = m_Settings.IsSnapToGrid() ? Texts::MoveSnappedHint : Texts::MoveFreeHint;
				break;
			case VisualScriptGesture::Idle:
			case VisualScriptGesture::Panning:
				if (gesture.SpaceHeld)
				{
					hint = Texts::PanHint;
				}
				break;
			case VisualScriptGesture::Marquee:
			case VisualScriptGesture::ResizingComment:
			case VisualScriptGesture::Cutting:
			case VisualScriptGesture::MinimapDrag:
				break;
		}
		m_HintShown = !hint.empty();
		if (!m_HintShown)
		{
			return;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		ImVec2 const size = ImGui::CalcTextSize(hint.data(), hint.data() + hint.size());
		ImVec2 const lower((canvasMin.x + canvasMax.x - size.x) * 0.5f - style.FramePadding.x,
		                   canvasMax.y - size.y - style.FramePadding.y * 2.0f - style.WindowPadding.y);
		ImVec2 const upper(lower.x + size.x + style.FramePadding.x * 2.0f, lower.y + size.y + style.FramePadding.y * 2.0f);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(lower, upper, WithAlpha(m_Theme.Body, HintBackgroundAlpha), style.FrameRounding);
		drawList->AddText(ImVec2(lower.x + style.FramePadding.x, lower.y + style.FramePadding.y), m_Theme.TextDisabled, hint.data(),
		                  hint.data() + hint.size());
	}

	void VisualScriptEditorPanel::DrawToasts(ImVec2 canvasMin, ImVec2 canvasMax)
	{
		if (m_Toasts.empty())
		{
			return;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		// Above the first canvas's hint while it shows.
		float y = canvasMax.y - style.WindowPadding.y - m_RibbonHeight;
		float const maxWidth = std::max((canvasMax.x - canvasMin.x) * 0.6f, ImGui::GetFontSize() * 8.0f);
		std::optional<size_t> dismissed;
		// The newest toast is the lowest; older ones stack above it.
		for (size_t i = m_Toasts.size(); i-- > 0;)
		{
			Toast& toast = m_Toasts[i];
			bool const hasButtons = toast.ShowConsole && m_Services.ShowConsole;
			float const textWidth = std::min(ImGui::CalcTextSize(toast.Text.c_str()).x, maxWidth);
			ImVec2 const textSize = ImGui::CalcTextSize(toast.Text.c_str(), nullptr, false, maxWidth);
			float const buttons =
				ImGui::GetFrameHeight() + (toast.HasShow ? ImGui::CalcTextSize("Show").x + style.FramePadding.x * 2.0f : 0.0f) +
				(hasButtons ? ImGui::CalcTextSize("Show Console").x + style.FramePadding.x * 2.0f : 0.0f) + style.ItemSpacing.x * 3.0f;
			ImVec2 const size(textWidth + buttons + style.WindowPadding.x * 2.0f,
			                  std::max(textSize.y, ImGui::GetFrameHeight()) + style.WindowPadding.y * 2.0f);
			y -= size.y;
			ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + style.WindowPadding.x, y));
			y -= style.ItemSpacing.y;
			ImGui::PushID(static_cast<int>(i));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
			ImGui::BeginChild("##toast", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
			                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
			ImGui::PopStyleColor();
			// Hovering keeps a toast: it leaves ToastSeconds after the mouse left it.
			if (toast.Expires && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			{
				toast.Expires = m_Time + ToastSeconds;
			}
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxWidth);
			if (!toast.Expires)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(VisualScriptErrorColor));
			}
			ImGui::TextUnformatted(toast.Text.c_str());
			if (!toast.Expires)
			{
				ImGui::PopStyleColor();
			}
			ImGui::PopTextWrapPos();
			if (toast.HasShow)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Show"))
				{
					if (!toast.ShowElements.empty())
					{
						// Elements an outside change touched beyond the view: framed, in their sub-graph.
						if (toast.ShowFunction != m_Model.GetFunction())
						{
							VisualScriptOpenRequest request;
							request.Graph = m_Graph;
							request.Function = toast.ShowFunction;
							request.Select = toast.ShowElements;
							request.Frame = VisualScriptFrame::Selection;
							request.Focus = false;
							m_Model.ApplyOpenRequest(request);
						}
						else
						{
							FrameElements(toast.ShowElements);
						}
					}
					else if (toast.ShowAsset.IsValid())
					{
						// Another script's step: the window manager opens that script's window, or brings it to the front when
						// it is open (spec §10.4).
						VisualScriptOpenRequest request;
						request.Graph = toast.ShowAsset;
						m_OtherGraphRequests.push_back(std::move(request));
					}
					else
					{
						ImGui::SetWindowFocus("Scene View");
					}
					dismissed = i;
				}
			}
			if (hasButtons)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Show Console"))
				{
					m_Services.ShowConsole();
				}
			}
			ImGui::SameLine();
			if (GlyphButton("##dismiss", Glyph::Close, false, true, "Dismiss"))
			{
				dismissed = i;
			}
			ImGui::EndChild();
			ImGui::PopID();
			if (y < canvasMin.y)
			{
				break;
			}
		}
		if (dismissed)
		{
			m_Toasts.erase(m_Toasts.begin() + static_cast<std::ptrdiff_t>(*dismissed));
		}
	}

	void VisualScriptEditorPanel::OpenTextEdit(VisualScriptTextEditRequest const& request)
	{
		TextEdit edit;
		edit.Kind = request.Kind;
		edit.Element = request.Element;
		edit.Text = request.Text;
		edit.Original = request.Text;
		m_TextEdit = std::move(edit);
	}

	void VisualScriptEditorPanel::DrawTextEdit()
	{
		if (!m_TextEdit)
		{
			return;
		}
		TextEdit& edit = *m_TextEdit;
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		VisualScriptView const& view = m_Model.GetView();
		float const fontSize = ImGui::GetFontSize();
		ImGuiStyle const& style = ImGui::GetStyle();
		ImVec2 position;
		float width = TextEditWidth * fontSize;
		if (edit.Kind == VisualScriptTextEditKind::CommentTitle)
		{
			std::optional<uint32_t> const index = canvas.FindComment(edit.Element);
			if (!index)
			{
				CloseTextEdit();
				return;
			}
			std::span<VisualScriptCommentVisual const> const comments = canvas.GetComments();
			VisualScriptCommentVisual const& comment = comments[*index];
			VisualScriptRect const bar = VisualScriptGeometry::GetCommentBarRect(comment, view.GetPixelsPerUnit(), view.GetUIScale());
			// Over the bar, from the canvas's left edge when the bar starts left of it, as the title is drawn
			// (VisualScriptGeometry::GetCommentTitleRect).
			glm::vec2 const upper = view.ToScreen(bar.Max);
			glm::vec2 const lower(std::max(view.ToScreen(bar.Min).x, m_CanvasMin.x), view.ToScreen(bar.Min).y);
			position = ImVec2(lower.x, (lower.y + upper.y - ImGui::GetFrameHeight()) * 0.5f);
			width = std::max(upper.x - lower.x, fontSize * 6.0f);
		}
		else
		{
			std::optional<uint32_t> const index = canvas.FindNode(edit.Element);
			if (!index)
			{
				CloseTextEdit();
				return;
			}
			std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
			VisualScriptNodeVisual const& node = nodes[*index];
			glm::vec2 const topLeft = view.ToScreen(node.Position);
			// Over the node's bubble when it has one, else just above the node, as the bubble will be.
			float const height = fontSize * TextEditLines + style.FramePadding.y * 2.0f;
			position = ImVec2(topLeft.x, topLeft.y - height - style.ItemSpacing.y);
		}

		ImGui::SetCursorScreenPos(position);
		ImGui::PushID("##textEdit");
		if (edit.Focus)
		{
			ImGui::SetKeyboardFocusHere();
			edit.Focus = false;
		}
		ImGui::SetNextItemWidth(width);
		bool confirmed = false;
		if (edit.Kind == VisualScriptTextEditKind::CommentTitle)
		{
			confirmed = ImGui::InputText("##title", &edit.Text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		}
		else
		{
			// Enter starts a new line; Ctrl+Enter (and a click elsewhere) commits.
			confirmed = ImGui::InputTextMultiline("##nodeComment", &edit.Text,
			                                      ImVec2(width, fontSize * TextEditLines + style.FramePadding.y * 2.0f),
			                                      ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SetItemTooltip("Ctrl+Enter or a click elsewhere: done \xC2\xB7 Esc: cancel");
		}
		bool const active = ImGui::IsItemActive();
		bool const deactivated = edit.WasActive && !active;
		bool const cancelled = deactivated && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		edit.WasActive = edit.WasActive || active;
		ImGui::PopID();
		if (cancelled)
		{
			CloseTextEdit();
		}
		else if (confirmed || deactivated)
		{
			CommitTextEdit();
		}
	}

	void VisualScriptEditorPanel::CloseTextEdit()
	{
		if (m_TextEdit && m_TextEdit->Kind == VisualScriptTextEditKind::CommentTitle)
		{
			// The comment's title field ends the run of its creation.
			m_NameRun.reset();
		}
		m_TextEdit.reset();
	}

	void VisualScriptEditorPanel::CommitTextEdit()
	{
		if (!m_TextEdit)
		{
			return;
		}
		TextEdit const edit = std::move(*m_TextEdit);
		m_TextEdit.reset();
		if (edit.Kind == VisualScriptTextEditKind::NodeComment)
		{
			m_NameRun.reset();
			if (edit.Text != edit.Original)
			{
				// A widget interaction (spec §4.4): it commits under the shared session's key, whose step the window manager
				// ends once the field is no longer the active item.
				ExecuteEdit(VisualScriptNodeCommentEdit{edit.Element, edit.Text}, m_Session.GetMergeKey());
			}
			return;
		}
		NameRun const* run = FindNameRun();
		uint64_t const key = run != nullptr ? run->Key : 0;
		Ref<VisualScriptDocument const> const stored = run != nullptr ? run->Stored : nullptr;
		m_NameRun.reset();
		if (edit.Text == edit.Original)
		{
			return;
		}
		VisualScriptCommentEdit rename;
		rename.Action = VisualScriptCommentAction::Rename;
		rename.Comment = edit.Element;
		rename.Text = edit.Text;
		ExecuteEdit(rename, key, stored.get());
	}

	VisualScriptEditorPanel::NameRun const* VisualScriptEditorPanel::FindNameRun()
	{
		if (!m_NameRun)
		{
			return nullptr;
		}
		Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(m_Graph);
		if (asset == nullptr || asset->GetSharedDocument() != m_NameRun->Written)
		{
			m_NameRun.reset();
			return nullptr;
		}
		return &*m_NameRun;
	}

	void VisualScriptEditorPanel::AddToast(std::string text, bool failure)
	{
		Toast toast;
		toast.Text = std::move(text);
		if (!failure)
		{
			toast.Expires = m_Time + ToastSeconds;
		}
		toast.ShowConsole = failure;
		m_Toasts.push_back(std::move(toast));
		if (m_Toasts.size() > MaxToasts)
		{
			m_Toasts.erase(m_Toasts.begin());
		}
	}

	VisualScriptEditContext VisualScriptEditorPanel::MakeContext(uint64_t mergeKey) const
	{
		VisualScriptEditContext context(m_Model);
		context.Library = m_Library.get();
		context.ScriptName = m_ScriptName;
		context.Graph = m_Graph;
		context.MergeKey = mergeKey;
		context.CommentColor = m_Settings.GetCommentColor();
		context.Scene = &m_SceneContext;
		return context;
	}

	VisualScriptPaneContext VisualScriptEditorPanel::MakePaneContext()
	{
		// The palette's previews are drawn as the canvas draws nodes; My Script's live values are the ones the window reads
		// for the context entity, so the running game is asked once per interval.
		std::function<void(VisualScriptPaletteEntry const&, ImDrawList*, ImVec2, float)> preview =
			[this](VisualScriptPaletteEntry const& entry, ImDrawList* drawList, ImVec2 position, float scale)
		{
			DrawNodePreview(entry, drawList, position, scale);
		};
		std::function<Result<ScriptFieldMap>(UUID)> live = [this](UUID entity) -> Result<ScriptFieldMap>
		{
			if (!m_LiveValues || entity != GetPresentContextEntity())
			{
				return Error{"the context entity has no running instance"};
			}
			return *m_LiveValues;
		};
		return {m_Model,
		        m_Semantics,
		        m_Settings,
		        m_Session,
		        m_Services,
		        m_PaneShared,
		        m_Graph,
		        m_ScriptName,
		        GetPresentContextEntity(),
		        m_Operations.GetContext().IsPlaying(),
		        m_Time,
		        m_Measure,
		        std::move(preview),
		        std::move(live)};
	}

	void VisualScriptEditorPanel::ApplyPaneResult(VisualScriptPaneResult result)
	{
		m_PanesWidgetActive = m_PanesWidgetActive || result.WidgetActive;
		Ref<VisualScriptDocument const> const before = m_Model.GetStoredDocument();
		bool committed = false;
		if (result.Commit)
		{
			committed = Commit(std::move(*result.Commit), {});
		}
		if (result.EditCommentTitle && committed)
		{
			// The comment's first title merges into the step that created it (spec §9.6 rule 1).
			uint32_t const comment = result.EditCommentTitle->Comment;
			m_NameRun = NameRun{result.EditCommentTitle->MergeKey, before, m_Model.GetStoredDocument()};
			std::string title = VisualScriptPalettePopup::NewCommentText;
			if (VisualScriptGraph const* graph = FindVisualScriptGraph(m_Model.GetDisplayDocument(), m_Model.GetFunction()))
			{
				for (VisualScriptComment const& candidate : graph->Comments)
				{
					if (candidate.ID == comment)
					{
						title = candidate.Text.substr(0, candidate.Text.find('\n'));
					}
				}
			}
			OpenTextEdit(VisualScriptTextEditRequest{VisualScriptTextEditKind::CommentTitle, comment, std::move(title)});
		}
		if (result.FocusSetting)
		{
			m_PaneShared.FocusSetting = result.FocusSetting;
		}
		if (result.ShowFunction)
		{
			m_Model.ShowFunction(*result.ShowFunction);
		}
		if (result.Open)
		{
			if (result.Open->Graph == m_Graph)
			{
				m_Model.ApplyOpenRequest(*result.Open);
			}
			else
			{
				m_OtherGraphRequests.push_back(std::move(*result.Open));
			}
		}
		if (!result.SelectAndFrame.empty())
		{
			m_Model.ApplyOpenRequest(MakeVisualScriptJump(m_Graph, m_Model.GetFunction(), result.SelectAndFrame));
		}
		if (result.FindReferences)
		{
			m_Drawer.ShowReferences(*result.FindReferences);
			ShowDrawer(VisualScriptDrawerMode::Find);
		}
		for (std::string& note : result.Notes)
		{
			AddToast(std::move(note));
		}
	}

	uint64_t VisualScriptEditorPanel::GetNudgeKey()
	{
		// A run of presses less than VisualScriptNudgeRunSeconds apart is one step; a later press starts another.
		if (m_NudgeKey == 0 || m_Time - m_LastNudge >= VisualScriptNudgeRunSeconds)
		{
			m_NudgeKey = VisualScriptNudgeMergeKey + ++m_NudgeRuns;
		}
		m_LastNudge = m_Time;
		return m_NudgeKey;
	}

	bool VisualScriptEditorPanel::HandlePanelAction(VisualScriptEditorAction action)
	{
		switch (action)
		{
			case Action::Find:
				m_FindBar.Open();
				return true;
			case Action::FindInProject:
				m_Drawer.ShowFind();
				ShowDrawer(VisualScriptDrawerMode::Find);
				return true;
			case Action::ShowShortcuts:
				m_OpenShortcuts = !m_ShortcutsOpen;
				return true;
			default:
				return false;
		}
	}

	void VisualScriptEditorPanel::ExecuteIntent(VisualScriptEditorIntent const& intent)
	{
		uint64_t key = 0;
		std::string clipboard;
		bool commentBox = false;
		if (VisualScriptActionIntent const* action = std::get_if<VisualScriptActionIntent>(&intent))
		{
			if (HandlePanelAction(action->Action))
			{
				return;
			}
			switch (action->Action)
			{
				case Action::NudgeLeft:
				case Action::NudgeRight:
				case Action::NudgeUp:
				case Action::NudgeDown:
				case Action::NudgeLeftFine:
				case Action::NudgeRightFine:
				case Action::NudgeUpFine:
				case Action::NudgeDownFine:
					key = GetNudgeKey();
					break;
				case Action::AddCommentBox:
					// The box and its first title are one step.
					key = VisualScriptNameMergeKey + ++m_NameRuns;
					commentBox = true;
					break;
				case Action::Paste:
					// Read only when pasting: the clipboard may hold anything.
					if (char const* text = ImGui::GetClipboardText(); text != nullptr)
					{
						clipboard = text;
					}
					break;
				default:
					break;
			}
		}
		if (std::holds_alternative<VisualScriptDropIntent>(intent))
		{
			// Entity drops also set the new variable on the context entity, when it runs this graph in the edited scene
			// (drops are refused while playing): its name and values are read now, not every frame.
			m_SceneContext.ContextEntity = UUID::Invalid();
			m_SceneContext.ContextEntityName.clear();
			m_SceneContext.ContextVariables.clear();
			UUID const context = GetPresentContextEntity();
			if (!m_SceneContext.Playing && RunsGraph(context))
			{
				Entity const entity = m_Operations.GetContext().GetScene().GetEntityByUUID(context);
				m_SceneContext.ContextEntity = context;
				m_SceneContext.ContextEntityName = entity.GetName();
				m_SceneContext.ContextVariables = entity.GetComponent<VisualScriptComponent>().Variables;
			}
		}
		Ref<VisualScriptDocument const> const before = m_Model.GetStoredDocument();
		VisualScriptEditContext context = MakeContext(key);
		context.ClipboardText = clipboard;
		bool const committed = HandleResult(ResolveVisualScriptIntent(intent, context), "Editing");
		if (commentBox && committed)
		{
			m_NameRun = NameRun{key, before, m_Model.GetStoredDocument()};
		}
	}

	void VisualScriptEditorPanel::ExecuteEdit(VisualScriptCanvasEdit const& edit, uint64_t mergeKey, VisualScriptDocument const* stored)
	{
		VisualScriptEditContext context = MakeContext(mergeKey);
		context.Stored = stored;
		HandleResult(ResolveVisualScriptEdit(edit, context), "Editing");
	}

	bool VisualScriptEditorPanel::HandleResult(Result<VisualScriptControllerResult> result, std::string_view action)
	{
		if (!result)
		{
			// An edit the canvas asked for could not be made: an editor bug, never the user's data.
			m_Model.CancelGesture();
			UI::ReportFailure(result, fmt::format("{} {}", action, m_ScriptName));
			AddToast(fmt::format("Could not edit {}: {}", m_ScriptName, result.GetError()), true);
			return false;
		}
		VisualScriptControllerResult value = result.TakeValue();
		bool committed = false;
		if (value.Commit)
		{
			committed = Commit(std::move(*value.Commit), std::move(value.ComponentEdits));
			if (committed && !value.GlideFrom.empty() && !m_Settings.IsReduceMotion())
			{
				m_Renderer.StartGlide(value.GlideFrom, m_Time);
			}
		}
		if (value.Clipboard)
		{
			ImGui::SetClipboardText(value.Clipboard->c_str());
		}
		for (std::string& note : value.Notes)
		{
			AddToast(std::move(note));
		}
		if (value.ShowFunction)
		{
			m_Model.ShowFunction(*value.ShowFunction);
		}
		if (value.NeedsRuntime)
		{
			// Refused for want of the scripting runtime: the degraded notice says why.
			m_BlinkStart = m_Time;
			m_Activity.DegradedBlinks++;
		}
		if (value.OpenPalette)
		{
			// Drawn later in this frame, which keeps the characters of the intent's query from being typed twice.
			m_Palette.Open(*value.OpenPalette);
		}
		if (value.DropMenu)
		{
			m_Menu = {};
			m_Menu.Kind = MenuKind::ElementDrop;
			m_Menu.OpenPending = true;
			m_Menu.Armed = true;
			m_Menu.Position = value.DropMenu->Drop.Position;
			m_Menu.ScreenPosition = m_Model.GetView().ToScreen(value.DropMenu->Drop.Position);
			m_Menu.Drop = std::move(value.DropMenu);
		}
		if (value.ContextMenu)
		{
			m_Menu = {};
			switch (value.ContextMenu->Target.Kind)
			{
				case VisualScriptHitKind::NodeHeader:
				case VisualScriptHitKind::Node:
				case VisualScriptHitKind::Badge:
				case VisualScriptHitKind::Reroute:
					m_Menu.Kind = MenuKind::Node;
					break;
				case VisualScriptHitKind::Pin:
					m_Menu.Kind = value.ContextMenu->Pin ? MenuKind::Pin : MenuKind::None;
					break;
				case VisualScriptHitKind::Link:
					m_Menu.Kind = value.ContextMenu->Target.Link ? MenuKind::Link : MenuKind::None;
					break;
				case VisualScriptHitKind::CommentEdge:
				case VisualScriptHitKind::CommentTitle:
					m_Menu.Kind = MenuKind::Comment;
					break;
				case VisualScriptHitKind::None:
				case VisualScriptHitKind::Minimap:
				case VisualScriptHitKind::Background:
					break;
			}
			m_Menu.OpenPending = m_Menu.Kind != MenuKind::None;
			// Opened at a right release: nothing of the click is left to pick an item.
			m_Menu.Armed = true;
			m_Menu.Context = std::move(*value.ContextMenu);
			m_Menu.Position = m_Model.GetView().ToCanvas(m_Menu.Context.ScreenPosition);
		}
		if (value.PinMenu)
		{
			m_Menu = {};
			m_Menu.Kind = MenuKind::AnchorPins;
			m_Menu.OpenPending = true;
			// Opened at a left press: the release that follows must not pick the item under the mouse.
			m_Menu.Armed = false;
			m_Menu.Anchor = *value.PinMenu;
			m_Menu.Position = m_Model.GetView().ToCanvas(value.PinMenu->ScreenPosition);
		}
		if (value.Open)
		{
			m_Model.ApplyOpenRequest(*value.Open);
		}
		if (value.FindReferences)
		{
			m_Drawer.ShowReferences(*value.FindReferences);
			ShowDrawer(VisualScriptDrawerMode::Find);
		}
		if (value.EditText)
		{
			OpenTextEdit(*value.EditText);
		}
		if (value.Reveal)
		{
			VisualScriptReveal const reveal = *value.Reveal;
			if (reveal.Kind == VisualScriptRevealKind::Node)
			{
				// Details shows the canvas's selection, which the node is.
				m_PaneShared.Element.reset();
				if (m_Layout.DetailsState == VisualScriptPaneState::AutoHidden)
				{
					m_DetailsOverlay = true;
				}
			}
			else
			{
				// A row of My Script, selected (shown in Details), renamed when asked.
				VisualScriptElement element;
				std::optional<uint32_t> show;
				switch (reveal.Kind)
				{
					case VisualScriptRevealKind::Variable:
						element = {VisualScriptElementKind::Variable, reveal.ID};
						break;
					case VisualScriptRevealKind::Local:
						element = {VisualScriptElementKind::Local, reveal.ID};
						for (VisualScriptFunction const& function : m_Model.GetDisplayDocument().Functions)
						{
							for (VisualScriptLocal const& local : function.Locals)
							{
								if (local.ID == reveal.ID)
								{
									show = function.ID;
								}
							}
						}
						break;
					case VisualScriptRevealKind::Function:
						element = {VisualScriptElementKind::Function, reveal.ID};
						break;
					case VisualScriptRevealKind::CustomEvent:
						element = {VisualScriptElementKind::CustomEvent, reveal.ID};
						break;
					case VisualScriptRevealKind::Node:
						break;
				}
				VisualScriptPaneContext context = MakePaneContext();
				SelectVisualScriptElement(context, element, show);
				if (reveal.Rename && !m_Model.IsReadOnly())
				{
					// My Script's rename field: shown over the canvas when My Script is not beside it.
					m_PaneShared.Rename = element;
					OpenSidebarOverlay();
				}
				else if (m_Layout.SidebarState == VisualScriptPaneState::AutoHidden)
				{
					m_SidebarOverlay = true;
				}
			}
		}
		return committed;
	}

	bool VisualScriptEditorPanel::Commit(VisualScriptCommitRequest request, std::vector<ComponentEdit> edits)
	{
		Ref<VisualScriptDocument const> const document = request.Document;
		std::string const description = request.Description;
		m_LastCommitTimings = request.Timings;
		Result<void> const result = edits.empty()
		                                ? m_Operations.EditVisualScript(m_Graph, document, std::move(request.Description), request.MergeKey,
		                                                                std::move(request.Text))
		                                : m_Operations.EditVisualScriptAndComponents(m_Graph, document, std::move(request.Description),
		                                                                             std::move(edits), std::move(request.Text));
		if (!result)
		{
			m_Model.CancelGesture();
			UI::ReportFailure(result, description);
			AddToast(fmt::format("Could not save {}: {}", m_ScriptName, result.GetError()), true);
			m_SaveState = VisualScriptSaveState::NotSaved;
			m_Model.CheckWritable();
			return false;
		}
		m_SaveState = VisualScriptSaveState::Saved;
		// The file now holds the canonical text of the document: nothing of the repairs it needed is left in it.
		m_RepairNotice = false;
		// The frame that commits draws the committed document: the model adopts it as the window's own change now.
		m_Model.OnCommitted(document);
		SyncModel();
		if (!request.Select.empty())
		{
			m_Model.Select(request.Select);
		}
		for (std::string& note : request.Notes)
		{
			AddToast(std::move(note));
		}
		return true;
	}

	void VisualScriptEditorPanel::DrawMenus()
	{
		if (m_Menu.Kind == MenuKind::None)
		{
			return;
		}
		char const* id = "##nodeMenu";
		switch (m_Menu.Kind)
		{
			case MenuKind::Node:
				id = "##nodeMenu";
				break;
			case MenuKind::Pin:
				id = "##pinMenu";
				break;
			case MenuKind::Link:
				id = "##linkMenu";
				break;
			case MenuKind::Comment:
				id = "##commentMenu";
				break;
			case MenuKind::AnchorPins:
				id = "##anchorMenu";
				break;
			case MenuKind::ElementDrop:
				id = "##elementDropMenu";
				break;
			case MenuKind::None:
				return;
		}
		if (m_Menu.OpenPending)
		{
			// Opened here, in the window's ID scope where it is drawn, never by BeginPopupContextItem: a right drag pans.
			ImGui::OpenPopup(id);
			glm::vec2 const at = m_Menu.Kind == MenuKind::AnchorPins    ? m_Menu.Anchor.ScreenPosition
			                     : m_Menu.Kind == MenuKind::ElementDrop ? m_Menu.ScreenPosition
			                                                            : m_Menu.Context.ScreenPosition;
			ImGui::SetNextWindowPos(ToImVec2(at));
			m_Menu.OpenPending = false;
		}
		if (!ImGui::BeginPopup(id))
		{
			m_Menu = {};
			return;
		}
		ImGuiIO const& io = ImGui::GetIO();
		if (!m_Menu.Armed && !io.MouseDown[ImGuiMouseButton_Left] && !io.MouseReleased[ImGuiMouseButton_Left])
		{
			m_Menu.Armed = true;
		}
		switch (m_Menu.Kind)
		{
			case MenuKind::Node:
				DrawNodeMenu();
				break;
			case MenuKind::Pin:
				DrawPinMenu();
				break;
			case MenuKind::Link:
				DrawLinkMenu();
				break;
			case MenuKind::Comment:
				DrawCommentMenu();
				break;
			case MenuKind::AnchorPins:
				DrawAnchorMenu();
				break;
			case MenuKind::ElementDrop:
				DrawElementDropMenu();
				break;
			case MenuKind::None:
				break;
		}
		ImGui::EndPopup();
		// A pick runs once the menu is drawn: its edit lays the canvas out again, which the menu's items were drawn from.
		if (m_MenuPick)
		{
			std::function<void()> const pick = std::move(m_MenuPick);
			m_MenuPick = nullptr;
			pick();
		}
	}

	void VisualScriptEditorPanel::DrawNodeMenu()
	{
		std::vector<VisualScriptMenuItem> const items = BuildVisualScriptNodeMenu(MakeContext(0));
		if (items.empty())
		{
			ImGui::CloseCurrentPopup();
			return;
		}
		std::optional<Action> picked;
		std::string_view openSubmenu;
		bool submenuShown = false;
		auto const closeSubmenu = [&]
		{
			if (!openSubmenu.empty() && submenuShown)
			{
				ImGui::EndMenu();
			}
			openSubmenu = {};
			submenuShown = false;
		};
		for (VisualScriptMenuItem const& item : items)
		{
			if (item.Submenu != openSubmenu)
			{
				closeSubmenu();
				if (item.SeparatorBefore)
				{
					ImGui::Separator();
				}
				if (!item.Submenu.empty())
				{
					openSubmenu = item.Submenu;
					submenuShown = ImGui::BeginMenu(std::string(item.Submenu).c_str());
				}
			}
			else if (item.SeparatorBefore && (openSubmenu.empty() || submenuShown))
			{
				ImGui::Separator();
			}
			if (!openSubmenu.empty() && !submenuShown)
			{
				continue;
			}
			std::string const shortcut = GetShortcutText(item.Action);
			if (ImGui::MenuItem(item.Label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), item.Checked, item.Enabled))
			{
				picked = item.Action;
			}
			if (!item.Enabled && !item.Reason.empty() &&
			    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", item.Reason.c_str());
			}
		}
		closeSubmenu();
		if (picked && m_Menu.Armed)
		{
			std::vector<uint32_t> selection(m_Model.GetSelection().begin(), m_Model.GetSelection().end());
			m_MenuPick = [this, action = *picked, selection = std::move(selection), position = m_Menu.Position]
			{
				ExecuteIntent(VisualScriptActionIntent{action, selection, position});
			};
		}
	}

	void VisualScriptEditorPanel::DrawPinMenu()
	{
		VisualScriptContextMenuIntent const& menu = m_Menu.Context;
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		std::optional<uint32_t> const nodeIndex = canvas.FindNode(menu.Pin->Node);
		std::optional<uint32_t> const pinIndex =
			nodeIndex ? canvas.FindPin(*nodeIndex, menu.Pin->Pin, menu.PinDirection) : std::optional<uint32_t>();
		if (!pinIndex)
		{
			// The pin went (an outside change): its menu has nothing left to act on.
			ImGui::CloseCurrentPopup();
			return;
		}
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		std::span<VisualScriptPinVisual const> const allPins = canvas.GetPins();
		std::span<VisualScriptLinkVisual const> const links = canvas.GetLinks();
		VisualScriptNodeVisual const& node = nodes[*nodeIndex];
		VisualScriptNode const* stored = FindShownNode(m_Model.GetDisplayDocument(), m_Model.GetFunction(), node.ID);
		bool const armed = m_Menu.Armed;

		auto const pinItems = [&](uint32_t pin, bool wholeSide)
		{
			VisualScriptPinVisual const& visual = allPins[pin];
			VisualScriptPinReference const reference{node.ID, std::string(canvas.GetText(visual.Name))};
			auto const edit = [&](VisualScriptPinAction action, std::optional<VisualScriptLink> link = std::nullopt)
			{
				VisualScriptPinEdit pinEdit;
				pinEdit.Action = action;
				pinEdit.Pin = reference;
				pinEdit.Direction = visual.Direction;
				pinEdit.WholeSide = wholeSide;
				pinEdit.Link = std::move(link);
				m_MenuPick = [this, pinEdit]
				{
					ExecuteEdit(pinEdit);
				};
			};
			bool const linked = visual.HasFlag(VisualScriptPinFlags::Linked);
			bool const exec = visual.Shape == VisualScriptPinShape::Exec;
			bool const readOnly = m_Model.IsReadOnly();
			if (ImGui::MenuItem("Break All Links", GetGestureText(VisualScriptMouseGestureId::BreakPinLinks).c_str(), false,
			                    linked && !readOnly) &&
			    armed)
			{
				edit(VisualScriptPinAction::BreakAllLinks);
			}
			if (wholeSide)
			{
				return;
			}
			std::vector<uint32_t> pinLinks;
			for (uint32_t const link : canvas.GetIncidentLinks(*nodeIndex))
			{
				VisualScriptLinkVisual const& candidate = links[link];
				if ((visual.Direction == VisualScriptPinDirection::In && candidate.ToPin == pin) ||
				    (visual.Direction == VisualScriptPinDirection::Out && candidate.FromPin == pin))
				{
					pinLinks.push_back(link);
				}
			}
			if (ImGui::BeginMenu("Break Link To", !pinLinks.empty() && !readOnly))
			{
				for (uint32_t const link : pinLinks)
				{
					VisualScriptLinkVisual const& candidate = links[link];
					bool const in = visual.Direction == VisualScriptPinDirection::In;
					VisualScriptNodeVisual const& other = nodes[in ? candidate.FromNode : candidate.ToNode];
					VisualScriptPinVisual const& otherPin = allPins[in ? candidate.FromPin : candidate.ToPin];
					std::string_view const otherLabel =
						canvas.GetText(otherPin.Label).empty() ? canvas.GetText(otherPin.Name) : canvas.GetText(otherPin.Label);
					std::string const label = fmt::format("{} ({})##{}", otherLabel, canvas.GetText(other.Title), link);
					if (ImGui::MenuItem(label.c_str()) && armed)
					{
						edit(VisualScriptPinAction::BreakLink, canvas.GetDocumentLink(link));
					}
				}
				ImGui::EndMenu();
			}
			// Literals are stored under the input's stored name, which a redirected member's renamed pin does not show.
			VisualScriptAnalysis const* analysis = m_Model.GetAnalysis();
			std::string const storedName = VisualScriptEditing::ToStoredVisualScriptPin(
				analysis != nullptr ? analysis->FindNode(node.ID) : nullptr, VisualScriptPinDirection::In, reference.Pin);
			bool const resettable = visual.Direction == VisualScriptPinDirection::In && stored != nullptr && stored->Inputs.is_object() &&
			                        stored->Inputs.contains(storedName);
			if (ImGui::MenuItem("Reset to Default", nullptr, false, resettable && !readOnly) && armed)
			{
				edit(VisualScriptPinAction::ResetToDefault);
			}
			if (visual.HasFlag(VisualScriptPinFlags::Splittable) &&
			    ImGui::MenuItem("Split Struct Pin", nullptr, false, !readOnly && m_Library != nullptr) && armed)
			{
				edit(VisualScriptPinAction::Split);
			}
			if (visual.HasFlag(VisualScriptPinFlags::SplitField) &&
			    ImGui::MenuItem("Recombine Struct Pin", nullptr, false, !readOnly && m_Library != nullptr) && armed)
			{
				edit(VisualScriptPinAction::Recombine);
			}
			bool const data = !exec && visual.Type != VisualScriptWildcardType;
			if (data)
			{
				// Variables and function inputs cannot hold arrays (v2 §3.2).
				bool const array = visual.Shape == VisualScriptPinShape::Array;
				auto const arrayReason = [array]
				{
					if (array && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
					{
						ImGui::SetTooltip("%s", VisualScriptRefusals::PromoteArray.data());
					}
				};
				ImGui::Separator();
				if (ImGui::MenuItem("Promote to Variable", nullptr, false, !readOnly && m_Library != nullptr && !array) && armed)
				{
					edit(VisualScriptPinAction::PromoteToVariable);
				}
				arrayReason();
				if (m_Model.GetFunction() != 0 && visual.Direction == VisualScriptPinDirection::In && !linked)
				{
					if (ImGui::MenuItem("Promote to Function Input", nullptr, false, !readOnly && m_Library != nullptr && !array) && armed)
					{
						edit(VisualScriptPinAction::PromoteToFunctionInput);
					}
					arrayReason();
				}
			}
			if (stored != nullptr && (stored->Type == "Event.Send" || stored->Type == "Event.Custom" || stored->Type == "Function.Call"))
			{
				ImGui::Separator();
				if (ImGui::MenuItem("Find References") && armed)
				{
					m_MenuPick = [this, id = node.ID, position = m_Menu.Position]
					{
						ExecuteIntent(VisualScriptActionIntent{Action::FindReferences, {id}, position});
					};
				}
			}
		};

		if (!menu.WholeSide)
		{
			pinItems(*pinIndex, false);
			return;
		}
		// A collapsed node's anchor stands for every pin of its side: the side's links at once, or each pin's own items.
		pinItems(*pinIndex, true);
		ImGui::Separator();
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins(node);
		for (size_t i = 0; i < pins.size(); i++)
		{
			VisualScriptPinVisual const& pin = pins[i];
			if (pin.Direction != menu.PinDirection)
			{
				continue;
			}
			std::string_view const label = canvas.GetText(pin.Label).empty() ? canvas.GetText(pin.Name) : canvas.GetText(pin.Label);
			if (ImGui::BeginMenu(fmt::format("{}##{}", label, i).c_str()))
			{
				pinItems(node.FirstPin + static_cast<uint32_t>(i), false);
				ImGui::EndMenu();
			}
		}
	}

	void VisualScriptEditorPanel::DrawLinkMenu()
	{
		VisualScriptLink const link = *m_Menu.Context.Target.Link;
		bool const editable = !m_Model.IsReadOnly();
		if (ImGui::MenuItem("Break Link", GetGestureText(VisualScriptMouseGestureId::BreakLink).c_str(), false, editable) && m_Menu.Armed)
		{
			VisualScriptPinEdit edit;
			edit.Action = VisualScriptPinAction::BreakLink;
			edit.Pin = link.To;
			edit.Direction = VisualScriptPinDirection::In;
			edit.Link = link;
			m_MenuPick = [this, edit]
			{
				ExecuteEdit(edit);
			};
		}
		if (ImGui::MenuItem("Add Reroute", GetGestureText(VisualScriptMouseGestureId::InsertReroute).c_str(), false,
		                    editable && m_Library != nullptr) &&
		    m_Menu.Armed)
		{
			m_MenuPick = [this, link, position = m_Menu.Position]
			{
				ExecuteIntent(VisualScriptInsertRerouteIntent{link, position});
			};
		}
	}

	void VisualScriptEditorPanel::DrawCommentMenu()
	{
		uint32_t const comment = m_Menu.Context.Target.Element;
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		std::optional<uint32_t> const index = canvas.FindComment(comment);
		if (!index)
		{
			ImGui::CloseCurrentPopup();
			return;
		}
		std::span<VisualScriptCommentVisual const> const comments = canvas.GetComments();
		VisualScriptCommentVisual const& visual = comments[*index];
		bool const editable = !m_Model.IsReadOnly();
		bool const armed = m_Menu.Armed;
		auto const edit = [&](VisualScriptCommentAction action, glm::vec4 color = glm::vec4(0.0f))
		{
			VisualScriptCommentEdit commentEdit;
			commentEdit.Action = action;
			commentEdit.Comment = comment;
			commentEdit.Color = color;
			m_MenuPick = [this, commentEdit]
			{
				ExecuteEdit(commentEdit);
			};
		};
		if (ImGui::MenuItem("Rename", GetShortcutText(Action::Rename).c_str(), false, editable) && armed)
		{
			OpenTextEdit({VisualScriptTextEditKind::CommentTitle, comment, std::string(canvas.GetText(visual.Title))});
		}
		if (ImGui::BeginMenu("Colour", editable))
		{
			for (VisualScriptCommentSwatch const& swatch : VisualScriptCommentSwatches)
			{
				glm::vec4 const color = swatch.Color;
				ImVec4 const shown(Math::LinearToSrgb(color.r), Math::LinearToSrgb(color.g), Math::LinearToSrgb(color.b), 1.0f);
				ImGui::ColorButton(fmt::format("##{}", swatch.Name).c_str(), shown, ImGuiColorEditFlags_NoTooltip,
				                   ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
				ImGui::SameLine();
				if (ImGui::MenuItem(std::string(swatch.Name).c_str()) && armed)
				{
					m_Settings.SetCommentColor(color);
					edit(VisualScriptCommentAction::Recolour, color);
				}
			}
			ImGui::Separator();
			if (ImGui::BeginMenu("Custom..."))
			{
				// Edited in sRGB as the picker shows colours; documents store linear RGBA.
				VisualScriptComment const* current = nullptr;
				if (VisualScriptGraph const* graph = FindVisualScriptGraph(m_Model.GetDisplayDocument(), m_Model.GetFunction()))
				{
					for (VisualScriptComment const& candidate : graph->Comments)
					{
						if (candidate.ID == comment)
						{
							current = &candidate;
						}
					}
				}
				if (current != nullptr)
				{
					if (!m_CustomColor || m_CustomColorComment != comment)
					{
						glm::vec4 const linear = current->Color;
						m_CustomColor =
							glm::vec4(Math::LinearToSrgb(linear.r), Math::LinearToSrgb(linear.g), Math::LinearToSrgb(linear.b), linear.a);
						m_CustomColorComment = comment;
					}
					ImGui::ColorPicker4("##custom", &m_CustomColor->x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview);
					if (ImGui::IsItemDeactivatedAfterEdit())
					{
						glm::vec4 const srgb = *m_CustomColor;
						glm::vec4 const linear(Math::SrgbToLinear(srgb.r), Math::SrgbToLinear(srgb.g), Math::SrgbToLinear(srgb.b), srgb.a);
						m_Settings.SetCommentColor(linear);
						edit(VisualScriptCommentAction::Recolour, linear);
					}
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Select Contents") && armed)
		{
			std::vector<uint32_t> contents;
			auto const inside = [&visual](VisualScriptRect const& rect)
			{
				return rect.Min.x >= visual.Rect.Min.x && rect.Min.y >= visual.Rect.Min.y && rect.Max.x <= visual.Rect.Max.x &&
				       rect.Max.y <= visual.Rect.Max.y;
			};
			for (VisualScriptNodeVisual const& node : canvas.GetNodes())
			{
				if (inside(VisualScriptGeometry::GetNodeRect(node)))
				{
					contents.push_back(node.ID);
				}
			}
			for (VisualScriptCommentVisual const& other : canvas.GetComments())
			{
				if (other.ID != comment && inside(other.Rect))
				{
					contents.push_back(other.ID);
				}
			}
			m_Model.Select(contents);
		}
		if (ImGui::MenuItem("Fit to Contents", nullptr, false, editable) && armed)
		{
			edit(VisualScriptCommentAction::FitToContents);
		}
		if (ImGui::MenuItem("Move Comment Contents", nullptr, m_Settings.IsMoveCommentContents()) && armed)
		{
			m_Settings.SetMoveCommentContents(!m_Settings.IsMoveCommentContents());
		}
		ImGui::SetItemTooltip("Moving a comment box by its title moves what is inside it (Alt inverts it for a drag)");
		ImGui::Separator();
		if (ImGui::MenuItem("Delete Comment", nullptr, false, editable) && armed)
		{
			edit(VisualScriptCommentAction::Delete);
		}
		if (ImGui::MenuItem("Delete Comment and Contents", nullptr, false, editable) && armed)
		{
			edit(VisualScriptCommentAction::DeleteWithContents);
		}
	}

	void VisualScriptEditorPanel::DrawAnchorMenu()
	{
		VisualScriptPinMenuIntent const& anchor = m_Menu.Anchor;
		VisualScriptCanvasModel const& canvas = m_Model.GetCanvas();
		std::optional<uint32_t> const nodeIndex = canvas.FindNode(anchor.Node);
		if (!nodeIndex)
		{
			ImGui::CloseCurrentPopup();
			return;
		}
		std::span<VisualScriptNodeVisual const> const nodes = canvas.GetNodes();
		VisualScriptNodeVisual const& node = nodes[*nodeIndex];
		std::span<VisualScriptPinVisual const> const pins = canvas.GetPins(node);
		for (size_t i = 0; i < pins.size(); i++)
		{
			VisualScriptPinVisual const& pin = pins[i];
			if (pin.Direction != anchor.Direction || pin.HasFlag(VisualScriptPinFlags::Orphan))
			{
				continue;
			}
			std::string_view const name = canvas.GetText(pin.Name);
			std::string_view const label = canvas.GetText(pin.Label).empty() ? name : canvas.GetText(pin.Label);
			ImVec2 const swatchMin = ImGui::GetCursorScreenPos();
			float const radius = ImGui::GetFontSize() * 0.25f;
			ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetTextLineHeight()));
			ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(swatchMin.x + radius, swatchMin.y + ImGui::GetTextLineHeight() * 0.5f),
			                                            radius, pin.Color);
			ImGui::SameLine();
			if (ImGui::Selectable(fmt::format("{}##{}", label, i).c_str()) && m_Menu.Armed)
			{
				// The wire follows the mouse from the chosen pin and ends at the next press.
				m_MenuPick = [this, node = anchor.Node, pin = std::string(name), direction = anchor.Direction]
				{
					m_Model.StartWire(node, pin, direction);
				};
			}
		}
	}

	void VisualScriptEditorPanel::DrawElementDropMenu()
	{
		if (!m_Menu.Drop)
		{
			ImGui::CloseCurrentPopup();
			return;
		}
		// The node a dropped variable makes: Ctrl+drop makes a Get, Alt+drop a Set without asking.
		bool const mac = ImGui::GetIO().ConfigMacOSXBehaviors;
		std::string const name = m_Menu.Drop->Name;
		std::optional<uint8_t> modifiers;
		if (ImGui::MenuItem(fmt::format("Get {}", name).c_str(), mac ? "Cmd+Drop" : "Ctrl+Drop"))
		{
			modifiers = VisualScriptDropModifiers::Ctrl;
		}
		if (ImGui::MenuItem(fmt::format("Set {}", name).c_str(), mac ? "Option+Drop" : "Alt+Drop", false, !m_Model.IsReadOnly()))
		{
			modifiers = VisualScriptDropModifiers::Alt;
		}
		if (modifiers && m_Menu.Armed)
		{
			VisualScriptDropIntent drop = m_Menu.Drop->Drop;
			drop.Modifiers = *modifiers;
			m_MenuPick = [this, drop = std::move(drop)]
			{
				ExecuteIntent(drop);
			};
		}
	}
}
