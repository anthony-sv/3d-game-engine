#include "Editor/Panels/ContentBrowserPanel.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/AssetThumbnails.h"
#include "Editor/FileDialogs.h"
#include "Editor/ScriptBrowsing.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/ImGui/ImGuiRenderer.h"
#include "Strada/Platform/Desktop.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ScriptProject.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <numbers>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		// Tiles of the slate theme: cards with a type band, glyphs in the type's accent color.
		constexpr ImU32 CardColor = IM_COL32(45, 50, 59, 255);
		constexpr ImU32 HoveredCardColor = IM_COL32(53, 59, 70, 255);
		constexpr ImU32 CardBorderColor = IM_COL32(62, 68, 79, 255);
		// Selection in the theme's accent, a lighter outline under the mouse.
		constexpr ImU32 SelectionColor = IM_COL32(90, 157, 255, 255);
		constexpr ImU32 SelectedLabelColor = IM_COL32(46, 101, 196, 255);
		constexpr ImU32 SelectedFolderColor = IM_COL32(90, 157, 255, 46);
		constexpr ImU32 HoverOutlineColor = IM_COL32(107, 115, 133, 255);
		constexpr ImU32 HoveredFolderColor = IM_COL32(255, 255, 255, 14);
		constexpr ImU32 PaperColor = IM_COL32(70, 77, 90, 255);
		constexpr ImU32 PaperFoldColor = IM_COL32(96, 104, 120, 255);
		constexpr ImU32 InkColor = IM_COL32(228, 231, 236, 255);
		constexpr ImU32 MutedInkColor = IM_COL32(134, 140, 152, 255);
		constexpr ImU32 FolderBackColor = IM_COL32(176, 134, 58, 255);
		constexpr ImU32 FolderFrontColor = IM_COL32(222, 178, 82, 255);
		constexpr ImU32 FolderEdgeColor = IM_COL32(242, 208, 132, 255);
		constexpr ImU32 MissingBadgeColor = IM_COL32(235, 92, 80, 255);
		constexpr ImVec4 MissingColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
		constexpr ImU32 BrokenBadgeColor = IM_COL32(232, 164, 56, 255);
		constexpr ImVec4 BrokenColor = ImVec4(0.93f, 0.68f, 0.30f, 1.0f);
		// The notice about an import: red for failures, amber for skipped files, on a dark tint of the color.
		constexpr ImU32 NoticeErrorColor = IM_COL32(235, 92, 80, 255);
		constexpr ImU32 NoticeWarningColor = IM_COL32(232, 164, 56, 255);
		// Failures listed by the notice; the console has them all.
		constexpr size_t NoticeErrorLines = 3;
		// Tiles scale with the font (and so with the display scale).
		constexpr float TileSizeInFontSizes = 5.5f;
		// Folders changed outside the editor show up after at most this many seconds.
		constexpr double RescanInterval = 1.0;
		// Frames a new rename field may take to receive keyboard focus before the rename is dropped.
		constexpr int RenameFocusFrames = 3;
		constexpr char const* DeletePopup = "Delete###ContentBrowserDelete";

		struct TypeStyle
		{
			char const* Label;
			// Shown when the label does not fit the tile.
			char const* ShortLabel;
			ImU32 Accent;
		};

		TypeStyle GetTypeStyle(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:
					return {"SCENE", "SCN", IM_COL32(86, 184, 116, 255)};
				case AssetType::Prefab:
					return {"PREFAB", "PFB", IM_COL32(82, 150, 236, 255)};
				case AssetType::Mesh:
					return {"MESH", "MSH", IM_COL32(236, 146, 70, 255)};
				case AssetType::Material:
					return {"MATERIAL", "MAT", IM_COL32(190, 114, 228, 255)};
				case AssetType::Texture:
					return {"TEXTURE", "TEX", IM_COL32(64, 184, 196, 255)};
				case AssetType::Environment:
					return {"HDR SKY", "HDR", IM_COL32(228, 194, 76, 255)};
				case AssetType::AudioClip:
					return {"AUDIO", "AUD", IM_COL32(236, 98, 152, 255)};
				case AssetType::Font:
					return {"FONT", "FNT", IM_COL32(156, 166, 188, 255)};
				case AssetType::VisualScript:
					return {"VISUAL SCRIPT", "VS", IM_COL32(150, 206, 72, 255)};
				case AssetType::None:
					break;
			}
			return {"FILE", "?", IM_COL32(120, 126, 138, 255)};
		}

		constexpr TypeStyle ScriptStyle = {"C# SCRIPT", "C#", IM_COL32(138, 118, 244, 255)};

		ImU32 Shade(ImU32 color, float factor, float alpha = 1.0f)
		{
			ImVec4 const value = ImGui::ColorConvertU32ToFloat4(color);
			return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(value.x * factor, 1.0f), std::min(value.y * factor, 1.0f),
			                                             std::min(value.z * factor, 1.0f), value.w * alpha));
		}

		ImU32 Mix(ImU32 from, ImU32 to, float amount)
		{
			ImVec4 const a = ImGui::ColorConvertU32ToFloat4(from);
			ImVec4 const b = ImGui::ColorConvertU32ToFloat4(to);
			return ImGui::ColorConvertFloat4ToU32(
				ImVec4(a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount, a.z + (b.z - a.z) * amount, a.w + (b.w - a.w) * amount));
		}

		// A rectangle in screen pixels.
		struct Box
		{
			ImVec2 Min;
			ImVec2 Max;

			float Width() const { return Max.x - Min.x; }
			float Height() const { return Max.y - Min.y; }
			ImVec2 Center() const { return ImVec2((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f); }
			// A point given in fractions of the size from the top-left corner.
			ImVec2 At(float x, float y) const { return ImVec2(Min.x + Width() * x, Min.y + Height() * y); }
		};

		// The largest square centered in the box, with a margin of a fraction of its side on every edge.
		Box Square(Box const& box, float margin)
		{
			float const side = std::min(box.Width(), box.Height()) * (1.0f - margin * 2.0f);
			ImVec2 const center = box.Center();
			return {ImVec2(center.x - side * 0.5f, center.y - side * 0.5f), ImVec2(center.x + side * 0.5f, center.y + side * 0.5f)};
		}

		float GetBandLabelSize()
		{
			return std::round(ImGui::GetFontSize() * 0.72f);
		}

		float GetCardRounding(Box const& card)
		{
			return std::max(3.0f, std::round(card.Width() * 0.06f));
		}

		void DrawCenteredText(ImDrawList* drawList, float fontSize, ImVec2 center, ImU32 color, char const* text)
		{
			ImFont* const font = ImGui::GetFont();
			ImVec2 const size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
			drawList->AddText(font, fontSize, ImVec2(std::round(center.x - size.x * 0.5f), std::round(center.y - size.y * 0.5f)), color,
			                  text);
		}

		// A card with the type's label on a band at its bottom; returns the area above the band.
		Box DrawCard(ImDrawList* drawList, Box const& card, TypeStyle const& style, ImU32 color)
		{
			float const rounding = GetCardRounding(card);
			float const labelSize = GetBandLabelSize();
			float const bandHeight = std::round(labelSize * 1.6f);
			ImVec2 const bandMin(card.Min.x, card.Max.y - bandHeight);
			drawList->AddRectFilled(card.Min, card.Max, color, rounding);
			drawList->AddRectFilled(bandMin, card.Max, Shade(style.Accent, 0.6f), rounding, ImDrawFlags_RoundCornersBottom);
			drawList->AddLine(bandMin, ImVec2(card.Max.x - 1.0f, bandMin.y), Shade(style.Accent, 0.9f), 1.0f);
			drawList->AddRect(card.Min, card.Max, CardBorderColor, rounding);
			char const* label = style.Label;
			if (ImGui::GetFont()->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, label).x > card.Width() - labelSize * 0.8f)
			{
				label = style.ShortLabel;
			}
			DrawCenteredText(drawList, labelSize, ImVec2(card.Center().x, bandMin.y + bandHeight * 0.5f), IM_COL32(255, 255, 255, 240),
			                 label);
			return {card.Min, ImVec2(card.Max.x, bandMin.y)};
		}

		float GetStroke(Box const& box)
		{
			return std::max(1.5f, box.Width() * 0.035f);
		}

		// A page with a folded corner, as wide as the given fraction of the box.
		Box DrawPage(ImDrawList* drawList, Box const& box, float widthFraction)
		{
			float const width = box.Width() * widthFraction;
			Box const page = {ImVec2(box.Center().x - width * 0.5f, box.Min.y), ImVec2(box.Center().x + width * 0.5f, box.Max.y)};
			float const fold = width * 0.3f;
			float const rounding = std::max(2.0f, width * 0.06f);
			drawList->AddRectFilled(page.Min, ImVec2(page.Max.x - fold, page.Max.y), PaperColor, rounding, ImDrawFlags_RoundCornersLeft);
			drawList->AddRectFilled(ImVec2(page.Max.x - fold - 1.0f, page.Min.y + fold), page.Max, PaperColor, rounding,
			                        ImDrawFlags_RoundCornersBottomRight);
			drawList->AddTriangleFilled(ImVec2(page.Max.x - fold - 1.0f, page.Min.y), ImVec2(page.Max.x, page.Min.y + fold),
			                            ImVec2(page.Max.x - fold - 1.0f, page.Min.y + fold), PaperFoldColor);
			return page;
		}

		// Scenes: a globe, the scene's world.
		void DrawSceneGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			ImVec2 const center = box.Center();
			float const radius = box.Width() * 0.46f;
			float const stroke = GetStroke(box);
			ImU32 const line = Shade(accent, 1.2f);
			drawList->AddCircleFilled(center, radius, Shade(accent, 0.42f, 0.7f));
			drawList->AddEllipse(center, ImVec2(radius * 0.42f, radius), line, 0.0f, 0, stroke);
			drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), line, stroke);
			drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), line, stroke);
			for (float const latitude : {-0.5f, 0.5f})
			{
				float const halfWidth = radius * std::sqrt(1.0f - latitude * latitude);
				float const y = center.y + radius * latitude;
				drawList->AddLine(ImVec2(center.x - halfWidth, y), ImVec2(center.x + halfWidth, y), line, stroke);
			}
			drawList->AddCircle(center, radius, Shade(accent, 1.45f), 0, stroke * 1.2f);
		}

		// Prefabs: a cube of three shaded faces.
		void DrawPrefabGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			ImVec2 const center = box.Center();
			float const size = box.Width() * 0.47f;
			float const side = size * 0.87f;
			ImVec2 const top(center.x, center.y - size);
			ImVec2 const left(center.x - side, center.y - size * 0.5f);
			ImVec2 const right(center.x + side, center.y - size * 0.5f);
			ImVec2 const middle(center.x, center.y);
			ImVec2 const bottom(center.x, center.y + size);
			ImVec2 const bottomLeft(center.x - side, center.y + size * 0.5f);
			ImVec2 const bottomRight(center.x + side, center.y + size * 0.5f);
			drawList->AddQuadFilled(top, right, middle, left, Shade(accent, 1.35f));
			drawList->AddQuadFilled(left, middle, bottom, bottomLeft, accent);
			drawList->AddQuadFilled(middle, right, bottomRight, bottom, Shade(accent, 0.66f));
			ImU32 const edge = Shade(accent, 1.6f, 0.9f);
			float const stroke = std::max(1.0f, GetStroke(box) * 0.7f);
			drawList->AddLine(left, middle, edge, stroke);
			drawList->AddLine(middle, right, edge, stroke);
			drawList->AddLine(middle, bottom, edge, stroke);
		}

		// Meshes: a low-polygon wireframe (an icosahedron seen along one of its axes).
		void DrawMeshGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			ImVec2 const center = box.Center();
			float const radius = box.Width() * 0.47f;
			auto const pointAt = [&center](float angleDegrees, float distance)
			{
				float const angle = angleDegrees * std::numbers::pi_v<float> / 180.0f;
				return ImVec2(center.x + std::cos(angle) * distance, center.y + std::sin(angle) * distance);
			};
			std::array<ImVec2, 6> outer;
			for (size_t index = 0; index < outer.size(); index++)
			{
				outer[index] = pointAt(-90.0f + 60.0f * static_cast<float>(index), radius);
			}
			std::array<ImVec2, 3> inner;
			for (size_t index = 0; index < inner.size(); index++)
			{
				inner[index] = pointAt(-90.0f + 120.0f * static_cast<float>(index), radius * 0.5f);
			}
			drawList->AddConvexPolyFilled(outer.data(), static_cast<int>(outer.size()), Shade(accent, 0.45f, 0.75f));
			drawList->AddTriangleFilled(inner[0], inner[1], inner[2], Shade(accent, 0.75f, 0.85f));
			ImU32 const line = Shade(accent, 1.25f);
			float const stroke = std::max(1.0f, GetStroke(box) * 0.75f);
			drawList->AddPolyline(outer.data(), static_cast<int>(outer.size()), line, stroke, ImDrawFlags_Closed);
			drawList->AddPolyline(inner.data(), static_cast<int>(inner.size()), line, stroke, ImDrawFlags_Closed);
			for (size_t index = 0; index < inner.size(); index++)
			{
				// Each inner corner meets the outer corner in its direction and the two beside it.
				size_t const facing = index * 2;
				drawList->AddLine(inner[index], outer[facing], line, stroke);
				drawList->AddLine(inner[index], outer[(facing + 1) % outer.size()], line, stroke);
				drawList->AddLine(inner[index], outer[(facing + outer.size() - 1) % outer.size()], line, stroke);
			}
			float const dot = stroke * 1.3f;
			for (ImVec2 const& corner : outer)
			{
				drawList->AddCircleFilled(corner, dot, Shade(accent, 1.6f));
			}
			for (ImVec2 const& corner : inner)
			{
				drawList->AddCircleFilled(corner, dot, Shade(accent, 1.6f));
			}
		}

		// Materials: a shaded ball.
		void DrawMaterialGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			ImVec2 const center = box.Center();
			float const radius = box.Width() * 0.46f;
			constexpr int Steps = 24;
			ImU32 const dark = Shade(accent, 0.38f);
			ImU32 const light = Shade(accent, 1.5f);
			for (int step = 0; step < Steps; step++)
			{
				float const t = static_cast<float>(step) / static_cast<float>(Steps - 1);
				ImVec2 const position(center.x - radius * 0.3f * t, center.y - radius * 0.34f * t);
				drawList->AddCircleFilled(position, radius * (1.0f - 0.8f * t), Mix(dark, light, t * t));
			}
			drawList->AddCircleFilled(ImVec2(center.x - radius * 0.38f, center.y - radius * 0.42f), radius * 0.1f,
			                          IM_COL32(255, 255, 255, 190));
		}

		// A landscape picture frame: the frame's box.
		Box GetPictureFrame(Box const& box)
		{
			return {box.At(0.0f, 0.14f), box.At(1.0f, 0.86f)};
		}

		// Textures: a picture of hills under a sun.
		void DrawTextureGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			Box const frame = GetPictureFrame(box);
			float const rounding = std::max(2.0f, frame.Width() * 0.05f);
			drawList->AddRectFilled(frame.Min, frame.Max, Shade(accent, 0.32f), rounding);
			drawList->PushClipRect(frame.Min, frame.Max, true);
			drawList->AddCircleFilled(frame.At(0.72f, 0.3f), frame.Height() * 0.12f, Shade(accent, 1.6f));
			drawList->AddTriangleFilled(frame.At(-0.05f, 1.0f), frame.At(0.34f, 0.38f), frame.At(0.78f, 1.0f), accent);
			drawList->AddTriangleFilled(frame.At(0.4f, 1.0f), frame.At(0.7f, 0.56f), frame.At(1.08f, 1.0f), Shade(accent, 0.7f));
			drawList->PopClipRect();
			drawList->AddRect(frame.Min, frame.Max, Shade(accent, 1.3f), rounding, GetStroke(box));
		}

		// Environments: a sun rising over the horizon of a sky.
		void DrawEnvironmentGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			Box const frame = GetPictureFrame(box);
			float const horizon = frame.Min.y + frame.Height() * 0.66f;
			ImU32 const high = Shade(accent, 0.3f);
			ImU32 const low = Shade(accent, 0.62f);
			drawList->AddRectFilledMultiColor(frame.Min, ImVec2(frame.Max.x, horizon), high, high, low, low);
			drawList->AddRectFilled(ImVec2(frame.Min.x, horizon), frame.Max, Shade(accent, 0.22f));
			ImVec2 const sun(frame.Center().x, horizon);
			float const radius = frame.Height() * 0.27f;
			drawList->PushClipRect(frame.Min, ImVec2(frame.Max.x, horizon), true);
			drawList->AddCircleFilled(sun, radius, Shade(accent, 1.5f));
			drawList->PopClipRect();
			float const stroke = GetStroke(box);
			for (int ray = 0; ray < 7; ray++)
			{
				float const angle = std::numbers::pi_v<float> * (1.0f + (static_cast<float>(ray) + 0.5f) / 7.0f);
				ImVec2 const direction(std::cos(angle), std::sin(angle));
				drawList->AddLine(ImVec2(sun.x + direction.x * radius * 1.3f, sun.y + direction.y * radius * 1.3f),
				                  ImVec2(sun.x + direction.x * radius * 1.65f, sun.y + direction.y * radius * 1.65f), Shade(accent, 1.35f),
				                  stroke);
			}
			drawList->AddLine(ImVec2(frame.Min.x, horizon), ImVec2(frame.Max.x, horizon), Shade(accent, 1.2f), stroke);
			drawList->AddRect(frame.Min, frame.Max, Shade(accent, 1.1f), 0.0f, stroke);
		}

		// Audio clips without their waveform yet: a speaker sending out waves.
		void DrawSpeakerGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			ImVec2 const center = box.Center();
			float const size = box.Width() * 0.5f;
			float const left = center.x - size * 0.7f;
			drawList->AddRectFilled(ImVec2(left, center.y - size * 0.2f), ImVec2(left + size * 0.26f, center.y + size * 0.2f), accent,
			                        2.0f);
			drawList->AddQuadFilled(
				ImVec2(left + size * 0.26f, center.y - size * 0.2f), ImVec2(left + size * 0.62f, center.y - size * 0.52f),
				ImVec2(left + size * 0.62f, center.y + size * 0.52f), ImVec2(left + size * 0.26f, center.y + size * 0.2f), accent);
			ImVec2 const origin(left + size * 0.62f, center.y);
			float const stroke = GetStroke(box) * 1.2f;
			for (int wave = 0; wave < 3; wave++)
			{
				drawList->PathArcTo(origin, size * (0.34f + 0.26f * static_cast<float>(wave)), -0.75f, 0.75f, 20);
				drawList->PathStroke(Shade(accent, 1.0f, 1.0f - 0.28f * static_cast<float>(wave)), stroke);
			}
		}

		// Audio clips: their peaks as bars mirrored around the middle, scaled to the loudest.
		void DrawWaveform(ImDrawList* drawList, Box const& area, AudioWaveform const& waveform, ImU32 accent)
		{
			if (waveform.Peaks.empty() || area.Width() < 2.0f)
			{
				return;
			}
			float const loudest = *std::max_element(waveform.Peaks.begin(), waveform.Peaks.end());
			// Silence stays flat instead of being scaled up.
			float const gain = 1.0f / std::max(loudest, 0.05f);
			size_t const bars = std::clamp<size_t>(static_cast<size_t>(area.Width() / 3.0f), 1, waveform.Peaks.size());
			float const barWidth = area.Width() / static_cast<float>(bars);
			float const middle = std::round(area.Center().y);
			float const halfHeight = area.Height() * 0.5f;
			drawList->AddLine(ImVec2(area.Min.x, middle), ImVec2(area.Max.x, middle), Shade(accent, 1.0f, 0.35f), 1.0f);
			for (size_t bar = 0; bar < bars; bar++)
			{
				size_t const begin = bar * waveform.Peaks.size() / bars;
				size_t const end = std::max(begin + 1, (bar + 1) * waveform.Peaks.size() / bars);
				float const peak = *std::max_element(waveform.Peaks.begin() + static_cast<std::ptrdiff_t>(begin),
				                                     waveform.Peaks.begin() + static_cast<std::ptrdiff_t>(end));
				float const height = std::max(1.0f, std::round(peak * gain * halfHeight));
				float const left = area.Min.x + static_cast<float>(bar) * barWidth + barWidth * 0.18f;
				float const right = std::max(left + 1.0f, area.Min.x + static_cast<float>(bar + 1) * barWidth - barWidth * 0.18f);
				drawList->AddRectFilled(ImVec2(left, middle - height), ImVec2(right, middle + height), accent,
				                        std::min(1.5f, (right - left) * 0.5f));
			}
		}

		// Fonts that cannot show their own glyphs: "Aa" in the editor's font.
		void DrawTypographyGlyph(ImDrawList* drawList, Box const& box, ImU32 color)
		{
			DrawCenteredText(drawList, box.Height() * 0.6f, box.Center(), color, "Aa");
		}

		// Scripts: a C# document.
		void DrawScriptGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			Box const page = DrawPage(drawList, box, 0.76f);
			float const textSize = page.Width() * 0.44f;
			ImVec2 const textPosition(std::round(page.Min.x + page.Width() * 0.13f), std::round(page.Min.y + page.Height() * 0.14f));
			ImFont* const font = ImGui::GetFont();
			// Drawn twice, a pixel apart: bolder.
			drawList->AddText(font, textSize, textPosition, accent, "C#");
			drawList->AddText(font, textSize, ImVec2(textPosition.x + 1.0f, textPosition.y), accent, "C#");
			float const lineHeight = std::max(1.5f, std::round(page.Height() * 0.05f));
			std::array<float, 3> const lineWidths = {0.62f, 0.44f, 0.54f};
			for (size_t line = 0; line < lineWidths.size(); line++)
			{
				float const y = page.Min.y + page.Height() * (0.62f + 0.12f * static_cast<float>(line));
				float const left = page.Min.x + page.Width() * 0.13f;
				drawList->AddRectFilled(ImVec2(left, y), ImVec2(left + page.Width() * lineWidths[line], y + lineHeight),
				                        line == 0 ? Shade(accent, 1.0f, 0.85f) : MutedInkColor, lineHeight * 0.5f);
			}
		}

		// Visual scripts: three nodes, the one on the left linked to the two on the right.
		void DrawVisualScriptGlyph(ImDrawList* drawList, Box const& box, ImU32 accent)
		{
			float const stroke = std::max(1.0f, GetStroke(box) * 0.8f);
			float const rounding = std::max(1.5f, box.Width() * 0.05f);
			ImU32 const link = Shade(accent, 1.3f);
			// The left node's output and the inputs of the right nodes, at the middle of their facing edges.
			ImVec2 const output = box.At(0.38f, 0.5f);
			std::array<ImVec2, 2> const inputs = {box.At(0.62f, 0.2f), box.At(0.62f, 0.8f)};
			for (ImVec2 const& input : inputs)
			{
				float const bend = (input.x - output.x) * 0.5f;
				drawList->AddBezierCubic(output, ImVec2(output.x + bend, output.y), ImVec2(input.x - bend, input.y), input, link, stroke);
			}
			// Each node: a body, a header band of the accent color and an outline.
			for (Box const& node : {Box{box.At(0.0f, 0.36f), box.At(0.38f, 0.64f)}, Box{box.At(0.62f, 0.06f), box.At(1.0f, 0.34f)},
			                        Box{box.At(0.62f, 0.66f), box.At(1.0f, 0.94f)}})
			{
				drawList->AddRectFilled(node.Min, node.Max, Shade(accent, 0.38f), rounding);
				drawList->AddRectFilled(node.Min, node.At(1.0f, 0.36f), accent, rounding, ImDrawFlags_RoundCornersTop);
				drawList->AddRect(node.Min, node.Max, Shade(accent, 1.25f), rounding, std::max(1.0f, stroke * 0.6f));
			}
			// The pins the links join.
			float const pin = stroke * 1.4f;
			drawList->AddCircleFilled(output, pin, Shade(accent, 1.6f));
			for (ImVec2 const& input : inputs)
			{
				drawList->AddCircleFilled(input, pin, Shade(accent, 1.6f));
			}
		}

		// Files of no known type: a blank page with a question mark.
		void DrawUnknownGlyph(ImDrawList* drawList, Box const& box)
		{
			Box const page = DrawPage(drawList, box, 0.72f);
			DrawCenteredText(drawList, page.Height() * 0.5f, ImVec2(page.Center().x, page.Center().y + page.Height() * 0.06f),
			                 MutedInkColor, "?");
		}

		void DrawFolderIcon(ImDrawList* drawList, Box const& box)
		{
			float const rounding = std::max(2.0f, box.Width() * 0.05f);
			float const top = box.Min.y + box.Height() * 0.1f;
			float const bottom = box.Max.y - box.Height() * 0.08f;
			float const tab = box.Height() * 0.12f;
			drawList->AddRectFilled(ImVec2(box.Min.x, top), ImVec2(box.Min.x + box.Width() * 0.44f, top + tab * 2.0f), FolderBackColor,
			                        rounding);
			drawList->AddRectFilled(ImVec2(box.Min.x, top + tab), ImVec2(box.Max.x, bottom), FolderBackColor, rounding);
			ImVec2 const frontMin(box.Min.x, top + tab * 2.0f);
			drawList->AddRectFilled(frontMin, ImVec2(box.Max.x, bottom), FolderFrontColor, rounding);
			drawList->AddLine(ImVec2(frontMin.x + rounding, frontMin.y + 1.0f), ImVec2(box.Max.x - rounding, frontMin.y + 1.0f),
			                  FolderEdgeColor, std::max(1.0f, box.Height() * 0.02f));
		}

		// A round badge with an exclamation mark in the card's top-right corner.
		void DrawCornerBadge(ImDrawList* drawList, Box const& card, float tileSize, ImU32 color)
		{
			float const radius = std::max(5.0f, tileSize * 0.075f);
			ImVec2 const center(card.Max.x - radius * 0.9f, card.Min.y + radius * 0.9f);
			drawList->AddCircleFilled(center, radius, color);
			DrawCenteredText(drawList, radius * 1.5f, center, IM_COL32_WHITE, "!");
		}

		std::string DescribeChannels(uint32_t channels)
		{
			switch (channels)
			{
				case 1:
					return "mono";
				case 2:
					return "stereo";
				default:
					return fmt::format("{} channels", channels);
			}
		}

		// A file or folder name typed by the user, trimmed. Empty names cancel the rename and are not errors.
		Result<std::string> CheckNewName(std::string_view text)
		{
			size_t const first = text.find_first_not_of(" \t");
			if (first == std::string_view::npos)
			{
				return std::string();
			}
			std::string_view const name = text.substr(first, text.find_last_not_of(" \t") - first + 1);
			if (name == "." || name == ".." || name.find_first_of("/\\") != std::string_view::npos)
			{
				return MakeError("'{}' is not a valid file or folder name", name);
			}
			return std::string(name);
		}

		// What the import dialog offers: every asset type and C# sources.
		std::string GetImportFilterExtensions()
		{
			std::string extensions;
			for (std::string_view const extension : GetSupportedAssetExtensions())
			{
				extensions += extensions.empty() ? "" : ",";
				extensions += extension.substr(1);
			}
			return extensions + "," + std::string(ScriptBrowsing::Extension.substr(1));
		}

		// Moves a file or folder that is not an asset (scripts, other files), refusing to replace anything.
		Result<void> MovePath(std::filesystem::path const& from, std::filesystem::path const& to)
		{
			if (FileSystem::Exists(to))
			{
				return MakeError("'{}' already exists", FileSystem::PathToUtf8(to.filename()));
			}
			return FileSystem::Move(from, to);
		}

		std::string GetExtension(std::string_view name)
		{
			size_t const dot = name.rfind('.');
			return dot == std::string_view::npos || dot == 0 ? std::string() : std::string(name.substr(dot));
		}

		// Folders (relative to their root) move unless dropped onto themselves, into their own subfolders or onto the folder
		// they are in.
		bool CanMoveFolder(std::string const& source, std::string const& target)
		{
			return !source.empty() && target != source && !target.starts_with(source + "/") &&
			       AssetBrowsing::JoinPath(target, AssetBrowsing::GetFileName(source)) != source;
		}
	}

	ContentBrowserPanel::ContentBrowserPanel()
		: m_OpenFile(
			  [](std::filesystem::path const& path)
			  {
				  return Desktop::Open(path);
			  })
	{
	}

	ContentBrowserPanel::Item ContentBrowserPanel::MakeFolderItem(ContentRoot root, std::string const& path)
	{
		Item item;
		item.Kind = ItemKind::Folder;
		item.Root = root;
		item.Path = path;
		item.Name = AssetBrowsing::GetFileName(path);
		return item;
	}

	ContentBrowserPanel::Item ContentBrowserPanel::MakeAssetItem(AssetMetadata const& asset)
	{
		Item item;
		item.Kind = ItemKind::Asset;
		item.Root = ContentRoot::Assets;
		item.Path = asset.Path;
		item.Name = AssetBrowsing::GetFileName(asset.Path);
		item.Asset = asset.Handle;
		item.Type = asset.Type;
		item.Missing = AssetManager::IsMissing(asset.Handle);
		return item;
	}

	ContentBrowserPanel::Item ContentBrowserPanel::MakeScriptItem(std::string const& path)
	{
		Item item;
		item.Kind = ItemKind::Script;
		item.Root = ContentRoot::Scripts;
		item.Path = path;
		item.Name = AssetBrowsing::GetFileName(path);
		return item;
	}

	ContentBrowserPanel::Item ContentBrowserPanel::MakeFileItem(std::string const& path)
	{
		Item item;
		item.Kind = ItemKind::File;
		item.Root = ContentRoot::Assets;
		item.Path = path;
		item.Name = AssetBrowsing::GetFileName(path);
		return item;
	}

	void ContentBrowserPanel::SetFolder(std::string folder)
	{
		Navigate(ContentRoot::Assets, std::move(folder));
	}

	void ContentBrowserPanel::SetScriptFolder(std::string folder)
	{
		Navigate(ContentRoot::Scripts, std::move(folder));
	}

	void ContentBrowserPanel::DropFiles(std::vector<std::filesystem::path> paths)
	{
		m_DroppedFiles.insert(m_DroppedFiles.end(), std::make_move_iterator(paths.begin()), std::make_move_iterator(paths.end()));
	}

	void ContentBrowserPanel::RevealScript(EditorContext& context, std::filesystem::path const& file)
	{
		Project const* const project = context.GetProject();
		if (project == nullptr)
		{
			return;
		}
		std::filesystem::path const directory = ScriptProject::GetSourceDirectory(*project);
		if (!FileSystem::IsInside(file, directory))
		{
			return;
		}
		std::string const path = FileSystem::PathToUtf8(FileSystem::GetRelativePath(file, directory));
		Navigate(ContentRoot::Scripts, AssetBrowsing::GetParentFolder(path));
		context.ClearSelection();
		m_Selected = Location{ContentRoot::Scripts, path};
		m_ScrollToSelection = true;
		InvalidateListings();
		RequestFocus();
	}

	void ContentBrowserPanel::Navigate(ContentRoot root, std::string folder)
	{
		m_Root = root;
		(root == ContentRoot::Assets ? m_Folder : m_ScriptFolder) = std::move(folder);
		SetFilter({});
	}

	void ContentBrowserPanel::SetFilter(std::string text)
	{
		if (text != m_Filter)
		{
			m_Filter = std::move(text);
			m_SearchFieldGeneration++;
		}
	}

	std::filesystem::path ContentBrowserPanel::GetRootDirectory(ContentRoot root) const
	{
		if (root == ContentRoot::Scripts)
		{
			return m_ScriptsDirectory;
		}
		return AssetManager::HasAssetDirectory() ? AssetManager::GetAssetDirectory() : std::filesystem::path();
	}

	std::filesystem::path ContentBrowserPanel::GetAbsolutePath(Item const& item) const
	{
		return GetRootDirectory(item.Root) / FileSystem::PathFromUtf8(item.Path);
	}

	void ContentBrowserPanel::OnImGuiRender(EditorOperations& operations, bool& open)
	{
		// Files dropped from the OS bring the browser to the front.
		if (std::exchange(m_FocusRequested, false) || !m_DroppedFiles.empty())
		{
			ImGui::SetNextWindowFocus();
		}
		bool const visible = ImGui::Begin(WindowName, &open);

		EditorContext& context = operations.GetContext();
		Project const* project = context.GetProject();
		std::filesystem::path const projectFile = project != nullptr ? project->GetFilePath() : std::filesystem::path();
		if (projectFile != m_ProjectFile)
		{
			// Leaving a project starts the next one at its asset directory; a folder set before any project was shown stays.
			if (!m_ProjectFile.empty())
			{
				m_Root = ContentRoot::Assets;
				m_Folder.clear();
				m_ScriptFolder.clear();
			}
			m_ProjectFile = projectFile;
			m_ScriptsDirectory = project != nullptr ? ScriptProject::GetSourceDirectory(*project) : std::filesystem::path();
			SetFilter({});
			m_Selected.reset();
			m_ScrollToSelection = false;
			m_Renaming.reset();
			m_PendingDelete.reset();
			m_Notice.reset();
			m_Listing.Valid = false;
			m_Subfolders.clear();
		}
		if (!visible)
		{
			// Behind another tab: dropped files go to the displayed folders.
			ImportDroppedFiles(operations);
			ImGui::End();
			return;
		}
		if (project == nullptr || !AssetManager::IsInitialized() || !AssetManager::HasAssetDirectory())
		{
			ImGui::TextDisabled("No project is open.");
			ImGui::TextWrapped("Create or open a project from the File menu to manage its assets.");
			ImportDroppedFiles(operations);
			ImGui::End();
			return;
		}
		// Entities selected elsewhere replace a selected folder or script, as they replace a selected asset.
		if (!context.GetSelection().IsEmpty())
		{
			m_Selected.reset();
		}
		m_FileDropTarget.reset();

		// The folder tree on the left, the toolbar and the tiles on the right.
		if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12.0f);
			ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			DrawFolderTree(operations);
			ImGui::TableNextColumn();
			DrawToolbar(operations);
			ImGui::Separator();
			DrawNotice();
			// After the toolbar, which may navigate, search or create.
			UpdateListing();
			DrawItems(operations);
			ImGui::EndTable();
		}
		HandleShortcuts(context);
		DrawDeletePopup(operations);

		std::vector<std::function<void()>> deferred;
		deferred.swap(m_Deferred);
		for (std::function<void()> const& action : deferred)
		{
			action();
		}
		ImportDroppedFiles(operations);
		ImGui::End();
	}

	void ContentBrowserPanel::UpdateListing()
	{
		double const now = ImGui::GetTime();
		// A folder is listed again every RescanInterval for files changed outside the editor; a search, which walks every
		// folder of the root, only when it or the registered assets change (and after Refresh).
		bool const rescanDue = m_Filter.empty() && now - m_Listing.ScanTime >= RescanInterval;
		if (m_Listing.Valid && m_Listing.Root == m_Root && m_Listing.Folder == GetDisplayedFolder() && m_Listing.Filter == m_Filter &&
		    m_Listing.AssetVersion == AssetManager::GetVersion() && !rescanDue)
		{
			return;
		}
		// The folder may have been moved or deleted (by automation, or outside the editor).
		std::filesystem::path const directory = GetRootDirectory(m_Root);
		std::string& displayed = m_Root == ContentRoot::Assets ? m_Folder : m_ScriptFolder;
		if (!displayed.empty() && !FileSystem::IsDirectory(directory / FileSystem::PathFromUtf8(displayed)))
		{
			displayed.clear();
		}
		if (m_Listing.Root != m_Root || m_Listing.Folder != displayed || m_Listing.Filter != m_Filter)
		{
			m_ScrollToTop = true;
		}

		std::vector<Item> items;
		if (m_Root == ContentRoot::Assets && !m_Filter.empty())
		{
			for (AssetMetadata const& asset : AssetBrowsing::SearchAssets(m_Filter))
			{
				items.push_back(MakeAssetItem(asset));
			}
			for (std::string const& file : AssetBrowsing::SearchOtherFiles(m_Filter))
			{
				items.push_back(MakeFileItem(file));
			}
		}
		else if (m_Root == ContentRoot::Assets)
		{
			for (std::string const& folder : AssetBrowsing::ListFolders(displayed))
			{
				items.push_back(MakeFolderItem(ContentRoot::Assets, folder));
			}
			for (AssetMetadata const& asset : AssetBrowsing::ListAssets(displayed))
			{
				items.push_back(MakeAssetItem(asset));
			}
			for (std::string const& file : AssetBrowsing::ListOtherFiles(displayed))
			{
				items.push_back(MakeFileItem(file));
			}
		}
		else if (!m_Filter.empty())
		{
			for (std::string const& script : ScriptBrowsing::SearchScripts(directory, m_Filter))
			{
				items.push_back(MakeScriptItem(script));
			}
		}
		else
		{
			for (std::string const& folder : ScriptBrowsing::ListFolders(directory, displayed))
			{
				items.push_back(MakeFolderItem(ContentRoot::Scripts, folder));
			}
			for (std::string const& script : ScriptBrowsing::ListScripts(directory, displayed))
			{
				items.push_back(MakeScriptItem(script));
			}
		}
		m_Listing.Valid = true;
		m_Listing.Root = m_Root;
		m_Listing.Folder = displayed;
		m_Listing.Filter = m_Filter;
		m_Listing.AssetVersion = AssetManager::GetVersion();
		m_Listing.ScanTime = now;
		m_Listing.Items = std::move(items);

		// A rename or deletion whose item went away (moved or deleted elsewhere) ends.
		auto const isListed = [this](Item const& item)
		{
			return std::any_of(m_Listing.Items.begin(), m_Listing.Items.end(),
			                   [&item](Item const& listed)
			                   {
								   return listed.GetLocation() == item.GetLocation();
							   });
		};
		if (m_Renaming && !isListed(*m_Renaming))
		{
			m_Renaming.reset();
		}
		if (m_PendingDelete && !isListed(*m_PendingDelete))
		{
			m_PendingDelete.reset();
		}
	}

	void ContentBrowserPanel::InvalidateListings()
	{
		m_Listing.Valid = false;
		m_Subfolders.clear();
	}

	void ContentBrowserPanel::DrawToolbar(EditorOperations& operations)
	{
		float const rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		// Copied: the buttons below may navigate.
		std::string const folder = GetDisplayedFolder();
		ContentRoot const root = m_Root;
		ImGui::BeginDisabled(folder.empty());
		if (ImGui::ArrowButton("##up", ImGuiDir_Up))
		{
			Navigate(root, AssetBrowsing::GetParentFolder(folder));
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Parent folder");

		// Breadcrumbs; each one also takes dropped assets, folders and scripts.
		ImGui::SameLine();
		if (ImGui::Button(root == ContentRoot::Assets ? "Assets" : "Scripts"))
		{
			Navigate(root, "");
		}
		AcceptDrop(operations, Location{root, ""});
		size_t start = 0;
		while (start < folder.size())
		{
			size_t const end = std::min(folder.find('/', start), folder.size());
			std::string const partial = folder.substr(0, end);
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::TextDisabled("/");
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::PushID(partial.c_str());
			bool const clicked = ImGui::Button(folder.substr(start, end - start).c_str());
			AcceptDrop(operations, Location{root, partial});
			ImGui::PopID();
			if (clicked)
			{
				Navigate(root, partial);
			}
			start = end + 1;
		}

		// The search field and the buttons sit at the right edge unless the breadcrumbs reach it.
		float const searchWidth = ImGui::GetFontSize() * 12.0f;
		float const sliderWidth = ImGui::GetFontSize() * 6.0f;
		float const buttonsWidth = ImGui::CalcTextSize("+").x + ImGui::CalcTextSize("Import").x + ImGui::CalcTextSize("Refresh").x +
		                           ImGui::GetStyle().FramePadding.x * 6.0f + ImGui::GetStyle().ItemSpacing.x * 4.0f;
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdge - sliderWidth - searchWidth - buttonsWidth));
		ImGui::SetNextItemWidth(sliderWidth);
		if (ImGui::SliderFloat("##size", &m_TileScale, static_cast<float>(MinTileScale), static_cast<float>(MaxTileScale), "Size",
		                       ImGuiSliderFlags_AlwaysClamp))
		{
			SetTileScale(m_TileScale);
		}
		ImGui::SetItemTooltip("Tile size");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(searchWidth);
		ImGui::PushID(static_cast<int>(m_SearchFieldGeneration));
		ImGui::InputTextWithHint("##search", root == ContentRoot::Assets ? "Search assets" : "Search scripts", &m_Filter);
		ImGui::PopID();
		ImGui::SameLine();
		if (ImGui::Button("+"))
		{
			ImGui::OpenPopup("##create");
		}
		ImGui::SetItemTooltip("Create");
		if (ImGui::BeginPopup("##create"))
		{
			DrawCreateMenuItems(operations, false);
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Import"))
		{
			ImportFiles(operations);
		}
		ImGui::SetItemTooltip("Copy files into this folder (C# scripts go to the scripts)");
		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
		{
			UI::ReportFailure(operations.RefreshAssets(), "Refreshing the assets");
			InvalidateListings();
		}
		ImGui::SetItemTooltip("Find new, missing and modified files");
	}

	void ContentBrowserPanel::DrawCreateMenuItems(EditorOperations& operations, bool prefixNew)
	{
		if (ImGui::MenuItem(prefixNew ? "New Folder" : "Folder"))
		{
			CreateFolder(operations);
		}
		ImGui::Separator();
		if (ImGui::MenuItem(prefixNew ? "New Material" : "Material", nullptr, false, m_Root == ContentRoot::Assets))
		{
			CreateMaterial(operations);
		}
		if (ImGui::MenuItem(prefixNew ? "New Visual Script" : "Visual Script", nullptr, false, m_Root == ContentRoot::Assets))
		{
			CreateVisualScript(operations);
		}
		if (m_NewScript && ImGui::MenuItem(prefixNew ? "New C# Script..." : "C# Script..."))
		{
			m_NewScript(m_Root == ContentRoot::Scripts ? m_ScriptFolder : std::string());
		}
	}

	void ContentBrowserPanel::SetTileScale(float scale)
	{
		m_TileScale = std::clamp(scale, static_cast<float>(MinTileScale), static_cast<float>(MaxTileScale));
		// The labels are laid out again for the new width.
		for (Item& item : m_Listing.Items)
		{
			item.LinesWidth = 0.0f;
		}
	}

	std::vector<std::string> const& ContentBrowserPanel::GetSubfolders(ContentRoot root, std::string const& folder)
	{
		double const now = ImGui::GetTime();
		if (now - m_SubfoldersTime >= RescanInterval || m_SubfoldersAssetVersion != AssetManager::GetVersion())
		{
			m_Subfolders.clear();
			m_SubfoldersTime = now;
			m_SubfoldersAssetVersion = AssetManager::GetVersion();
		}
		std::string const key = (root == ContentRoot::Assets ? "A:" : "S:") + folder;
		auto found = m_Subfolders.find(key);
		if (found == m_Subfolders.end())
		{
			std::vector<std::string> folders =
				root == ContentRoot::Assets ? AssetBrowsing::ListFolders(folder) : ScriptBrowsing::ListFolders(m_ScriptsDirectory, folder);
			found = m_Subfolders.emplace(key, std::move(folders)).first;
		}
		return found->second;
	}

	void ContentBrowserPanel::DrawFolderTree(EditorOperations& operations)
	{
		if (!ImGui::BeginChild("##folders"))
		{
			ImGui::EndChild();
			return;
		}
		DrawFolderTreeNode(operations, ContentRoot::Assets, "");
		DrawFolderTreeNode(operations, ContentRoot::Scripts, "");
		m_TreeRevealedFolder = Location{m_Root, GetDisplayedFolder()};
		ImGui::EndChild();
	}

	void ContentBrowserPanel::DrawFolderTreeNode(EditorOperations& operations, ContentRoot root, std::string const& folder)
	{
		// Copied: drawing the children may list folders again and replace the cached vector.
		std::vector<std::string> const children = GetSubfolders(root, folder);
		ImGuiTreeNodeFlags flags =
			ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (children.empty())
		{
			flags |= ImGuiTreeNodeFlags_Leaf;
		}
		if (root == m_Root && folder == GetDisplayedFolder() && m_Filter.empty())
		{
			flags |= ImGuiTreeNodeFlags_Selected;
		}
		if (folder.empty() && root == ContentRoot::Assets)
		{
			flags |= ImGuiTreeNodeFlags_DefaultOpen;
		}
		// The displayed folder's ancestors open when it changes (navigating in the tiles).
		Location const displayed{m_Root, GetDisplayedFolder()};
		if (m_TreeRevealedFolder != displayed && root == m_Root && (folder.empty() || displayed.Path.starts_with(folder + "/")) &&
		    !displayed.Path.empty())
		{
			ImGui::SetNextItemOpen(true);
		}
		std::string const label =
			folder.empty() ? std::string(root == ContentRoot::Assets ? "Assets" : "Scripts") : AssetBrowsing::GetFileName(folder);
		ImGui::PushID(static_cast<int>(root));
		ImGui::PushID(folder.c_str());
		bool const open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
		// The displayed folder's node scrolls into view when it changes.
		if (m_TreeRevealedFolder != displayed && root == m_Root && folder == displayed.Path)
		{
			ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		{
			Navigate(root, folder);
		}
		if (folder.empty() && root == ContentRoot::Scripts)
		{
			ImGui::SetItemTooltip("The project's C# scripts (Scripts/Source)");
		}
		AcceptDrop(operations, Location{root, folder});
		if (open)
		{
			for (std::string const& child : children)
			{
				DrawFolderTreeNode(operations, root, child);
			}
			ImGui::TreePop();
		}
		// Popped once TreePop has removed the open node's own ID: the ID stack stays last in, first out, which ImGui checks
		// for tree lines and keyboard navigation.
		ImGui::PopID();
		ImGui::PopID();
	}

	void ContentBrowserPanel::DrawItems(EditorOperations& operations)
	{
		if (ImGui::BeginChild("##items"))
		{
			if (std::exchange(m_ScrollToTop, false))
			{
				ImGui::SetScrollY(0.0f);
			}
			std::vector<Item>& items = m_Listing.Items;
			ImGuiStyle const& style = ImGui::GetStyle();
			float const tileSize = std::round(ImGui::GetFontSize() * TileSizeInFontSizes * m_TileScale);
			size_t const columns = static_cast<size_t>(
				std::max(1.0f, std::floor((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tileSize + style.ItemSpacing.x))));
			size_t const rows = (items.size() + columns - 1) / columns;

			// Only visible rows are drawn. A row is a tile, two label lines and the spacing between them and to the next row.
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(rows), tileSize + ImGui::GetTextLineHeight() * 2.0f + style.ItemSpacing.y * 3.0f);
			if (m_Renaming)
			{
				// The renamed item is drawn even when scrolled away, so its field can take focus (which scrolls to it).
				auto const renamed = std::find_if(items.begin(), items.end(),
				                                  [this](Item const& item)
				                                  {
													  return item.GetLocation() == m_Renaming->GetLocation();
												  });
				if (renamed != items.end())
				{
					clipper.IncludeItemByIndex(static_cast<int>(static_cast<size_t>(renamed - items.begin()) / columns));
				}
			}
			// What the browser selected itself (what an import brought in, a revealed script) is drawn and scrolled into view.
			std::optional<size_t> reveal;
			if (std::exchange(m_ScrollToSelection, false))
			{
				EditorContext const& context = operations.GetContext();
				auto const selected = std::find_if(items.begin(), items.end(),
				                                   [this, &context](Item const& item)
				                                   {
													   return IsSelected(context, item);
												   });
				if (selected != items.end())
				{
					reveal = static_cast<size_t>(selected - items.begin());
					clipper.IncludeItemByIndex(static_cast<int>(*reveal / columns));
				}
			}
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				{
					for (size_t column = 0; column < columns; column++)
					{
						size_t const index = static_cast<size_t>(row) * columns + column;
						if (index >= items.size())
						{
							break;
						}
						if (column > 0)
						{
							ImGui::SameLine();
						}
						DrawItem(operations, items[index], tileSize);
						if (reveal == index)
						{
							ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
						}
					}
				}
			}
			clipper.End();

			if (items.empty())
			{
				if (!m_Filter.empty())
				{
					ImGui::TextDisabled(m_Root == ContentRoot::Assets ? "No assets match the search." : "No scripts match the search.");
				}
				else if (m_Root == ContentRoot::Scripts && GetDisplayedFolder().empty())
				{
					ImGui::TextDisabled("No C# scripts yet. Create one with + > C# Script, or drop .cs files here.");
				}
				else
				{
					ImGui::TextDisabled("This folder is empty.");
				}
			}
			DrawBackground(operations);
		}
		ImGui::EndChild();
	}

	void ContentBrowserPanel::DrawItem(EditorOperations& operations, Item& item, float tileSize)
	{
		EditorContext& context = operations.GetContext();
		ImGui::PushID(static_cast<int>(item.Root));
		ImGui::PushID(item.Path.c_str());
		ImGui::BeginGroup();

		ImVec2 const tileMin = ImGui::GetCursorScreenPos();
		ImVec2 const tileMax(tileMin.x + tileSize, tileMin.y + tileSize);
		// Left clicks select on release: a press that starts a drag (onto the inspector, an asset field) keeps the selection
		// the drop target belongs to.
		bool const clicked = ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileSize));
		bool const hovered = ImGui::IsItemHovered();
		bool const selected = IsSelected(context, item);

		if (clicked || ImGui::IsItemClicked(ImGuiMouseButton_Right))
		{
			Select(operations, item);
		}
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			Open(operations, item);
		}
		switch (item.Kind)
		{
			case ItemKind::Folder:
				if (ImGui::BeginDragDropSource())
				{
					ImGui::SetDragDropPayload(item.Root == ContentRoot::Assets ? DragDropPayload::AssetFolder
					                                                           : DragDropPayload::ScriptFolder,
					                          item.Path.data(), item.Path.size());
					ImGui::TextUnformatted(item.Name.c_str());
					ImGui::EndDragDropSource();
				}
				AcceptDrop(operations, item.GetLocation());
				break;
			case ItemKind::Asset:
				UI::AssetDragSource(item.Asset, item.Name);
				break;
			case ItemKind::Script:
				UI::ScriptDragSource(GetAbsolutePath(item), item.Name);
				break;
			case ItemKind::File:
				if (ImGui::BeginDragDropSource())
				{
					ImGui::SetDragDropPayload(DragDropPayload::AssetFile, item.Path.data(), item.Path.size());
					ImGui::TextUnformatted(item.Name.c_str());
					ImGui::EndDragDropSource();
				}
				break;
		}
		DrawItemTooltip(item);
		DrawItemContextMenu(operations, item);

		// Selection and hover frame the card; folders, which have none, get a plate behind their icon.
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		Box const card = Square(Box{tileMin, tileMax}, 0.06f);
		float const rounding = GetCardRounding(card);
		if (item.IsFolder() && (selected || hovered))
		{
			drawList->AddRectFilled(card.Min, card.Max, selected ? SelectedFolderColor : HoveredFolderColor, rounding);
		}
		DrawTile(item, tileMin, tileSize, hovered);
		if (selected)
		{
			// Around the card's edge, so that thumbnails keep every pixel.
			constexpr float Outset = 1.5f;
			drawList->AddRect(ImVec2(card.Min.x - Outset, card.Min.y - Outset), ImVec2(card.Max.x + Outset, card.Max.y + Outset),
			                  SelectionColor, rounding + Outset, 2.0f);
		}
		else if (hovered)
		{
			drawList->AddRect(card.Min, card.Max, HoverOutlineColor, rounding, 1.0f);
		}

		if (m_Renaming && m_Renaming->GetLocation() == item.GetLocation())
		{
			DrawRenameField(operations, item, tileSize);
		}
		else
		{
			DrawItemLabel(item, tileSize, selected);
		}
		ImGui::EndGroup();
		ImGui::PopID();
		ImGui::PopID();
	}

	void ContentBrowserPanel::DrawItemLabel(Item& item, float tileSize, bool selected)
	{
		if (item.LinesWidth != tileSize)
		{
			item.Lines = UI::SplitTileLabel(item.Name, tileSize, item.IsFolder() ? std::string() : GetExtension(item.Name),
			                                item.IsFolder() ? std::string_view() : UI::GetDistinctFileNamePart(item.Path));
			item.LinesWidth = tileSize;
		}
		ImGuiStyle const& style = ImGui::GetStyle();
		if (selected)
		{
			float width = 0.0f;
			float lines = 0.0f;
			for (std::string const& line : item.Lines)
			{
				if (!line.empty())
				{
					width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
					lines += 1.0f;
				}
			}
			if (lines > 0.0f)
			{
				float const padding = std::round(ImGui::GetFontSize() * 0.3f);
				float const plateWidth = std::min(width + padding * 2.0f, tileSize);
				ImVec2 const start = ImGui::GetCursorScreenPos();
				ImVec2 const plateMin(start.x + std::round((tileSize - plateWidth) * 0.5f), start.y - 1.0f);
				ImVec2 const plateMax(plateMin.x + plateWidth,
				                      start.y + ImGui::GetTextLineHeight() * lines + style.ItemSpacing.y * (lines - 1.0f) + 1.0f);
				ImGui::GetWindowDrawList()->AddRectFilled(plateMin, plateMax, SelectedLabelColor, std::round(ImGui::GetFontSize() * 0.25f));
			}
		}
		// Always two lines (the second may be empty) so the tiles of a row line up.
		ImVec4 const color = selected ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : (item.Missing ? MissingColor : style.Colors[ImGuiCol_Text]);
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		for (std::string const& line : item.Lines)
		{
			float const width = ImGui::CalcTextSize(line.c_str()).x;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((tileSize - width) * 0.5f, 0.0f));
			ImGui::TextUnformatted(line.c_str());
		}
		ImGui::PopStyleColor();
	}

	void ContentBrowserPanel::DrawTile(Item const& item, ImVec2 tileMin, float tileSize, bool hovered)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImU32 const cardColor = hovered ? HoveredCardColor : CardColor;
		Box const tile = {tileMin, ImVec2(tileMin.x + tileSize, tileMin.y + tileSize)};
		if (item.IsFolder())
		{
			DrawFolderIcon(drawList, Square(tile, 0.13f));
			return;
		}
		Box const card = Square(tile, 0.06f);
		if (item.Kind == ItemKind::Script)
		{
			Box const area = DrawCard(drawList, card, ScriptStyle, cardColor);
			DrawScriptGlyph(drawList, Square(area, 0.1f), ScriptStyle.Accent);
			return;
		}
		if (item.Kind == ItemKind::File)
		{
			// The band names the file's extension: "BIN", "TXT".
			std::string label = GetExtension(item.Name);
			label = label.size() > 1 ? label.substr(1) : std::string("FILE");
			std::transform(label.begin(), label.end(), label.begin(),
			               [](unsigned char c)
			               {
							   return static_cast<char>(std::toupper(c));
						   });
			TypeStyle const style = {label.c_str(), "FILE", GetTypeStyle(AssetType::None).Accent};
			Box const area = DrawCard(drawList, card, style, cardColor);
			DrawUnknownGlyph(drawList, Square(area, 0.14f));
			return;
		}

		TypeStyle const style = GetTypeStyle(item.Type);
		if (item.Type == AssetType::AudioClip)
		{
			Box const area = DrawCard(drawList, card, style, cardColor);
			if (AudioWaveform const* waveform = m_Thumbnails != nullptr ? m_Thumbnails->GetWaveform(item.Asset) : nullptr)
			{
				float const margin = area.Width() * 0.08f;
				float const labelSize = GetBandLabelSize();
				Box const wave = {ImVec2(area.Min.x + margin, area.Min.y + margin + labelSize),
				                  ImVec2(area.Max.x - margin, area.Max.y - margin)};
				DrawWaveform(drawList, wave, *waveform, style.Accent);
				drawList->AddText(ImGui::GetFont(), labelSize, ImVec2(area.Min.x + margin, area.Min.y + margin * 0.6f), MutedInkColor,
				                  UI::FormatDuration(waveform->Duration).c_str());
			}
			else
			{
				DrawSpeakerGlyph(drawList, Square(area, 0.16f), style.Accent);
			}
		}
		else if (item.Type == AssetType::Font)
		{
			Box const area = DrawCard(drawList, card, style, cardColor);
			Box const glyph = Square(area, 0.06f);
			if (nvrhi::ITexture* const sample = m_Thumbnails != nullptr ? m_Thumbnails->Get(item.Asset) : nullptr)
			{
				drawList->AddImage(ImGuiRenderer::GetTextureID(sample), glyph.Min, glyph.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
				                   InkColor);
			}
			else
			{
				DrawTypographyGlyph(drawList, glyph, MutedInkColor);
			}
		}
		else if (nvrhi::ITexture* const thumbnail = m_Thumbnails != nullptr ? m_Thumbnails->Get(item.Asset) : nullptr)
		{
			// Textures keep their aspect ratio; rendered thumbnails are square. A strip of the type's color underneath.
			float const rounding = GetCardRounding(card);
			nvrhi::TextureDesc const& desc = thumbnail->getDesc();
			float const aspect = static_cast<float>(desc.width) / static_cast<float>(std::max(desc.height, 1u));
			ImVec2 const size =
				aspect >= 1.0f ? ImVec2(card.Width(), card.Height() / aspect) : ImVec2(card.Width() * aspect, card.Height());
			ImVec2 const imageMin(card.Min.x + (card.Width() - size.x) * 0.5f, card.Min.y + (card.Height() - size.y) * 0.5f);
			ImVec2 const imageMax(imageMin.x + size.x, imageMin.y + size.y);
			// Textures may be transparent or not fill the card: the card stays visible behind them.
			drawList->AddRectFilled(card.Min, card.Max, cardColor, rounding);
			drawList->AddImageRounded(ImGuiRenderer::GetTextureID(thumbnail), imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
			                          IM_COL32_WHITE, rounding);
			drawList->AddRect(card.Min, card.Max, CardBorderColor, rounding);
			float const strip = std::max(2.0f, std::round(tileSize * 0.03f));
			drawList->AddRectFilled(ImVec2(card.Min.x, card.Max.y - strip), card.Max, style.Accent, rounding,
			                        ImDrawFlags_RoundCornersBottom);
		}
		else
		{
			Box const area = DrawCard(drawList, card, style, cardColor);
			Box const glyph = Square(area, 0.14f);
			switch (item.Type)
			{
				case AssetType::Scene:
					DrawSceneGlyph(drawList, glyph, style.Accent);
					break;
				case AssetType::Prefab:
					DrawPrefabGlyph(drawList, glyph, style.Accent);
					break;
				case AssetType::Mesh:
					DrawMeshGlyph(drawList, glyph, style.Accent);
					break;
				case AssetType::Material:
					DrawMaterialGlyph(drawList, glyph, style.Accent);
					break;
				case AssetType::Texture:
					DrawTextureGlyph(drawList, glyph, style.Accent);
					break;
				case AssetType::Environment:
					DrawEnvironmentGlyph(drawList, glyph, style.Accent);
					break;
				case AssetType::VisualScript:
					DrawVisualScriptGlyph(drawList, glyph, style.Accent);
					break;
				case AssetType::AudioClip:
				case AssetType::Font:
				case AssetType::None:
					DrawUnknownGlyph(drawList, glyph);
					break;
			}
		}
		if (item.Missing)
		{
			// Red: the file is gone.
			DrawCornerBadge(drawList, card, tileSize, MissingBadgeColor);
		}
		else if (m_Thumbnails != nullptr && m_Thumbnails->GetLoadError(item.Asset) != nullptr)
		{
			// Amber: the file is there but cannot be read.
			DrawCornerBadge(drawList, card, tileSize, BrokenBadgeColor);
		}
	}

	void ContentBrowserPanel::DrawItemTooltip(Item const& item)
	{
		if (!ImGui::BeginItemTooltip())
		{
			return;
		}
		ImGui::TextUnformatted((std::string(item.Root == ContentRoot::Assets ? "Assets/" : "Scripts/") + item.Path).c_str());
		switch (item.Kind)
		{
			case ItemKind::Folder:
				break;
			case ItemKind::Asset:
			{
				if (item.Type == AssetType::VisualScript)
				{
					ImGui::TextDisabled("Visual script: double-click to edit it, drag it onto an entity to run it there");
				}
				else
				{
					ImGui::TextDisabled("%s", UI::FormatDisplayName(AssetTypeToString(item.Type)).c_str());
				}
				if (AudioWaveform const* waveform =
				        item.Type == AssetType::AudioClip && m_Thumbnails != nullptr ? m_Thumbnails->GetWaveform(item.Asset) : nullptr)
				{
					ImGui::TextDisabled("%s, %u Hz, %s", UI::FormatDuration(waveform->Duration).c_str(), waveform->SampleRate,
					                    DescribeChannels(waveform->ChannelCount).c_str());
				}
				if (item.Missing)
				{
					ImGui::TextColored(MissingColor, "The file is missing.");
				}
				else if (std::string const* error = m_Thumbnails != nullptr ? m_Thumbnails->GetLoadError(item.Asset) : nullptr)
				{
					ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, BrokenColor);
					ImGui::TextWrapped("The file cannot be read: %s", error->c_str());
					ImGui::PopStyleColor();
					ImGui::PopTextWrapPos();
				}
				break;
			}
			case ItemKind::Script:
				ImGui::TextDisabled("C# script: double-click to edit it, drag it onto an entity to add it.");
				break;
			case ItemKind::File:
				ImGui::TextDisabled("Not an asset: kept beside the assets. Double-click opens it.");
				break;
		}
		ImGui::EndTooltip();
	}

	void ContentBrowserPanel::DrawItemContextMenu(EditorOperations& operations, Item const& item)
	{
		if (!ImGui::BeginPopupContextItem("##itemMenu"))
		{
			return;
		}
		if (item.Kind == ItemKind::Script && ImGui::MenuItem("Open in Editor"))
		{
			Open(operations, item);
		}
		if (item.Kind == ItemKind::File && ImGui::MenuItem("Open with Default App"))
		{
			Open(operations, item);
		}
		bool const opens = item.IsFolder() || item.Type == AssetType::Scene || (item.Type == AssetType::VisualScript && m_OpenVisualScript);
		if (opens && ImGui::MenuItem("Open"))
		{
			Open(operations, item);
		}
		if (ImGui::MenuItem("Rename", "F2"))
		{
			BeginRename(item);
		}
		if (ImGui::MenuItem("Delete", "Delete"))
		{
			m_PendingDelete = item;
			m_OpenDeletePopup = true;
		}
		ImGui::Separator();
		if (item.Kind == ItemKind::Asset && ImGui::MenuItem("Copy Reference"))
		{
			ImGui::SetClipboardText(AssetManager::GetReference(item.Asset).c_str());
		}
		std::filesystem::path const absolute = GetAbsolutePath(item);
		if (ImGui::MenuItem("Copy Path"))
		{
			ImGui::SetClipboardText(FileSystem::PathToUtf8(absolute).c_str());
		}
		if (m_OpenFile && ImGui::MenuItem("Show in File Manager"))
		{
			UI::ReportFailure(m_OpenFile(item.IsFolder() ? absolute : absolute.parent_path()), "Showing the folder");
		}
		ImGui::EndPopup();
	}

	void ContentBrowserPanel::DrawBackground(EditorOperations& operations)
	{
		// The space after the tiles: clicking clears the browser's selection, dropping moves into this folder.
		ImVec2 size = ImGui::GetContentRegionAvail();
		size.x = std::max(size.x, 1.0f);
		size.y = std::max(size.y, ImGui::GetFrameHeight());
		ImGui::InvisibleButton("##background", size);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			m_Selected.reset();
			operations.GetContext().SelectAsset(AssetHandle());
		}
		AcceptDrop(operations, Location{m_Root, GetDisplayedFolder()});
		if (ImGui::BeginPopupContextItem("##backgroundMenu"))
		{
			DrawCreateMenuItems(operations, true);
			ImGui::Separator();
			if (ImGui::MenuItem("Import..."))
			{
				ImportFiles(operations);
			}
			if (ImGui::MenuItem("Refresh"))
			{
				UI::ReportFailure(operations.RefreshAssets(), "Refreshing the assets");
				InvalidateListings();
			}
			if (m_OpenFile && ImGui::MenuItem("Show in File Manager"))
			{
				std::filesystem::path const directory = GetRootDirectory(m_Root) / FileSystem::PathFromUtf8(GetDisplayedFolder());
				UI::ReportFailure(m_OpenFile(directory), "Showing the folder");
			}
			ImGui::EndPopup();
		}
	}

	void ContentBrowserPanel::DrawRenameField(EditorOperations& operations, Item const& item, float width)
	{
		if (std::exchange(m_FocusRenameField, false))
		{
			ImGui::SetKeyboardFocusHere();
			m_RenameFieldFrame = ImGui::GetFrameCount();
		}
		ImGui::SetNextItemWidth(width);
		ImGui::InputText("##rename", &m_RenameBuffer, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
		bool const active = ImGui::IsItemActive();
		// As tall as the two label lines it replaces, so the row keeps its height.
		float const padding = ImGui::GetTextLineHeight() * 2.0f - ImGui::GetFrameHeight();
		if (padding > 0.0f)
		{
			ImGui::Dummy(ImVec2(width, padding));
		}
		if (active)
		{
			m_RenameFieldActivated = true;
			return;
		}
		if (!m_RenameFieldActivated)
		{
			// Focus requests take effect within a frame or two; a field that never got focus is dropped.
			if (m_RenameFieldFrame >= 0 && ImGui::GetFrameCount() - m_RenameFieldFrame >= RenameFocusFrames)
			{
				m_Renaming.reset();
			}
			return;
		}

		// The field lost focus: Enter or a click elsewhere commits; Escape restored the original text.
		m_Renaming.reset();
		Result<std::string> const name = CheckNewName(m_RenameBuffer);
		if (!name)
		{
			ST_ERROR("Renaming failed: {}", name.GetError());
			return;
		}
		std::string const newName = name.GetValue() + (item.IsFolder() ? std::string() : GetExtension(item.Name));
		if (name.GetValue().empty() || newName == item.Name)
		{
			return;
		}
		Rename(operations, item, newName);
	}

	void ContentBrowserPanel::Rename(EditorOperations& operations, Item const& item, std::string const& newName)
	{
		std::string const newPath = AssetBrowsing::JoinPath(AssetBrowsing::GetParentFolder(item.Path), newName);
		Defer(
			[this, &operations, item, newPath]
			{
				auto const rename = [&]() -> Result<void>
				{
					if (item.Kind == ItemKind::Asset)
					{
						return operations.MoveAsset(item.Asset, newPath);
					}
					if (item.Kind == ItemKind::Folder && item.Root == ContentRoot::Assets)
					{
						return operations.MoveAssetFolder(item.Path, newPath);
					}
					return MovePath(GetAbsolutePath(item), GetRootDirectory(item.Root) / FileSystem::PathFromUtf8(newPath));
				};
				Result<void> const renamed = rename();
				UI::ReportFailure(renamed, "Renaming");
				if (renamed && m_Selected == item.GetLocation())
				{
					m_Selected = Location{item.Root, newPath};
				}
				InvalidateListings();
			});
	}

	void ContentBrowserPanel::Delete(EditorOperations& operations, Item const& item)
	{
		Defer(
			[this, &operations, item]
			{
				auto const remove = [&]() -> Result<void>
				{
					switch (item.Kind)
					{
						case ItemKind::Asset:
							return operations.DeleteAsset(item.Asset);
						case ItemKind::Folder:
							return item.Root == ContentRoot::Assets ? operations.DeleteAssetFolder(item.Path)
						                                            : FileSystem::RemoveAll(GetAbsolutePath(item));
						case ItemKind::Script:
						case ItemKind::File:
							return FileSystem::Remove(GetAbsolutePath(item));
					}
					return {};
				};
				Result<void> const deleted = remove();
				UI::ReportFailure(deleted, "Deleting");
				if (deleted && m_Selected && m_Selected->Root == item.Root &&
			        (m_Selected->Path == item.Path || m_Selected->Path.starts_with(item.Path + "/")))
				{
					m_Selected.reset();
				}
				InvalidateListings();
			});
	}

	void ContentBrowserPanel::DrawDeletePopup(EditorOperations& operations)
	{
		if (std::exchange(m_OpenDeletePopup, false))
		{
			ImGui::OpenPopup(DeletePopup);
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal(DeletePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}
		if (!m_PendingDelete)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		Item const item = *m_PendingDelete;
		ImGui::Text("Delete '%s'?", item.Path.c_str());
		ImGui::TextDisabled(item.IsFolder() ? "The folder and every file inside are deleted from disk. This cannot be undone."
		                                    : "The file is deleted from disk. This cannot be undone.");
		ImGui::Spacing();
		if (ImGui::Button("Delete"))
		{
			ImGui::CloseCurrentPopup();
			m_PendingDelete.reset();
			Delete(operations, item);
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			m_PendingDelete.reset();
		}
		ImGui::EndPopup();
	}

	void ContentBrowserPanel::HandleShortcuts(EditorContext const& context)
	{
		// Registered on the panel (its item area is a child window, part of the same focus route) every frame, even with
		// nothing selected: while the browser is focused these keys never reach the editor's editing keys, which act on the
		// Edit menu's target. Every chord is registered, pressed or not: routes decide the next frame.
		std::array<bool, FocusedChords.size()> pressed{};
		for (size_t i = 0; i < FocusedChords.size(); i++)
		{
			pressed[i] = ImGui::Shortcut(FocusedChords[i], ImGuiInputFlags_RouteFocused);
		}
		static_assert(FocusedChords[0] == ImGuiKey_F2 && FocusedChords[1] == ImGuiKey_Delete, "the order HandleShortcuts reads");
		bool const renamePressed = pressed[0];
		bool const deletePressed = pressed[1] || pressed[2];
		Item const* selected = FindSelectedItem(context);
		if (selected == nullptr || m_Renaming)
		{
			return;
		}
		if (renamePressed)
		{
			BeginRename(*selected);
		}
		else if (deletePressed)
		{
			m_PendingDelete = *selected;
			m_OpenDeletePopup = true;
		}
	}

	void ContentBrowserPanel::AcceptDrop(EditorOperations& operations, Location const& folder)
	{
		// Files dropped from the OS go to the folder under the cursor.
		if (!m_DroppedFiles.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
		{
			m_FileDropTarget = folder;
		}
		if (!ImGui::BeginDragDropTarget())
		{
			return;
		}
		if (folder.Root == ContentRoot::Scripts)
		{
			// C# sources move into the folder unless they are there already.
			if (std::optional<std::filesystem::path> const script = UI::AcceptScriptDrop())
			{
				std::filesystem::path const target = m_ScriptsDirectory / FileSystem::PathFromUtf8(folder.Path) / script->filename();
				if (FileSystem::IsInside(*script, m_ScriptsDirectory) && script->lexically_normal() != target.lexically_normal())
				{
					Defer(
						[this, source = *script, target]
						{
							UI::ReportFailure(MovePath(source, target), "Moving the script");
							InvalidateListings();
						});
				}
			}
			if (ImGuiPayload const* payload =
			        ImGui::AcceptDragDropPayload(DragDropPayload::ScriptFolder, ImGuiDragDropFlags_AcceptPeekOnly))
			{
				std::string const source(static_cast<char const*>(payload->Data), static_cast<size_t>(payload->DataSize));
				if (CanMoveFolder(source, folder.Path) && ImGui::AcceptDragDropPayload(DragDropPayload::ScriptFolder) != nullptr)
				{
					std::string const newFolder = AssetBrowsing::JoinPath(folder.Path, AssetBrowsing::GetFileName(source));
					Defer(
						[this, source, newFolder]
						{
							Result<void> const moved = MovePath(m_ScriptsDirectory / FileSystem::PathFromUtf8(source),
						                                        m_ScriptsDirectory / FileSystem::PathFromUtf8(newFolder));
							UI::ReportFailure(moved, "Moving the folder");
							if (moved && m_Selected == Location{ContentRoot::Scripts, source})
							{
								m_Selected = Location{ContentRoot::Scripts, newFolder};
							}
							InvalidateListings();
						});
				}
			}
			ImGui::EndDragDropTarget();
			return;
		}

		// Assets move into the folder unless they are already there.
		AssetHandle const asset = UI::AcceptAssetDrop(
			[&folder](AssetMetadata const& metadata)
			{
				return metadata.IsFileAsset() && AssetBrowsing::GetParentFolder(metadata.Path) != folder.Path;
			});
		if (std::optional<AssetMetadata> const metadata = asset.IsValid() ? AssetManager::GetMetadata(asset) : std::nullopt)
		{
			std::string const newPath = AssetBrowsing::JoinPath(folder.Path, AssetBrowsing::GetFileName(metadata->Path));
			Defer(
				[&operations, asset, newPath]
				{
					UI::ReportFailure(operations.MoveAsset(asset, newPath), "Moving the asset");
				});
		}
		// Entities dragged from the hierarchy become prefabs in the folder, named after them.
		if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::Entities))
		{
			std::vector<UUID> const entities = UI::ReadEntityPayload(*payload);
			Defer(
				[&operations, entities, path = folder.Path]
				{
					for (UUID const entity : entities)
					{
						UI::ReportFailure(operations.CreatePrefabInFolder(entity, path), "Creating the prefab");
					}
				});
		}
		// Files that are not assets (a model's buffers) move like scripts do.
		if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::AssetFile, ImGuiDragDropFlags_AcceptPeekOnly))
		{
			std::string const source(static_cast<char const*>(payload->Data), static_cast<size_t>(payload->DataSize));
			if (AssetBrowsing::GetParentFolder(source) != folder.Path &&
			    ImGui::AcceptDragDropPayload(DragDropPayload::AssetFile) != nullptr)
			{
				std::string const newPath = AssetBrowsing::JoinPath(folder.Path, AssetBrowsing::GetFileName(source));
				Defer(
					[this, source, newPath]
					{
						std::filesystem::path const assets = GetRootDirectory(ContentRoot::Assets);
						Result<void> const moved =
							MovePath(assets / FileSystem::PathFromUtf8(source), assets / FileSystem::PathFromUtf8(newPath));
						UI::ReportFailure(moved, "Moving the file");
						if (moved && m_Selected == Location{ContentRoot::Assets, source})
						{
							m_Selected = Location{ContentRoot::Assets, newPath};
						}
						InvalidateListings();
					});
			}
		}
		if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::AssetFolder, ImGuiDragDropFlags_AcceptPeekOnly))
		{
			std::string const source(static_cast<char const*>(payload->Data), static_cast<size_t>(payload->DataSize));
			std::string const newFolder = AssetBrowsing::JoinPath(folder.Path, AssetBrowsing::GetFileName(source));
			if (CanMoveFolder(source, folder.Path) && ImGui::AcceptDragDropPayload(DragDropPayload::AssetFolder) != nullptr)
			{
				Defer(
					[this, &operations, source, newFolder]
					{
						Result<void> const moved = operations.MoveAssetFolder(source, newFolder);
						UI::ReportFailure(moved, "Moving the folder");
						Location const moving{ContentRoot::Assets, source};
						if (moved && m_Selected == moving)
						{
							m_Selected = Location{ContentRoot::Assets, newFolder};
						}
					});
			}
		}
		ImGui::EndDragDropTarget();
	}

	bool ContentBrowserPanel::IsSelected(EditorContext const& context, Item const& item) const
	{
		AssetHandle const asset = context.GetSelectedAsset();
		if (item.Kind == ItemKind::Asset)
		{
			return asset.IsValid() && item.Asset == asset;
		}
		return !asset.IsValid() && m_Selected == item.GetLocation();
	}

	ContentBrowserPanel::Item const* ContentBrowserPanel::FindSelectedItem(EditorContext const& context) const
	{
		auto const it = std::find_if(m_Listing.Items.begin(), m_Listing.Items.end(),
		                             [this, &context](Item const& item)
		                             {
										 return IsSelected(context, item);
									 });
		return it != m_Listing.Items.end() ? &*it : nullptr;
	}

	void ContentBrowserPanel::Select(EditorOperations& operations, Item const& item)
	{
		EditorContext& context = operations.GetContext();
		if (item.Kind == ItemKind::Asset)
		{
			m_Selected.reset();
			context.SelectAsset(item.Asset);
		}
		else
		{
			// What was selected last is shown: a folder or a script replaces selected entities and assets.
			context.ClearSelection();
			m_Selected = item.GetLocation();
		}
	}

	void ContentBrowserPanel::Open(EditorOperations& operations, Item const& item)
	{
		switch (item.Kind)
		{
			case ItemKind::Folder:
				Navigate(item.Root, item.Path);
				m_Selected.reset();
				return;
			case ItemKind::Asset:
				Select(operations, item);
				if (item.Type == AssetType::Scene && m_OpenScene)
				{
					m_OpenScene(AssetManager::GetAbsolutePath(item.Asset));
				}
				else if (item.Type == AssetType::VisualScript && m_OpenVisualScript)
				{
					m_OpenVisualScript(item.Asset);
				}
				return;
			case ItemKind::Script:
			case ItemKind::File:
				Select(operations, item);
				if (m_OpenFile)
				{
					UI::ReportFailure(m_OpenFile(GetAbsolutePath(item)), "Opening the file");
				}
				return;
		}
	}

	void ContentBrowserPanel::BeginRename(Item item)
	{
		m_RenameBuffer = item.IsFolder() ? item.Name : FileSystem::PathToUtf8(FileSystem::PathFromUtf8(item.Name).stem());
		m_Renaming = std::move(item);
		m_FocusRenameField = true;
		m_RenameFieldActivated = false;
		m_RenameFieldFrame = -1;
	}

	void ContentBrowserPanel::CreateFolder(EditorOperations& operations)
	{
		std::string path;
		if (m_Root == ContentRoot::Assets)
		{
			path = AssetBrowsing::JoinPath(m_Folder, AssetBrowsing::MakeUniqueName(m_Folder, "New Folder", ""));
			Result<void> const created = operations.CreateAssetFolder(path);
			UI::ReportFailure(created, "Creating the folder");
			if (!created)
			{
				return;
			}
		}
		else
		{
			std::filesystem::path const parent = m_ScriptsDirectory / FileSystem::PathFromUtf8(m_ScriptFolder);
			path = AssetBrowsing::JoinPath(m_ScriptFolder, AssetBrowsing::MakeUniqueFileName(parent, "New Folder", ""));
			Result<void> const created = FileSystem::CreateDirectories(m_ScriptsDirectory / FileSystem::PathFromUtf8(path));
			UI::ReportFailure(created, "Creating the folder");
			if (!created)
			{
				return;
			}
		}
		// Shown right away (outside any search) so it can be named.
		SetFilter({});
		InvalidateListings();
		Item const item = MakeFolderItem(m_Root, path);
		Select(operations, item);
		BeginRename(item);
	}

	void ContentBrowserPanel::CreateMaterial(EditorOperations& operations)
	{
		std::string const name = AssetBrowsing::MakeUniqueName(m_Folder, "New Material", ".smat");
		Result<AssetHandle> const created = operations.CreateMaterial(AssetBrowsing::JoinPath(m_Folder, name));
		UI::ReportFailure(created, "Creating the material");
		std::optional<AssetMetadata> const metadata = created ? AssetManager::GetMetadata(created.GetValue()) : std::nullopt;
		if (!metadata)
		{
			return;
		}
		SetFilter({});
		Item const item = MakeAssetItem(*metadata);
		Select(operations, item);
		BeginRename(item);
	}

	void ContentBrowserPanel::CreateVisualScript(EditorOperations& operations)
	{
		std::string const name = AssetBrowsing::MakeUniqueName(m_Folder, "New Visual Script", ".sgraph");
		Result<AssetHandle> const created = operations.CreateVisualScript(AssetBrowsing::JoinPath(m_Folder, name));
		UI::ReportFailure(created, "Creating the visual script");
		std::optional<AssetMetadata> const metadata = created ? AssetManager::GetMetadata(created.GetValue()) : std::nullopt;
		if (!metadata)
		{
			return;
		}
		SetFilter({});
		Item const item = MakeAssetItem(*metadata);
		Select(operations, item);
		BeginRename(item);
	}

	void ContentBrowserPanel::ImportFiles(EditorOperations& operations)
	{
		std::array<FileDialogFilter, 1> const filters = {{{"Assets and C# scripts", GetImportFilterExtensions()}}};
		Result<std::vector<std::filesystem::path>> chosen = FileDialogs::OpenFiles(filters);
		if (!chosen)
		{
			ST_ERROR("{}", chosen.GetError());
			return;
		}
		if (!chosen.GetValue().empty())
		{
			Import(operations, chosen.GetValue(), ProjectImport::Destination{m_Folder, m_ScriptFolder});
		}
	}

	void ContentBrowserPanel::ImportDroppedFiles(EditorOperations& operations)
	{
		if (m_DroppedFiles.empty())
		{
			return;
		}
		std::vector<std::filesystem::path> const files = std::exchange(m_DroppedFiles, {});
		std::optional<Location> const target = std::exchange(m_FileDropTarget, std::nullopt);
		Location const location = target ? *target : Location{m_Root, GetDisplayedFolder()};
		(void)ImportDropped(operations, files, location.Root, location.Path);
	}

	ProjectImport::Report ContentBrowserPanel::ImportDropped(EditorOperations& operations, std::span<std::filesystem::path const> paths,
	                                                         ContentRoot root, std::string const& folder)
	{
		ProjectImport::Destination destination{m_Folder, m_ScriptFolder};
		(root == ContentRoot::Assets ? destination.AssetFolder : destination.ScriptFolder) = folder;
		// The browser shows what came in, and what did not.
		RequestFocus();
		return Import(operations, paths, destination);
	}

	ProjectImport::Report ContentBrowserPanel::Import(EditorOperations& operations, std::span<std::filesystem::path const> paths,
	                                                  ProjectImport::Destination const& destination)
	{
		ProjectImport::Report report = ProjectImport::Import(operations, m_Scripts, paths, destination);
		ProjectImport::Log(report);
		InvalidateListings();

		// What did not come in is said where the user looks, not only in the console.
		m_Notice.reset();
		if (!report.Errors.empty() || !report.Skipped.empty())
		{
			Notice notice;
			notice.Error = !report.Errors.empty();
			if (report.Errors.size() == 1)
			{
				notice.Lines.push_back("Not imported: " + report.Errors.front());
			}
			else if (!report.Errors.empty())
			{
				notice.Lines.push_back(fmt::format("{} parts of the import failed:", report.Errors.size()));
				for (size_t index = 0; index < std::min(report.Errors.size(), NoticeErrorLines); index++)
				{
					notice.Lines.push_back(report.Errors[index]);
				}
				if (report.Errors.size() > NoticeErrorLines)
				{
					notice.Lines.push_back(fmt::format("...and {} more (see the console)", report.Errors.size() - NoticeErrorLines));
				}
			}
			if (!report.Skipped.empty())
			{
				std::string names;
				for (std::string const& name : report.Skipped)
				{
					names += names.empty() ? name : ", " + name;
				}
				notice.Lines.push_back("Not imported, neither assets nor C# scripts: " + names);
				notice.Detail = ProjectImport::DescribeImportableFiles();
			}
			m_Notice = std::move(notice);
		}

		bool const assets = !report.Assets.empty() || !report.Folders.empty() || !report.KeptFiles.empty();
		bool const scripts = !report.Scripts.empty() || !report.ScriptFolders.empty();
		// What came in is shown: in the other root when it all went there.
		if (assets && !scripts && m_Root != ContentRoot::Assets)
		{
			Navigate(ContentRoot::Assets, m_Folder);
		}
		else if (scripts && !assets && m_Root != ContentRoot::Scripts)
		{
			Navigate(ContentRoot::Scripts, m_ScriptFolder);
		}

		// The first thing that came into the folder it was dropped into is selected (the inspector shows an asset).
		EditorContext& context = operations.GetContext();
		if (m_Root == ContentRoot::Scripts && scripts)
		{
			std::optional<std::string> path;
			if (!report.ScriptFolders.empty())
			{
				path = report.ScriptFolders.front();
			}
			else if (FileSystem::IsInside(report.Scripts.front(), m_ScriptsDirectory))
			{
				path = FileSystem::PathToUtf8(FileSystem::GetRelativePath(report.Scripts.front(), m_ScriptsDirectory));
			}
			if (path)
			{
				context.ClearSelection();
				m_Selected = Location{ContentRoot::Scripts, *path};
				m_ScrollToSelection = true;
			}
			return report;
		}
		if (!assets)
		{
			return report;
		}
		m_ScrollToSelection = true;
		auto const inDestination = [&destination](AssetHandle asset)
		{
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
			return metadata && AssetBrowsing::GetParentFolder(metadata->Path) == destination.AssetFolder;
		};
		auto const shown = std::find_if(report.Assets.begin(), report.Assets.end(), inDestination);
		if (shown != report.Assets.end())
		{
			m_Selected.reset();
			context.SelectAsset(*shown);
		}
		else if (!report.Folders.empty())
		{
			context.ClearSelection();
			m_Selected = Location{ContentRoot::Assets, report.Folders.front()};
		}
		else if (!report.Assets.empty())
		{
			m_Selected.reset();
			context.SelectAsset(report.Assets.front());
		}
		return report;
	}

	std::string ContentBrowserPanel::GetNotice() const
	{
		if (!m_Notice)
		{
			return {};
		}
		std::string text;
		for (std::string const& line : m_Notice->Lines)
		{
			text += text.empty() ? line : "\n" + line;
		}
		if (!m_Notice->Detail.empty())
		{
			text += "\n" + m_Notice->Detail;
		}
		return text;
	}

	void ContentBrowserPanel::DrawNotice()
	{
		if (!m_Notice)
		{
			return;
		}
		double const now = ImGui::GetTime();
		if (!m_Notice->Time)
		{
			m_Notice->Time = now;
		}
		else if (now - *m_Notice->Time > NoticeSeconds)
		{
			m_Notice.reset();
			return;
		}
		ImU32 const accent = m_Notice->Error ? NoticeErrorColor : NoticeWarningColor;
		ImGuiStyle const& style = ImGui::GetStyle();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, Mix(CardColor, accent, 0.16f));
		ImGui::PushStyleColor(ImGuiCol_Border, Shade(accent, 0.85f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, std::round(ImGui::GetFontSize() * 0.3f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x * 1.5f, style.FramePadding.y * 1.5f));
		bool dismissed = false;
		if (ImGui::BeginChild("##notice", ImVec2(0.0f, 0.0f),
		                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding))
		{
			// The buttons sit at the top-right; the text wraps before them.
			char const* const consoleLabel = "Show Console";
			float const closeWidth = ImGui::GetFrameHeight();
			float const consoleWidth =
				m_ShowConsole ? ImGui::CalcTextSize(consoleLabel).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f;
			float const buttonsWidth = closeWidth + consoleWidth;
			float const top = ImGui::GetCursorPosY();
			float const right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

			// A round badge with an exclamation mark, like the ones of broken tiles.
			float const lineHeight = ImGui::GetTextLineHeight();
			ImVec2 const cursor = ImGui::GetCursorScreenPos();
			ImVec2 const badgeCenter(cursor.x + lineHeight * 0.5f, cursor.y + lineHeight * 0.5f);
			ImDrawList* const drawList = ImGui::GetWindowDrawList();
			drawList->AddCircleFilled(badgeCenter, lineHeight * 0.45f, accent);
			DrawCenteredText(drawList, lineHeight * 0.7f, badgeCenter, IM_COL32_WHITE, "!");
			ImGui::Dummy(ImVec2(lineHeight, lineHeight));
			ImGui::SameLine();
			float const textLeft = ImGui::GetCursorPosX();
			ImGui::PushTextWrapPos(right - buttonsWidth - style.ItemSpacing.x);
			for (std::string const& line : m_Notice->Lines)
			{
				ImGui::SetCursorPosX(textLeft);
				ImGui::TextWrapped("%s", line.c_str());
			}
			if (!m_Notice->Detail.empty())
			{
				ImGui::SetCursorPosX(textLeft);
				ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
				ImGui::TextWrapped("%s", m_Notice->Detail.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::PopTextWrapPos();

			// Centered on the first line of text. The notice is as tall as what is drawn in it: its text, or its buttons
			// when the text is shorter.
			ImGui::SetCursorPos(ImVec2(right - buttonsWidth, top - (ImGui::GetFrameHeight() - lineHeight) * 0.5f));
			if (m_ShowConsole)
			{
				if (ImGui::Button(consoleLabel))
				{
					m_ShowConsole();
				}
				ImGui::SameLine();
			}
			if (ImGui::Button("##dismiss", ImVec2(closeWidth, 0.0f)))
			{
				dismissed = true;
			}
			// A cross in the text's color.
			ImVec2 const crossCenter((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
			                         (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
			float const arm = std::round(ImGui::GetFontSize() * 0.22f);
			float const thickness = std::max(1.0f, ImGui::GetFontSize() * 0.08f);
			ImU32 const ink = ImGui::GetColorU32(ImGuiCol_Text);
			drawList->AddLine(ImVec2(crossCenter.x - arm, crossCenter.y - arm), ImVec2(crossCenter.x + arm, crossCenter.y + arm), ink,
			                  thickness);
			drawList->AddLine(ImVec2(crossCenter.x + arm, crossCenter.y - arm), ImVec2(crossCenter.x - arm, crossCenter.y + arm), ink,
			                  thickness);
			ImGui::SetItemTooltip("Dismiss");
			// It stays while the mouse is over it.
			if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			{
				m_Notice->Time = now;
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
		if (dismissed)
		{
			m_Notice.reset();
		}
	}
}
