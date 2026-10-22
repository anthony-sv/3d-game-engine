#include "Editor/UI/EditorUI.h"

#include "Editor/Commands/CommandHistory.h"
#include "Editor/EntityPresets.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Strada
{
	namespace UI
	{
		namespace
		{
			// What shortened tile labels leave text out with.
			constexpr std::string_view TileEllipsis = "...";

			float TextWidth(std::string_view text)
			{
				return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
			}

			// Whether an ASCII capital at index starts a word of a PascalCase name: after a lowercase letter ("Showcase|Tests")
			// or as the last capital of an acronym ("XML|Parser").
			bool StartsWord(std::string_view text, size_t index)
			{
				auto const isUpper = [&text](size_t at)
				{
					return at < text.size() && text[at] >= 'A' && text[at] <= 'Z';
				};
				auto const isLower = [&text](size_t at)
				{
					return at < text.size() && text[at] >= 'a' && text[at] <= 'z';
				};
				return index > 0 && isUpper(index) && (isLower(index - 1) || (isUpper(index - 1) && isLower(index + 1)));
			}

			// Bytes of the UTF-8 code point starting at index.
			size_t CodePointLength(std::string_view text, size_t index)
			{
				size_t length = 1;
				while (index + length < text.size() && (static_cast<unsigned char>(text[index + length]) & 0xC0) == 0x80)
				{
					length++;
				}
				return length;
			}

			// The longest start of the text, cut between code points, that is at most the width wide.
			size_t FitCodePoints(std::string_view text, float width)
			{
				size_t end = 0;
				while (end < text.size())
				{
					size_t const next = end + CodePointLength(text, end);
					if (TextWidth(text.substr(0, next)) > width)
					{
						break;
					}
					end = next;
				}
				return end;
			}

			// The text shortened with "..." to fit the width. A given end of the text (a file's extension) stays visible:
			// "LongMaterialName.smat" becomes "LongMat...smat".
			std::string FitTextKeepingEnd(std::string_view text, float width, std::string_view end)
			{
				if (TextWidth(text) <= width)
				{
					return std::string(text);
				}
				if (end.size() > 1 && end.size() < text.size())
				{
					// The ellipsis replaces the extension's dot.
					std::string_view const tail = end.starts_with('.') ? end.substr(1) : end;
					float const tailWidth = TextWidth(TileEllipsis) + TextWidth(tail);
					size_t const head = FitCodePoints(text.substr(0, text.size() - end.size()), width - tailWidth);
					if (head > 0)
					{
						return std::string(text.substr(0, head)) + std::string(TileEllipsis) + std::string(tail);
					}
				}
				return std::string(text.substr(0, FitCodePoints(text, width - TextWidth(TileEllipsis)))) + std::string(TileEllipsis);
			}

			bool IsWordSeparator(char c)
			{
				return c == '_' || c == '-' || c == ' ';
			}

			// Whether a file name (or the end of one) has a word in front of its extension that is not a resolution ("1k").
			bool SaysMoreThanResolution(std::string_view name)
			{
				std::string_view const stem = name.substr(0, std::min(name.find_last_of('.'), name.size()));
				for (size_t start = 0; start < stem.size();)
				{
					size_t end = start;
					while (end < stem.size() && !IsWordSeparator(stem[end]))
					{
						end++;
					}
					std::string_view const word = stem.substr(start, end - start);
					bool const resolution = word.size() >= 2 && (word.back() == 'k' || word.back() == 'K') &&
					                        std::all_of(word.begin(), word.end() - 1,
					                                    [](char c)
					                                    {
															return std::isdigit(static_cast<unsigned char>(c)) != 0;
														});
					if (!word.empty() && !resolution)
					{
						return true;
					}
					start = end + 1;
				}
				return false;
			}

			// What follows the first word separator ("arm_1k.jpg" of "brass_arm_1k.jpg"); empty without one.
			std::string_view DropFirstWord(std::string_view part)
			{
				size_t const separator = static_cast<size_t>(std::find_if(part.begin(), part.end(), IsWordSeparator) - part.begin());
				return separator < part.size() ? part.substr(separator + 1) : std::string_view();
			}

			// The UTF-8 encodings of U+2192 and U+2190 share their first two bytes.
			constexpr std::string_view ArrowLead = "\xE2\x86";
			constexpr char RightArrowLast = '\x92';
			constexpr char LeftArrowLast = '\x90';
			// Drawn arrows in font sizes: about as wide as an em of the editor's fonts, with side bearings like a glyph's.
			constexpr float ArrowAdvance = 0.85f;
			constexpr float ArrowSideBearing = 0.12f;
			constexpr float ArrowHeadLength = 0.3f;
			constexpr float ArrowHeadHalfHeight = 0.18f;
			constexpr float ArrowStemThickness = 0.075f;

			bool HasArrows(std::string_view text)
			{
				for (size_t position = text.find(ArrowLead); position != std::string_view::npos;
				     position = text.find(ArrowLead, position + 1))
				{
					if (position + 2 < text.size() && (text[position + 2] == RightArrowLast || text[position + 2] == LeftArrowLast))
					{
						return true;
					}
				}
				return false;
			}

			// Calls onText with each run of text between the arrows of a line (none of them empty) and onArrow with each arrow
			// (true for a right arrow), in order.
			template<typename TOnText, typename TOnArrow>
			void ForEachArrowTextPart(std::string_view line, TOnText const& onText, TOnArrow const& onArrow)
			{
				size_t start = 0;
				size_t position = line.find(ArrowLead);
				while (position != std::string_view::npos)
				{
					bool const isArrow =
						position + 2 < line.size() && (line[position + 2] == RightArrowLast || line[position + 2] == LeftArrowLast);
					if (!isArrow)
					{
						position = line.find(ArrowLead, position + 1);
						continue;
					}
					if (position > start)
					{
						onText(line.substr(start, position - start));
					}
					onArrow(line[position + 2] == RightArrowLast);
					start = position + 3;
					position = line.find(ArrowLead, start);
				}
				if (start < line.size())
				{
					onText(line.substr(start));
				}
			}

			// A null font is the current font, a size of 0 or less the current size, as ImDrawList::AddText takes them.
			void ResolveFont(ImFont*& font, float& fontSize)
			{
				if (font == nullptr)
				{
					font = ImGui::GetFont();
				}
				if (fontSize <= 0.0f)
				{
					fontSize = ImGui::GetFontSize();
				}
			}

			float MeasureText(ImFont* font, float fontSize, std::string_view text)
			{
				return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
			}

			// How far below the top of a line the font's math axis lies, in pixels: the middle of its '+', where arrows sit
			// (as minus signs do); the middle of the line for a font without one.
			float GetMathAxis(ImFont* font, float fontSize)
			{
				ImFontBaked* const baked = font->GetFontBaked(fontSize);
				ImFontGlyph const* const plus = baked != nullptr ? baked->FindGlyphNoFallback('+') : nullptr;
				if (plus == nullptr || !plus->Visible || baked->Size <= 0.0f)
				{
					return fontSize * 0.5f;
				}
				return (plus->Y0 + plus->Y1) * 0.5f * (fontSize / baked->Size);
			}

			// An arrow in the advance whose top-left corner is at, on the math axis that lies axis pixels below it.
			void DrawArrow(ImDrawList* drawList, ImVec2 at, float fontSize, float axis, ImU32 color, bool pointsRight)
			{
				// The stem covers whole rows of pixels, so it stays sharp at every size; the head is anti-aliased.
				float const thickness = std::max(1.0f, std::round(fontSize * ArrowStemThickness));
				float const stemTop = std::round(at.y + axis - thickness * 0.5f);
				float const middle = stemTop + thickness * 0.5f;
				float const halfHeight = std::max(thickness + 1.0f, fontSize * ArrowHeadHalfHeight);
				float const headLength = std::max(thickness + 2.0f, fontSize * ArrowHeadLength);
				float const left = at.x + fontSize * ArrowSideBearing;
				float const right = at.x + fontSize * (ArrowAdvance - ArrowSideBearing);
				// The stem reaches half a pixel into the head, so no seam shows between them. Filled shapes list their points
				// clockwise (screen Y points down), as ImGui's anti-aliasing expects.
				if (pointsRight)
				{
					float const base = right - headLength;
					drawList->AddRectFilled(ImVec2(left, stemTop), ImVec2(base + 0.5f, stemTop + thickness), color);
					drawList->AddTriangleFilled(ImVec2(base, middle - halfHeight), ImVec2(right, middle), ImVec2(base, middle + halfHeight),
					                            color);
				}
				else
				{
					float const base = left + headLength;
					drawList->AddRectFilled(ImVec2(base - 0.5f, stemTop), ImVec2(right, stemTop + thickness), color);
					drawList->AddTriangleFilled(ImVec2(left, middle), ImVec2(base, middle - halfHeight), ImVec2(base, middle + halfHeight),
					                            color);
				}
			}
		}

		ImVec2 GetToolbarButtonSize()
		{
			float const height = ImGui::GetFrameHeight();
			return ImVec2(std::floor(height * 1.35f), height);
		}

		bool ToolbarButton(char const* id, ToolbarIcon icon, bool active)
		{
			ImVec2 const size = GetToolbarButtonSize();
			bool const pressed = ImGui::InvisibleButton(id, size);
			bool const hovered = ImGui::IsItemHovered();
			bool const held = ImGui::IsItemActive();
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			ImGuiCol const background = active || held ? ImGuiCol_ButtonActive : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
			drawList->AddRectFilled(min, max, ImGui::GetColorU32(background), ImGui::GetStyle().FrameRounding);

			// The icon fills the middle of the button; Alpha follows BeginDisabled through GetColorU32.
			ImU32 const color = ImGui::GetColorU32(ImGuiCol_Text);
			float const extent = size.y * 0.42f;
			ImVec2 const center(std::floor((min.x + max.x) * 0.5f), std::floor((min.y + max.y) * 0.5f));
			float const half = extent * 0.5f;
			switch (icon)
			{
				case ToolbarIcon::Play:
					drawList->AddTriangleFilled(ImVec2(center.x - half * 0.8f, center.y - half),
					                            ImVec2(center.x - half * 0.8f, center.y + half), ImVec2(center.x + half, center.y), color);
					break;
				case ToolbarIcon::Stop:
					drawList->AddRectFilled(ImVec2(center.x - half * 0.85f, center.y - half * 0.85f),
					                        ImVec2(center.x + half * 0.85f, center.y + half * 0.85f), color, 1.5f);
					break;
				case ToolbarIcon::Pause:
				{
					float const bar = half * 0.6f;
					drawList->AddRectFilled(ImVec2(center.x - half * 0.85f, center.y - half),
					                        ImVec2(center.x - half * 0.85f + bar, center.y + half), color, 1.0f);
					drawList->AddRectFilled(ImVec2(center.x + half * 0.85f - bar, center.y - half),
					                        ImVec2(center.x + half * 0.85f, center.y + half), color, 1.0f);
					break;
				}
				case ToolbarIcon::Step:
				{
					float const bar = half * 0.45f;
					drawList->AddTriangleFilled(ImVec2(center.x - half, center.y - half), ImVec2(center.x - half, center.y + half),
					                            ImVec2(center.x + half * 0.55f, center.y), color);
					drawList->AddRectFilled(ImVec2(center.x + half * 0.55f, center.y - half),
					                        ImVec2(center.x + half * 0.55f + bar, center.y + half), color, 1.0f);
					break;
				}
			}
			return pressed;
		}

		ImVec2 CalcArrowTextSize(std::string_view text)
		{
			ImVec2 size = CalcArrowTextSize(nullptr, 0.0f, text);
			size.x = ImCeilFast(size.x);
			return size;
		}

		ImVec2 CalcArrowTextSize(ImFont* font, float fontSize, std::string_view text)
		{
			ResolveFont(font, fontSize);
			if (text.empty())
			{
				// What ImGui measures an empty text as; an empty view may have no data, which ImGui would read up to a zero.
				return ImVec2(0.0f, fontSize);
			}
			if (!HasArrows(text))
			{
				return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
			}
			float width = 0.0f;
			float height = 0.0f;
			size_t lineStart = 0;
			while (true)
			{
				size_t const lineEnd = text.find('\n', lineStart);
				std::string_view const line =
					text.substr(lineStart, lineEnd != std::string_view::npos ? lineEnd - lineStart : std::string_view::npos);
				float lineWidth = 0.0f;
				ForEachArrowTextPart(
					line,
					[&](std::string_view run)
					{
						lineWidth += MeasureText(font, fontSize, run);
					},
					[&](bool)
					{
						lineWidth += fontSize * ArrowAdvance;
					});
				width = std::max(width, lineWidth);
				if (lineEnd == std::string_view::npos)
				{
					// As ImGui measures text: each line break adds a line, and the last line counts unless it is empty.
					if (lineWidth > 0.0f || height == 0.0f)
					{
						height += fontSize;
					}
					return ImVec2(width, height);
				}
				height += fontSize;
				lineStart = lineEnd + 1;
			}
		}

		void DrawArrowText(ImDrawList* drawList, ImVec2 position, uint32_t color, std::string_view text)
		{
			DrawArrowText(drawList, nullptr, 0.0f, position, color, text);
		}

		void DrawArrowText(ImDrawList* drawList, ImFont* font, float fontSize, ImVec2 position, uint32_t color, std::string_view text)
		{
			ResolveFont(font, fontSize);
			if (!HasArrows(text))
			{
				drawList->AddText(font, fontSize, position, color, text.data(), text.data() + text.size());
				return;
			}
			float const axis = GetMathAxis(font, fontSize);
			float lineTop = position.y;
			size_t lineStart = 0;
			while (true)
			{
				size_t const lineEnd = text.find('\n', lineStart);
				std::string_view const line =
					text.substr(lineStart, lineEnd != std::string_view::npos ? lineEnd - lineStart : std::string_view::npos);
				float x = position.x;
				ForEachArrowTextPart(
					line,
					[&](std::string_view run)
					{
						drawList->AddText(font, fontSize, ImVec2(x, lineTop), color, run.data(), run.data() + run.size());
						x += MeasureText(font, fontSize, run);
					},
					[&](bool pointsRight)
					{
						// Snapped to whole pixels as ImGui snaps text, so the arrow sits on the rows of the glyphs beside it.
						ImVec2 const origin =
							(drawList->Flags & ImDrawListFlags_TextNoPixelSnap) != 0 ? ImVec2(x, lineTop) : ImTrunc(ImVec2(x, lineTop));
						DrawArrow(drawList, origin, fontSize, axis, color, pointsRight);
						x += fontSize * ArrowAdvance;
					});
				if (lineEnd == std::string_view::npos)
				{
					return;
				}
				lineTop += fontSize;
				lineStart = lineEnd + 1;
			}
		}

		std::string FormatDisplayName(std::string_view name)
		{
			auto const isUpper = [](char c)
			{
				return std::isupper(static_cast<unsigned char>(c)) != 0;
			};
			auto const isLower = [](char c)
			{
				return std::islower(static_cast<unsigned char>(c)) != 0;
			};
			auto const isDigit = [](char c)
			{
				return std::isdigit(static_cast<unsigned char>(c)) != 0;
			};

			// Private C# fields of scripts follow the m_/s_ prefix convention; the prefix is not part of the name shown.
			if (name.size() > 2 && (name.starts_with("m_") || name.starts_with("s_")) && isUpper(name[2]))
			{
				name.remove_prefix(2);
			}

			std::string result;
			result.reserve(name.size() + 4);
			for (size_t i = 0; i < name.size(); i++)
			{
				char const c = name[i];
				if (i > 0 && isUpper(c))
				{
					char const previous = name[i - 1];
					bool const startsWord = isLower(previous) || isDigit(previous);
					// The last capital of an acronym of two or more letters starts the next word: "UVTiling" -> "UV Tiling",
					// but "VSync" stays one word.
					bool const endsAcronym =
						isUpper(previous) && i >= 2 && isUpper(name[i - 2]) && i + 1 < name.size() && isLower(name[i + 1]);
					if (startsWord || endsAcronym)
					{
						result += ' ';
					}
				}
				result += c;
			}
			return result;
		}

		std::string DescribeAssetReference(AssetHandle asset)
		{
			if (!asset.IsValid())
			{
				return "None";
			}
			if (!AssetManager::IsInitialized())
			{
				return asset.ToString();
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
			if (!metadata)
			{
				return fmt::format("Unknown asset {}", asset);
			}
			// File and built-in assets are found by their paths; the others are named after what holds them.
			std::string text = metadata->IsFileAsset() || metadata->IsBuiltIn() ? metadata->Path : metadata->GetDisplayName();
			return AssetManager::IsMissing(asset) ? text + " (missing)" : text;
		}

		std::string GetEllipsis()
		{
			// ImGui shortens labels with the font's ellipsis glyph: U+2026, or a "..." it makes from dots.
			std::array<char, 5> encoded{};
			ImTextCharToUtf8(encoded.data(), ImGui::GetFont()->EllipsisChar);
			return std::string(encoded.data());
		}

		std::string_view GetDistinctFileNamePart(std::string_view path)
		{
			size_t const separator = path.find_last_of('/');
			if (separator == std::string_view::npos)
			{
				return {};
			}
			std::string_view const name = path.substr(separator + 1);
			auto const equalIgnoringCase = [](std::string_view a, std::string_view b)
			{
				return a.size() == b.size() &&
				       std::equal(a.begin(), a.end(), b.begin(),
				                  [](char x, char y)
				                  {
									  return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
								  });
			};
			// The longest folder whose name, followed by a word separator, starts the file name.
			size_t prefix = 0;
			for (size_t start = 0; start < separator;)
			{
				size_t const end = path.find('/', start);
				std::string_view const folder = path.substr(start, end - start);
				if (!folder.empty() && folder.size() > prefix && name.size() > folder.size() + 1 && IsWordSeparator(name[folder.size()]) &&
				    equalIgnoringCase(name.substr(0, folder.size()), folder))
				{
					prefix = folder.size() + 1;
				}
				start = end + 1;
			}
			std::string_view const rest = name.substr(prefix);
			return prefix > 0 && SaysMoreThanResolution(rest) ? rest : std::string_view();
		}

		std::string FitPath(std::string_view path, float maxWidth)
		{
			auto const measure = [](std::string_view part)
			{
				return ImGui::CalcTextSize(part.data(), part.data() + part.size()).x;
			};
			if (measure(path) <= maxWidth)
			{
				return std::string(path);
			}
			std::string const ellipsis = GetEllipsis();
			float const available = maxWidth - measure(ellipsis);
			// Whole folders first: the longest end that starts at a separator. A scheme ("builtin://") is not a folder: its
			// references show the name alone.
			size_t const scheme = path.find("://");
			size_t const firstFolder = scheme == std::string_view::npos ? 0 : scheme + 3;
			for (size_t slash = path.find('/', firstFolder); slash != std::string_view::npos; slash = path.find('/', slash + 1))
			{
				if (measure(path.substr(slash)) <= available)
				{
					return ellipsis + std::string(path.substr(slash));
				}
			}
			// Then the name alone.
			size_t const separator = path.find_last_of('/');
			std::string_view const name = separator == std::string_view::npos ? path : path.substr(separator + 1);
			if (measure(name) <= maxWidth)
			{
				return std::string(name);
			}
			// The files of a set repeat the set's folder in their names ("wooden_crate_01/textures/wooden_crate_01_diff_1k.jpg"):
			// without that start, what tells them apart remains ("...diff_1k.jpg"), or as many of its last words as fit while
			// they say more than a resolution ("...arm_1k.jpg" of "brass_arm_1k.jpg").
			for (std::string_view part = GetDistinctFileNamePart(path); !part.empty() && SaysMoreThanResolution(part);
			     part = DropFirstWord(part))
			{
				if (measure(part) <= available)
				{
					return ellipsis + std::string(part);
				}
			}

			// At last the middle of the name gives way: its start names the asset, its end keeps the extension. The end gets up
			// to half of the room and starts at a word ("leaves.smat", ".smat") when one fits, the start the rest. Names are cut
			// between characters: UTF-8 continuation bytes (10xxxxxx) start none.
			auto const startsCharacter = [name](size_t index)
			{
				return (static_cast<unsigned char>(name[index]) & 0xC0) != 0x80;
			};
			size_t const extension = std::min(name.find_last_of('.'), name.size());
			auto const startsWord = [name, extension](size_t index)
			{
				return index == extension || IsWordSeparator(name[index - 1]) || StartsWord(name, index);
			};
			size_t endStart = name.size();
			size_t wordEndStart = name.size();
			for (size_t index = name.size(); index-- > 1;)
			{
				if (startsCharacter(index))
				{
					if (measure(name.substr(index)) > available * 0.5f)
					{
						break;
					}
					endStart = index;
					if (startsWord(index))
					{
						wordEndStart = index;
					}
				}
			}
			if (wordEndStart < name.size())
			{
				endStart = wordEndStart;
			}
			float const endWidth = measure(name.substr(endStart));
			size_t startEnd = 0;
			for (size_t index = 1; index < endStart; index++)
			{
				if (startsCharacter(index))
				{
					if (measure(name.substr(0, index)) + endWidth > available)
					{
						break;
					}
					startEnd = index;
				}
			}
			if (startEnd > 0)
			{
				return std::string(name.substr(0, startEnd)) + ellipsis + std::string(name.substr(endStart));
			}
			// Too narrow for both: as much of the start as fits.
			for (size_t index = name.size(); index-- > 1;)
			{
				if (startsCharacter(index) && measure(name.substr(0, index)) <= available)
				{
					return std::string(name.substr(0, index)) + ellipsis;
				}
			}
			return ellipsis;
		}

		void AssetDragSource(AssetHandle asset, std::string_view label)
		{
			if (!ImGui::BeginDragDropSource())
			{
				return;
			}
			uint64_t const value = asset.GetUUID().GetValue();
			ImGui::SetDragDropPayload(DragDropPayload::Asset, &value, sizeof(value));
			ImGui::TextUnformatted(label.data(), label.data() + label.size());
			ImGui::EndDragDropSource();
		}

		AssetHandle AcceptAssetDrop(std::function<bool(AssetMetadata const&)> const& accepts)
		{
			// Peeking first keeps the target from highlighting for assets it would refuse.
			ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::Asset, ImGuiDragDropFlags_AcceptPeekOnly);
			if (payload == nullptr || payload->DataSize != static_cast<int>(sizeof(uint64_t)) || !AssetManager::IsInitialized())
			{
				return AssetHandle();
			}
			uint64_t value = 0;
			std::memcpy(&value, payload->Data, sizeof(value));
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(AssetHandle(UUID(value)));
			if (!metadata || !accepts(*metadata))
			{
				return AssetHandle();
			}
			return ImGui::AcceptDragDropPayload(DragDropPayload::Asset) != nullptr ? metadata->Handle : AssetHandle();
		}

		AssetHandle AcceptAssetDrop(std::span<AssetType const> types)
		{
			return AcceptAssetDrop(
				[types](AssetMetadata const& metadata)
				{
					return types.empty() || std::find(types.begin(), types.end(), metadata.Type) != types.end();
				});
		}

		std::vector<UUID> ReadEntityPayload(ImGuiPayload const& payload)
		{
			size_t const count = static_cast<size_t>(payload.DataSize) / sizeof(uint64_t);
			std::vector<UUID> entities;
			entities.reserve(count);
			for (size_t i = 0; i < count; i++)
			{
				uint64_t value = 0;
				std::memcpy(&value, static_cast<char const*>(payload.Data) + i * sizeof(uint64_t), sizeof(value));
				entities.emplace_back(value);
			}
			return entities;
		}

		void ScriptDragSource(std::filesystem::path const& file, std::string_view label)
		{
			if (!ImGui::BeginDragDropSource())
			{
				return;
			}
			std::string const path = FileSystem::PathToUtf8(file);
			ImGui::SetDragDropPayload(DragDropPayload::ScriptFile, path.data(), path.size());
			ImGui::TextUnformatted(label.data(), label.data() + label.size());
			ImGui::EndDragDropSource();
		}

		std::optional<std::filesystem::path> AcceptScriptDrop()
		{
			ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::ScriptFile);
			if (payload == nullptr || payload->DataSize <= 0)
			{
				return std::nullopt;
			}
			return FileSystem::PathFromUtf8(
				std::string_view(static_cast<char const*>(payload->Data), static_cast<size_t>(payload->DataSize)));
		}

		std::array<std::string, 2> SplitTileLabel(std::string_view name, float width, std::string_view extension, std::string_view distinct)
		{
			if (TextWidth(name) <= width)
			{
				return {std::string(name), std::string()};
			}
			// The first line ends at the last place that fits after a space, '_' or '-', before a '.' (the extension then
			// starts the second line) or before a word of a PascalCase name ("Showcase|Tests"); a long word breaks where the
			// line is full. The text before index end always fits.
			size_t end = 0;
			size_t breakAt = 0;
			while (end < name.size())
			{
				char const c = name[end];
				if (end > 0 && (c == '.' || StartsWord(name, end)))
				{
					breakAt = end;
				}
				size_t const next = end + CodePointLength(name, end);
				if (TextWidth(name.substr(0, next)) > width)
				{
					break;
				}
				if (c == ' ' || c == '_' || c == '-')
				{
					breakAt = next;
				}
				end = next;
			}
			if (breakAt > 0)
			{
				end = breakAt;
			}
			// Every line shows at least one character, however narrow the tile.
			end = std::max(end, CodePointLength(name, 0));
			std::string_view first = name.substr(0, end);
			while (first.ends_with(' '))
			{
				first.remove_suffix(1);
			}
			std::string_view const rest = name.substr(end);
			// The files of a set repeat the set's name ("wooden_crate_01_diff_1k.jpg", "wooden_crate_01_arm_1k.jpg"): what tells
			// them apart stays, as in asset fields (FitPath).
			if (TextWidth(rest) > width && !distinct.empty() && rest.ends_with(distinct))
			{
				for (std::string_view part = distinct; !part.empty() && SaysMoreThanResolution(part); part = DropFirstWord(part))
				{
					if (TextWidth(TileEllipsis) + TextWidth(part) <= width)
					{
						return {std::string(first), std::string(TileEllipsis) + std::string(part)};
					}
				}
			}
			return {std::string(first), FitTextKeepingEnd(rest, width, rest.ends_with(extension) ? extension : std::string_view())};
		}

		std::string FormatDuration(double seconds)
		{
			// Negative and invalid lengths read as none; a billion seconds is beyond any clip.
			int64_t const milliseconds = std::isfinite(seconds) ? std::llround(std::clamp(seconds, 0.0, 1.0e9) * 1000.0) : 0;
			if (milliseconds < 1000)
			{
				return fmt::format("{} ms", milliseconds);
			}
			// From 9.95 seconds on, one decimal would round to "10.0 s".
			if (milliseconds < 9950)
			{
				return fmt::format("{:.1f} s", static_cast<double>(milliseconds) / 1000.0);
			}
			int64_t const total = (milliseconds + 500) / 1000;
			return fmt::format("{}:{:02}", total / 60, total % 60);
		}

		EntityPreset const* DrawEntityPresetMenuItems(std::string_view openSubmenu)
		{
			EntityPreset const* chosen = nullptr;
			for (EntityPresetMenuSection const& section : EntityPresets::GetMenu())
			{
				std::string const category(section.Category);
				if (section.Submenu && section.Category == openSubmenu)
				{
					// The submenu's popup has the ID BeginMenu gives it, from its label in this window.
					ImGui::OpenPopup(category.c_str());
				}
				if (section.Submenu && !ImGui::BeginMenu(category.c_str()))
				{
					continue;
				}
				for (EntityPreset const* preset : section.Presets)
				{
					if (ImGui::MenuItem(std::string(preset->Label).c_str()))
					{
						chosen = preset;
					}
				}
				if (section.Submenu)
				{
					ImGui::EndMenu();
				}
				else
				{
					ImGui::Separator();
				}
			}
			return chosen;
		}
	}

	EditSession::EditSession(uint64_t keyPrefix)
		: m_KeyPrefix(keyPrefix)
	{
	}

	void EditSession::Update(bool interactionActive, CommandHistory& history)
	{
		if (m_CurrentKey != 0 && !interactionActive)
		{
			history.BreakMerge();
			m_CurrentKey = 0;
		}
	}

	uint64_t EditSession::GetMergeKey()
	{
		if (m_CurrentKey == 0)
		{
			m_CurrentKey = m_KeyPrefix + ++m_SessionCount;
		}
		return m_CurrentKey;
	}
}
