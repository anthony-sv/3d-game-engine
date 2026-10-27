#include "Editor/UI/FieldEditor.h"

#include "Editor/AssetThumbnails.h"
#include "Editor/UI/EditorUI.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/ImGui/ImGuiRenderer.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<ImVec4, 4> AxisColors = {
			ImVec4(0.70f, 0.20f, 0.20f, 1.0f),
			ImVec4(0.25f, 0.56f, 0.25f, 1.0f),
			ImVec4(0.22f, 0.36f, 0.72f, 1.0f),
			ImVec4(0.42f, 0.42f, 0.42f, 1.0f),
		};
		constexpr std::array<char const*, 4> AxisLabels = {"X", "Y", "Z", "W"};
		constexpr ImVec4 MixedLabelColor = ImVec4(0.95f, 0.72f, 0.30f, 1.0f);
		constexpr ImVec4 MissingAssetColor = ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
		// The asset picker is at least this wide (in font sizes, so it follows the display scale) and lists this many rows
		// before it scrolls.
		constexpr float MinPickerWidthInFontSizes = 20.0f;
		constexpr size_t MaxPickerRows = 14;
		// An enum's list in a popup of its own is at least this wide (in font sizes), as is the widget of a value that opens no
		// popup of its own when DrawValuePopup draws it.
		constexpr float MinEnumListWidthInFontSizes = 12.0f;
		// The image of an asset in the tooltip of its field fits a square this large (in font sizes).
		constexpr float TooltipPreviewSizeInFontSizes = 9.0f;
		// The share of a table's width the name column starts with: what the names need, but never so much that the values
		// get less than 40% (50% beside asset paths), and tables of short names line up at the same share.
		constexpr float MinNameShare = 0.3f;
		constexpr float MaxNameShare = 0.6f;
		constexpr float MaxNameShareBesideAssets = 0.5f;
		constexpr char const* AngleFormat = "%.1f\xc2\xb0";
		// Compact widgets show numbers as short as they read, Euler degrees too (as the visual script canvas' chips they replace
		// show them: no degree sign).
		constexpr char const* CompactFloatFormat = "%g";
		// The axis stripe at the left edge of a compact vector's cells, as a share of the cell's height: the canvas chips' 2
		// units of 19.
		constexpr float CompactStripeShare = 2.0f / 19.0f;
		// Integer widgets stay within the integers JSON consumers represent exactly.
		constexpr int64_t IntegerWidgetLimit = int64_t(1) << 53;

		float ReadFloat(Json const& json)
		{
			return json.is_number() ? json.get<float>() : 0.0f;
		}

		std::array<float, 4> ReadFloats(Json const& json)
		{
			std::array<float, 4> values{};
			if (json.is_array())
			{
				for (size_t i = 0; i < std::min<size_t>(json.size(), values.size()); i++)
				{
					values[i] = ReadFloat(json[i]);
				}
			}
			return values;
		}

		Json WriteFloats(float const* values, int count)
		{
			Json json = Json::array();
			for (int i = 0; i < count; i++)
			{
				json.push_back(values[i]);
			}
			return json;
		}

		UUID ReadID(Json const& json)
		{
			if (json.is_string())
			{
				return UUID::FromString(json.get_ref<std::string const&>()).value_or(UUID::Invalid());
			}
			return UUID::Invalid();
		}

		int GetVectorSize(FieldKind kind)
		{
			switch (kind)
			{
				case FieldKind::Vec2:
					return 2;
				case FieldKind::Vec3:
					return 3;
				case FieldKind::Vec4:
					return 4;
				default:
					return 0;
			}
		}

		// The range numeric widgets clamp to: the hinted range within what the C++ type represents. Hints can come from user
		// data (the ranges of script fields and of visual script variables), so a hinted range no value of the type lies in
		// is left out.
		std::pair<double, double> GetBounds(FieldDescriptor const& field)
		{
			double const min = std::max(field.Hints.Min, field.TypeMin);
			double const max = std::min(field.Hints.Max, field.TypeMax);
			if (min <= max)
			{
				return {min, max};
			}
			return {field.TypeMin, field.TypeMax};
		}

		float ToFloatBound(double bound)
		{
			return static_cast<float>(std::clamp(bound, static_cast<double>(-FLT_MAX), static_cast<double>(FLT_MAX)));
		}

		// Pixels to value: a hinted range is crossed in about 300 pixels; unbounded values move in small steps.
		float GetDragSpeed(FieldDescriptor const& field)
		{
			if (field.Hints.HasMin() && field.Hints.HasMax())
			{
				return static_cast<float>(std::clamp((field.Hints.Max - field.Hints.Min) / 300.0, 0.001, 10.0));
			}
			if (field.Hints.Display == FieldDisplay::Angle)
			{
				return 0.5f;
			}
			return field.Kind == FieldKind::Int || field.Kind == FieldKind::UInt ? 0.25f : 0.05f;
		}

		std::string MakeTooltip(FieldDescriptor const& field, bool mixed)
		{
			std::string text(field.Hints.Description);
			auto const appendLine = [&text](std::string const& line)
			{
				text += text.empty() ? "" : "\n";
				text += line;
			};
			if (field.Hints.HasMin() && field.Hints.HasMax())
			{
				appendLine(fmt::format("Range: {} to {}", field.Hints.Min, field.Hints.Max));
			}
			else if (field.Hints.HasMin())
			{
				appendLine(fmt::format("Minimum: {}", field.Hints.Min));
			}
			else if (field.Hints.HasMax())
			{
				appendLine(fmt::format("Maximum: {}", field.Hints.Max));
			}
			if (mixed)
			{
				appendLine("The selected entities have different values; editing sets them all.");
			}
			return text;
		}

		void Indent(int depth)
		{
			if (depth > 0)
			{
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(depth) * ImGui::GetStyle().IndentSpacing);
			}
		}

		// One line of text in the space left in the cell, shortened with an ellipsis when it does not fit; returns whether it
		// was shortened.
		bool DrawFittedText(std::string const& text)
		{
			ImGuiWindow* window = ImGui::GetCurrentWindow();
			ImVec2 const size = ImGui::CalcTextSize(text.c_str());
			float const available = ImGui::GetContentRegionAvail().x;
			if (window->SkipItems || size.x <= available)
			{
				ImGui::TextUnformatted(text.c_str());
				return false;
			}
			// Laid out like TextUnformatted: on the line's text baseline, as one item.
			ImVec2 const position(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
			ImRect const bounds(position, ImVec2(position.x + std::max(available, 0.0f), position.y + size.y));
			ImGui::ItemSize(bounds.GetSize(), 0.0f);
			if (ImGui::ItemAdd(bounds, 0))
			{
				ImGui::RenderTextEllipsis(window->DrawList, bounds.Min, bounds.Max, bounds.Max.x, text.c_str(), nullptr, &size);
			}
			return true;
		}

		// Labels of fields whose switch is off are dimmed (their tooltips still show). A label shortened to fit its cell
		// shows in full on the first line of its tooltip.
		void DrawLabel(std::string const& label, std::string const& tooltip, bool mixed, int depth, bool switchedOn = true)
		{
			ImGui::AlignTextToFramePadding();
			Indent(depth);
			bool const colored = mixed || !switchedOn;
			if (colored)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, mixed ? MixedLabelColor : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			}
			bool const shortened = DrawFittedText(label);
			if (colored)
			{
				ImGui::PopStyleColor();
			}
			std::string const text = !shortened ? tooltip : tooltip.empty() ? label : label + "\n" + tooltip;
			if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", text.c_str());
			}
		}

		FieldDescriptor const* FindField(std::span<FieldDescriptor const> fields, std::string_view name)
		{
			auto const found = std::find_if(fields.begin(), fields.end(),
			                                [name](FieldDescriptor const& field)
			                                {
												return field.Name == name;
											});
			return found != fields.end() ? &*found : nullptr;
		}

		// Switches (EnabledBy) are drawn in front of the fields they switch instead of in rows of their own.
		bool IsSwitch(std::span<FieldDescriptor const> fields, FieldDescriptor const& field)
		{
			return field.Kind == FieldKind::Bool && std::any_of(fields.begin(), fields.end(),
			                                                    [&field](FieldDescriptor const& other)
			                                                    {
																	return other.Hints.EnabledBy == field.Name;
																});
		}

		// The widest name cell of the rows of these fields (and of the nested structs' rows, open or not), laid out like
		// DrawRows lays them out: the indentation, a switch's check box, the tree node's arrow and the name.
		float MeasureNames(std::span<FieldDescriptor const> fields, int depth)
		{
			ImGuiStyle const& style = ImGui::GetStyle();
			float const indent = static_cast<float>(depth) * style.IndentSpacing;
			float widest = 0.0f;
			for (FieldDescriptor const& field : fields)
			{
				if (IsSwitch(fields, field) || field.Hints.HiddenInEditors)
				{
					continue;
				}
				float width = indent + ImGui::CalcTextSize(UI::FormatDisplayName(field.Name).c_str()).x;
				if (field.Kind == FieldKind::Struct)
				{
					width += ImGui::GetTreeNodeToLabelSpacing();
					widest = std::max(widest, MeasureNames(field.Fields, depth + 1));
				}
				else if (!field.Hints.EnabledBy.empty() && FindField(fields, field.Hints.EnabledBy) != nullptr)
				{
					width += ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
				}
				widest = std::max(widest, width);
			}
			return widest;
		}

		// Whether rows of these fields (or of their nested structs) show asset paths.
		bool HasAssetFields(std::span<FieldDescriptor const> fields)
		{
			return std::any_of(fields.begin(), fields.end(),
			                   [](FieldDescriptor const& field)
			                   {
								   return !field.Hints.HiddenInEditors &&
				                          (field.Kind == FieldKind::Asset ||
				                           (field.Kind == FieldKind::Array && field.ElementKind == FieldKind::Asset) ||
				                           HasAssetFields(field.Fields));
							   });
		}

		// Colored X/Y/Z/W buttons (reset the component to its default) followed by drag fields; reports which component changed.
		bool DrawVectorComponents(float* values, int count, float speed, float min, float max, char const* format, float const* defaults,
		                          int& changedComponent)
		{
			bool changed = false;
			float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			float const buttonWidth = ImGui::GetFrameHeight();
			float const totalWidth = ImGui::CalcItemWidth();
			float const fieldWidth =
				std::max(1.0f, (totalWidth - static_cast<float>(count) * buttonWidth - static_cast<float>(count - 1) * spacing) /
			                       static_cast<float>(count));
			for (int i = 0; i < count; i++)
			{
				ImGui::PushID(i);
				if (i > 0)
				{
					ImGui::SameLine(0.0f, spacing);
				}
				ImVec4 const color = AxisColors[static_cast<size_t>(i)];
				ImGui::PushStyleColor(ImGuiCol_Button, color);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 1.25f, color.y * 1.25f, color.z * 1.25f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
				if (ImGui::Button(AxisLabels[static_cast<size_t>(i)], ImVec2(buttonWidth, 0.0f)))
				{
					values[i] = defaults[i];
					changed = true;
					changedComponent = i;
				}
				ImGui::PopStyleColor(3);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
				{
					ImGui::SetTooltip("Reset to %g", static_cast<double>(defaults[i]));
				}
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::SetNextItemWidth(fieldWidth);
				if (ImGui::DragFloat("##component", &values[i], speed, min, max, format, ImGuiSliderFlags_AlwaysClamp))
				{
					changed = true;
					changedComponent = i;
				}
				ImGui::PopID();
			}
			return changed;
		}

		// Drag fields without axis buttons, one per component in the item's width, each with its axis's stripe at its left edge
		// (compact widgets); reports which component changed.
		bool DrawComponentCells(float* values, int count, float speed, float min, float max, char const* format, int& changedComponent)
		{
			ImGuiStyle const& style = ImGui::GetStyle();
			bool changed = false;
			float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			float const cellWidth =
				std::max(1.0f, (ImGui::CalcItemWidth() - static_cast<float>(count - 1) * spacing) / static_cast<float>(count));
			for (int i = 0; i < count; i++)
			{
				ImGui::PushID(i);
				if (i > 0)
				{
					ImGui::SameLine(0.0f, spacing);
				}
				ImGui::SetNextItemWidth(cellWidth);
				if (ImGui::DragFloat("##component", &values[i], speed, min, max, format, ImGuiSliderFlags_AlwaysClamp))
				{
					changed = true;
					changedComponent = i;
				}
				ImVec2 const cellMin = ImGui::GetItemRectMin();
				ImVec2 const cellMax = ImGui::GetItemRectMax();
				float const stripe = std::max(1.0f, (cellMax.y - cellMin.y) * CompactStripeShare);
				ImGui::GetWindowDrawList()->AddRectFilled(cellMin, ImVec2(cellMin.x + stripe, cellMax.y),
				                                          ImGui::GetColorU32(AxisColors[static_cast<size_t>(std::min(i, 3))]),
				                                          style.FrameRounding, ImDrawFlags_RoundCornersLeft);
				ImGui::PopID();
			}
			return changed;
		}

		// Whether a row is shown: always, unless the field's visibility condition names a sibling (one of `fields`, valued in
		// `values`) whose value is none of the condition's. A sibling whose values differ between the edited objects (mixed)
		// shows it.
		bool IsVisible(FieldDescriptor const& field, std::span<FieldDescriptor const> fields, Json const& values,
		               std::span<std::string const> mixedFields)
		{
			std::string_view const siblingName = field.Hints.VisibleWhenField;
			if (siblingName.empty() || std::find(mixedFields.begin(), mixedFields.end(), siblingName) != mixedFields.end())
			{
				return true;
			}
			FieldDescriptor const* sibling = FindField(fields, siblingName);
			if (sibling == nullptr)
			{
				return true;
			}
			std::string const key(siblingName);
			Json const& value = values.is_object() && values.contains(key) ? values[key] : sibling->Default;
			std::span<std::string_view const> const accepted = field.Hints.GetVisibleWhenValues();
			auto const accepts = [accepted](std::string_view name)
			{
				return std::find(accepted.begin(), accepted.end(), name) != accepted.end();
			};
			if (value.is_boolean())
			{
				return accepts(value.get<bool>() ? "true" : "false");
			}
			return !value.is_string() || accepts(value.get_ref<std::string const&>());
		}

		// Text color of an asset reference: dimmed without an asset, the error color when its file is missing or the handle is
		// unknown.
		ImU32 GetAssetReferenceColor(AssetHandle handle, bool mixed)
		{
			if (mixed || !handle.IsValid())
			{
				return ImGui::GetColorU32(ImGuiCol_TextDisabled);
			}
			if (AssetManager::IsInitialized() && (!AssetManager::IsValid(handle) || AssetManager::IsMissing(handle)))
			{
				return ImGui::GetColorU32(MissingAssetColor);
			}
			return ImGui::GetColorU32(ImGuiCol_Text);
		}

		// The part of an image that fills a square: the middle of wide or tall textures.
		std::pair<ImVec2, ImVec2> GetSquareUVs(nvrhi::ITexture* texture)
		{
			nvrhi::TextureDesc const& desc = texture->getDesc();
			float const aspect = static_cast<float>(desc.width) / static_cast<float>(std::max(desc.height, 1u));
			if (aspect >= 1.0f)
			{
				float const margin = (1.0f - 1.0f / aspect) * 0.5f;
				return {ImVec2(margin, 0.0f), ImVec2(1.0f - margin, 1.0f)};
			}
			float const margin = (1.0f - aspect) * 0.5f;
			return {ImVec2(0.0f, margin), ImVec2(1.0f, 1.0f - margin)};
		}

		// The image of an asset in a square, when `previews` makes images of its type: the asset's image, or a placeholder while
		// the image is made. Returns false (and draws nothing) for assets without images.
		bool DrawAssetImage(AssetThumbnails* previews, AssetHandle asset, ImVec2 min, float size)
		{
			if (previews == nullptr || !AssetManager::IsInitialized() || !AssetThumbnails::Supports(AssetManager::GetAssetType(asset)) ||
			    AssetManager::IsMissing(asset))
			{
				return false;
			}
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			ImVec2 const max(min.x + size, min.y + size);
			float const rounding = ImGui::GetStyle().FrameRounding * 0.5f;
			if (nvrhi::ITexture* const image = previews->Get(asset))
			{
				// GetColorU32 applies the style's alpha: the images of disabled (read-only) fields fade like their text.
				auto const [uv0, uv1] = GetSquareUVs(image);
				drawList->AddImageRounded(ImGuiRenderer::GetTextureID(image), min, max, uv0, uv1, ImGui::GetColorU32(IM_COL32_WHITE),
				                          rounding);
			}
			else
			{
				drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), rounding);
			}
			return true;
		}

		// A row of the asset picker: the asset's image (when `previews` makes one) and its path, shortened to the row (the
		// tooltip has all of it); returns true when clicked.
		bool DrawAssetPickerRow(AssetThumbnails* previews, AssetHandle asset, std::string const& label, bool selected)
		{
			ImVec2 const position = ImGui::GetCursorScreenPos();
			float const right = position.x + ImGui::GetContentRegionAvail().x;
			bool const clicked = ImGui::Selectable("##asset", selected);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			{
				ImGui::SetTooltip("%s", label.c_str());
			}
			float const imageSize = ImGui::GetTextLineHeight();
			float const textX = DrawAssetImage(previews, asset, position, imageSize)
			                        ? position.x + imageSize + ImGui::GetStyle().ItemInnerSpacing.x
			                        : position.x;
			std::string const text = UI::FitPath(label, right - textX);
			ImGui::GetWindowDrawList()->AddText(ImVec2(textX, position.y), GetAssetReferenceColor(asset, false), text.c_str());
			return clicked;
		}

		// A box framed like a text field showing the referenced asset (long paths shortened), after a small image of it when
		// `previews` makes one (the place stays while the image is made); returns true when clicked. The tooltip has the whole
		// path and a larger image.
		// An asset as the visual script canvas' chips name it: its display name (a memory asset's name), marked when its file
		// is missing; "None" without one.
		std::string DescribeCompactAsset(AssetHandle handle)
		{
			if (!handle.IsValid())
			{
				return "None";
			}
			if (!AssetManager::IsInitialized())
			{
				return handle.ToString();
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
			if (!metadata)
			{
				return "Unknown asset " + handle.ToString();
			}
			std::string name = metadata->GetDisplayName();
			return AssetManager::IsMissing(handle) ? name + " (missing)" : name;
		}

		bool DrawAssetReferenceBox(AssetHandle handle, bool mixed, float width, AssetThumbnails* previews, bool compact = false)
		{
			ImGuiStyle const& style = ImGui::GetStyle();
			bool const clicked = ImGui::InvisibleButton("##reference", ImVec2(std::max(1.0f, width), ImGui::GetFrameHeight()));
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			ImGuiCol const background =
				ImGui::IsItemActive() ? ImGuiCol_FrameBgActive : (ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
			ImGui::RenderFrame(min, max, ImGui::GetColorU32(background), true, style.FrameRounding);
			ImDrawList* drawList = ImGui::GetWindowDrawList();

			float const inset = std::max(1.0f, std::floor(style.FramePadding.y * 0.5f));
			float const imageSize = max.y - min.y - inset * 2.0f;
			bool const pictured =
				!mixed && handle.IsValid() && DrawAssetImage(previews, handle, ImVec2(min.x + inset, min.y + inset), imageSize);
			float const textX = pictured ? min.x + inset + imageSize + style.ItemInnerSpacing.x : min.x + style.FramePadding.x;
			std::string const text = UI::FitPath(mixed     ? std::string("--")
			                                     : compact ? DescribeCompactAsset(handle)
			                                               : UI::DescribeAssetReference(handle),
			                                     max.x - style.FramePadding.x - textX);
			drawList->PushClipRect(min, max, true);
			drawList->AddText(ImVec2(textX, min.y + style.FramePadding.y), GetAssetReferenceColor(handle, mixed), text.c_str());
			drawList->PopClipRect();

			// A compact box is drawn at the canvas's zoom, where a tooltip would take the zoomed font and spacing (spec §7.8 rule
			// 3): it shows none.
			if (!compact && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
			{
				// The whole path, which the box may shorten, the image (whole textures) and how to change it unless the field is
				// read-only.
				bool const disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(mixed ? "The selected entities reference different assets."
				                             : UI::DescribeAssetReference(handle).c_str());
				if (nvrhi::ITexture* const preview = pictured ? previews->Get(handle) : nullptr)
				{
					nvrhi::TextureDesc const& desc = preview->getDesc();
					float const size = ImGui::GetFontSize() * TooltipPreviewSizeInFontSizes;
					float const scale = size / static_cast<float>(std::max({desc.width, desc.height, 1u}));
					ImGui::Image(ImGuiRenderer::GetTextureID(preview),
					             ImVec2(static_cast<float>(desc.width) * scale, static_cast<float>(desc.height) * scale));
				}
				if (!disabled)
				{
					ImGui::TextDisabled("Click to choose an asset, or drop one here.");
				}
				ImGui::EndTooltip();
			}
			return clicked;
		}

		bool IsFlagSet(Json const& value, std::string_view name)
		{
			return value.is_array() && std::any_of(value.begin(), value.end(),
			                                       [name](Json const& element)
			                                       {
													   return element.is_string() && element.get_ref<std::string const&>() == name;
												   });
		}

		// How wide an enum's list is drawn: its longest name, at least the width of a short field.
		float GetEnumListWidth(FieldDescriptor const& field)
		{
			ImGuiStyle const& style = ImGui::GetStyle();
			float labelWidth = 0.0f;
			for (std::string_view const name : field.EnumValues)
			{
				labelWidth = std::max(labelWidth, ImGui::CalcTextSize(UI::FormatDisplayName(name).c_str()).x);
			}
			return std::max(labelWidth + style.FramePadding.x * 2.0f + style.ScrollbarSize,
			                ImGui::GetFontSize() * MinEnumListWidthInFontSizes);
		}

		// The set flags' stored names joined with " | ", as a compact frame shows them (the canvas chip's text).
		std::string DescribeCompactFlags(FieldDescriptor const& field, Json const& value, bool mixed)
		{
			if (mixed)
			{
				return "--";
			}
			std::string text;
			for (std::string_view const name : field.EnumValues)
			{
				if (IsFlagSet(value, name))
				{
					text += (text.empty() ? "" : " | ") + std::string(name);
				}
			}
			return text.empty() ? std::string("None") : text;
		}

		// The set flags' names, as a combo shows them.
		std::string DescribeFlags(FieldDescriptor const& field, Json const& value, bool mixed)
		{
			if (mixed)
			{
				return "--";
			}
			std::string preview;
			for (std::string_view const name : field.EnumValues)
			{
				if (IsFlagSet(value, name))
				{
					preview += (preview.empty() ? "" : ", ") + UI::FormatDisplayName(name);
				}
			}
			return preview.empty() ? std::string("None") : preview;
		}

		// A check box per flag; the value is the array of the set names, in declaration order.
		bool DrawFlagList(FieldDescriptor const& field, Json const& value, FieldChange& change)
		{
			bool changed = false;
			Json names = Json::array();
			for (std::string_view const name : field.EnumValues)
			{
				bool set = IsFlagSet(value, name);
				if (ImGui::Checkbox(UI::FormatDisplayName(name).c_str(), &set))
				{
					changed = true;
				}
				if (set)
				{
					names.push_back(std::string(name));
				}
			}
			if (changed)
			{
				change.Value = std::move(names);
			}
			return changed;
		}

		// Flags enums: a combo of check boxes.
		bool DrawFlags(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change)
		{
			bool changed = false;
			if (ImGui::BeginCombo("##value", DescribeFlags(field, value, mixed).c_str()))
			{
				changed = DrawFlagList(field, value, change);
				ImGui::EndCombo();
			}
			return changed;
		}

		bool IsColor(FieldDescriptor const& field)
		{
			return field.Hints.Display == FieldDisplay::Color && GetVectorSize(field.Kind) >= 3;
		}

		// Colours are linear in the data and shown and picked in sRGB, so the swatch shows what the renderer displays.
		std::array<float, 4> ToDisplayColor(Json const& value)
		{
			std::array<float, 4> display = ReadFloats(value);
			for (size_t i = 0; i < 3; i++)
			{
				display[i] = Math::LinearToSrgb(std::clamp(display[i], 0.0f, 1.0f));
			}
			return display;
		}

		// A colour edited in sRGB by a widget that edits the display values in place (a colour edit or picker), back to the
		// data's linear values.
		bool EditColor(FieldDescriptor const& field, Json const& value, FieldChange& change, bool (*edit)(float* display, int components))
		{
			int const size = GetVectorSize(field.Kind);
			std::array<float, 4> values = ReadFloats(value);
			std::array<float, 4> display = ToDisplayColor(value);
			if (!edit(display.data(), size))
			{
				return false;
			}
			auto const [min, max] = GetBounds(field);
			for (size_t i = 0; i < static_cast<size_t>(size); i++)
			{
				float const linear = i < 3 ? Math::SrgbToLinear(display[i]) : display[i];
				values[i] = std::clamp(linear, ToFloatBound(min), ToFloatBound(max));
			}
			change.Value = WriteFloats(values.data(), size);
			return true;
		}

		ImGuiColorEditFlags GetColorFlags(int components)
		{
			return components == 4 ? ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_None;
		}

		// A frame drawn like a closed combo box (the text, then the arrow box), as wide as the next item: compact widgets draw
		// it in place of a combo and report its click instead of opening a popup. Returns true when clicked.
		bool DrawChoiceFrame(std::string const& text)
		{
			ImGuiStyle const& style = ImGui::GetStyle();
			float const height = ImGui::GetFrameHeight();
			bool const clicked = ImGui::InvisibleButton("##value", ImVec2(std::max(1.0f, ImGui::CalcItemWidth()), height));
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			bool const hovered = ImGui::IsItemHovered();
			ImGuiCol const background =
				ImGui::IsItemActive() ? ImGuiCol_FrameBgActive : (hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
			ImGui::RenderFrame(min, max, ImGui::GetColorU32(background), true, style.FrameRounding);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			float textRight = max.x - style.FramePadding.x;
			if (max.x - min.x > height * 2.0f)
			{
				// The combo box's arrow box, at its right end.
				textRight = max.x - height;
				drawList->AddRectFilled(ImVec2(textRight, min.y), max,
				                        ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), style.FrameRounding,
				                        ImDrawFlags_RoundCornersRight);
				ImGui::RenderArrow(drawList, ImVec2(textRight + style.FramePadding.y, min.y + style.FramePadding.y),
				                   ImGui::GetColorU32(ImGuiCol_Text), ImGuiDir_Down, 1.0f);
			}
			ImGui::RenderTextClipped(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y), ImVec2(textRight, max.y),
			                         text.c_str(), nullptr, nullptr);
			return clicked;
		}

		// An enumerator's rank in a filtered list: 0 when its name (or label) is the filter text, 1 when it starts with it, 2
		// when it holds it, ignoring the case of ASCII letters; nothing when it does neither.
		std::optional<int> RankEnumerator(std::string const& name, std::string const& label, std::string const& filter)
		{
			auto const startsWith = [&filter](std::string const& text)
			{
				return text.size() >= filter.size() && ImStrnicmp(text.c_str(), filter.c_str(), filter.size()) == 0;
			};
			auto const equals = [&filter, &startsWith](std::string const& text)
			{
				return text.size() == filter.size() && startsWith(text);
			};
			auto const holds = [&filter](std::string const& text)
			{
				return ImStristr(text.c_str(), nullptr, filter.c_str(), nullptr) != nullptr;
			};
			if (equals(name) || equals(label))
			{
				return 0;
			}
			if (startsWith(name) || startsWith(label))
			{
				return 1;
			}
			if (holds(name) || holds(label))
			{
				return 2;
			}
			return std::nullopt;
		}

		// A search field given text before it had the keyboard: its first active frame puts the cursor after the text (ImGui
		// selects all of a field the keyboard is given to, and the next character would replace it).
		void PlaceCursorAtEnd(ImGuiInputTextCallbackData* data, bool& pending)
		{
			if (pending)
			{
				data->CursorPos = data->BufTextLen;
				data->SelectionStart = data->BufTextLen;
				data->SelectionEnd = data->BufTextLen;
				pending = false;
			}
		}

		// Enter pressed while a search field is active. ImGui routes the key to a field that just took the keyboard only from
		// the next frame, so the field alone would miss an Enter pressed right after its popup opened.
		bool IsEnterPressed()
		{
			return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		}

		// Takes the characters typed this frame into a search field that is still to get the keyboard (a popup opens with its
		// field asked for it, which takes a frame or two): what is typed as a list opens is its filter.
		bool TakeTypedCharacters(std::string& text)
		{
			ImGuiIO& io = ImGui::GetIO();
			if (io.InputQueueCharacters.empty())
			{
				return false;
			}
			for (ImWchar const character : io.InputQueueCharacters)
			{
				if (character >= 0x20 && character != 0x7F)
				{
					std::array<char, 5> utf8{};
					ImTextCharToUtf8(utf8.data(), character);
					text += utf8.data();
				}
			}
			io.InputQueueCharacters.resize(0);
			return true;
		}

		// Text without its leading and trailing spaces.
		std::string Trim(std::string const& text)
		{
			size_t const first = text.find_first_not_of(' ');
			return first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(' ') - first + 1);
		}

		// Entity reference: the entity's name, an entity drag-and-drop target and, with menu, a context menu to clear it.
		bool DrawEntityReference(Json const& value, Scene* scene, FieldChange& change, bool menu)
		{
			UUID const id = ReadID(value);
			std::string label = "None";
			if (id.IsValid())
			{
				Entity const entity = scene != nullptr ? scene->GetEntityByUUID(id) : Entity();
				label = entity ? entity.GetName() : fmt::format("Missing entity {}", id);
			}
			bool changed = false;
			ImGui::Button(fmt::format("{}###entity", label).c_str(), ImVec2(ImGui::CalcItemWidth(), 0.0f));
			if (ImGui::BeginDragDropTarget())
			{
				if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::Entities))
				{
					if (payload->DataSize >= static_cast<int>(sizeof(uint64_t)))
					{
						uint64_t dropped = 0;
						std::memcpy(&dropped, payload->Data, sizeof(dropped));
						change.Value = UUID(dropped).ToString();
						changed = true;
					}
				}
				ImGui::EndDragDropTarget();
			}
			if (menu && ImGui::BeginPopupContextItem("##entityMenu"))
			{
				if (ImGui::MenuItem("Clear", nullptr, false, id.IsValid()))
				{
					change.Value = UUID::Invalid().ToString();
					changed = true;
				}
				ImGui::EndPopup();
			}
			return changed;
		}

		FieldDescriptor MakeElementDescriptor(FieldDescriptor const& array)
		{
			FieldDescriptor element;
			element.Name = array.Name;
			element.Kind = array.ElementKind;
			element.Hints = array.Hints;
			element.TypeMin = array.TypeMin;
			element.TypeMax = array.TypeMax;
			element.EnumValues = array.EnumValues;
			element.Fields = array.Fields;
			switch (element.Kind)
			{
				case FieldKind::Bool:
					element.Default = false;
					break;
				case FieldKind::Int:
				case FieldKind::UInt:
				case FieldKind::Float:
				case FieldKind::Double:
				{
					double const zero = std::clamp(0.0, std::max(array.Hints.Min, array.TypeMin), std::min(array.Hints.Max, array.TypeMax));
					element.Default =
						element.Kind == FieldKind::Int || element.Kind == FieldKind::UInt ? Json(static_cast<int64_t>(zero)) : Json(zero);
					break;
				}
				case FieldKind::String:
					element.Default = "";
					break;
				case FieldKind::UUID:
				case FieldKind::Asset:
					element.Default = UUID::Invalid().ToString();
					break;
				case FieldKind::Enum:
					element.Default = element.EnumValues.empty() ? Json("") : Json(std::string(element.EnumValues.front()));
					break;
				default:
				{
					int const size = GetVectorSize(element.Kind);
					std::array<float, 4> const zeros{};
					element.Default = size > 0 ? WriteFloats(zeros.data(), size) : Json();
					break;
				}
			}
			return element;
		}
	}

	Json FieldChange::MakePatch(Json const& values) const
	{
		if (Path.empty())
		{
			return Json::object();
		}

		std::string const root(Path.front());
		Json field = values.is_object() && values.contains(root) ? values[root] : Json();
		Json* node = &field;
		for (size_t i = 1; i < Path.size(); i++)
		{
			if (!node->is_object())
			{
				*node = Json::object();
			}
			node = &(*node)[std::string(Path[i])];
		}

		if (EulerDegrees && Component && *Component < 3 && node->is_array() && node->size() == 4)
		{
			// Other objects keep their own angles around the axes the user did not touch.
			std::array<float, 4> const current = ReadFloats(*node);
			glm::quat const rotation = glm::quat::wxyz(current[3], current[0], current[1], current[2]);
			glm::vec3 degrees = glm::length(rotation) > 1e-6f ? Math::QuaternionToEulerDegrees(rotation) : glm::vec3(0.0f);
			degrees[static_cast<glm::length_t>(*Component)] = (*EulerDegrees)[static_cast<glm::length_t>(*Component)];
			glm::quat const updated = Math::EulerDegreesToQuaternion(degrees);
			*node = Json::array({updated.x, updated.y, updated.z, updated.w});
		}
		else if (Component && !EulerDegrees && node->is_array() && Value.is_array() && node->size() == Value.size() &&
		         *Component < Value.size())
		{
			(*node)[*Component] = Value[*Component];
		}
		else
		{
			*node = Value;
		}

		Json patch = Json::object();
		patch[root] = std::move(field);
		return patch;
	}

	float FieldEditor::GetNameColumnShare(std::span<FieldDescriptor const> fields, float tableWidth)
	{
		// The contents of the two columns are separated by the one-pixel border of a resizable table and the cell padding on
		// both sides of it; one more pixel covers the rounding of the columns' widths down to whole pixels.
		float const contentWidth = tableWidth - 2.0f * ImGui::GetStyle().CellPadding.x - 1.0f;
		if (!(contentWidth > 0.0f))
		{
			return MinNameShare;
		}
		float const maxShare = HasAssetFields(fields) ? MaxNameShareBesideAssets : MaxNameShare;
		return std::clamp((MeasureNames(fields, 0) + 1.0f) / contentWidth, MinNameShare, maxShare);
	}

	std::optional<FieldChange> FieldEditor::Draw(std::span<FieldDescriptor const> fields, Json const& values,
	                                             std::span<std::string const> mixedFields, Scene* scene)
	{
		PruneEulerStates();
		m_Scene = scene;
		std::optional<FieldChange> change;
		float const nameShare = GetNameColumnShare(fields, ImGui::GetContentRegionAvail().x);
		if (!ImGui::BeginTable("##fields", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings))
		{
			return change;
		}
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, nameShare);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f - nameShare);
		std::vector<std::string_view> path;
		DrawRows(fields, values, mixedFields, false, path, change);
		ImGui::EndTable();
		m_Scene = nullptr;
		return change;
	}

	void FieldEditor::DrawRows(std::span<FieldDescriptor const> fields, Json const& values, std::span<std::string const> mixedFields,
	                           bool parentMixed, std::vector<std::string_view>& path, std::optional<FieldChange>& change)
	{
		int const depth = static_cast<int>(path.size());
		auto const isMixed = [&](std::string const& key)
		{
			return parentMixed || (depth == 0 && std::find(mixedFields.begin(), mixedFields.end(), key) != mixedFields.end());
		};
		auto const getValue = [&values](FieldDescriptor const& field, std::string const& key) -> Json const&
		{
			return values.is_object() && values.contains(key) ? values[key] : field.Default;
		};
		for (FieldDescriptor const& field : fields)
		{
			// Switches are drawn in front of the fields they switch; fields that do not matter for their siblings' values are
			// hidden, as are those edited by UI of their own.
			if (IsSwitch(fields, field) || field.Hints.HiddenInEditors ||
			    (!parentMixed && !IsVisible(field, fields, values, depth == 0 ? mixedFields : std::span<std::string const>())))
			{
				continue;
			}
			std::string const key(field.Name);
			bool const mixed = isMixed(key);
			Json const& value = getValue(field, key);

			ImGui::PushID(key.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			std::string const label = UI::FormatDisplayName(field.Name);
			path.push_back(field.Name);
			if (field.Kind == FieldKind::Struct)
			{
				if (depth > 0)
				{
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(depth) * ImGui::GetStyle().IndentSpacing);
				}
				bool const open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen);
				if (open)
				{
					DrawRows(field.Fields, value, {}, mixed, path, change);
				}
			}
			else
			{
				// A switched field shows its switch in front of its name, and its value disabled while the switch is off
				// (enabled when the selected objects differ, so all of them can be edited).
				bool enabled = true;
				FieldDescriptor const* switchField = field.Hints.EnabledBy.empty() ? nullptr : FindField(fields, field.Hints.EnabledBy);
				if (switchField != nullptr)
				{
					std::string const switchKey(switchField->Name);
					bool const switchMixed = isMixed(switchKey);
					Json const& switchValue = getValue(*switchField, switchKey);
					bool checked = switchValue.is_boolean() && switchValue.get<bool>();
					enabled = checked || switchMixed;

					ImGui::AlignTextToFramePadding();
					Indent(depth);
					ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, switchMixed);
					bool const switched = ImGui::Checkbox("##switch", &checked);
					ImGui::PopItemFlag();
					if (std::string const tooltip = MakeTooltip(*switchField, switchMixed);
					    !tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
					{
						ImGui::SetTooltip("%s", tooltip.c_str());
					}
					if (switched && !change)
					{
						FieldChange switchChange;
						switchChange.Path = path;
						switchChange.Path.back() = switchField->Name;
						switchChange.Value = checked;
						change = std::move(switchChange);
					}
					ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
					DrawLabel(label, MakeTooltip(field, mixed), mixed, 0, enabled);
				}
				else
				{
					DrawLabel(label, MakeTooltip(field, mixed), mixed, depth);
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::PushItemWidth(-FLT_MIN);
				ImGui::BeginDisabled(!enabled);
				FieldChange fieldChange;
				if (DrawValue(field, value, mixed, fieldChange) && !change)
				{
					fieldChange.Path = path;
					change = std::move(fieldChange);
				}
				ImGui::EndDisabled();
				ImGui::PopItemWidth();
			}
			path.pop_back();
			ImGui::PopID();
		}
	}

	bool FieldEditor::DrawValue(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change)
	{
		switch (field.Kind)
		{
			case FieldKind::Bool:
			{
				bool checked = value.is_boolean() && value.get<bool>();
				ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed);
				bool const changed = ImGui::Checkbox("##value", &checked);
				ImGui::PopItemFlag();
				if (changed)
				{
					change.Value = checked;
				}
				return changed;
			}
			case FieldKind::Int:
			case FieldKind::UInt:
			{
				auto const [min, max] = GetBounds(field);
				// Clamped before the conversion: a bound past the range of int64_t would not convert.
				constexpr double Limit = static_cast<double>(IntegerWidgetLimit);
				int64_t const lower = static_cast<int64_t>(std::clamp(min, -Limit, Limit));
				int64_t const upper = static_cast<int64_t>(std::clamp(max, -Limit, Limit));
				int64_t number = 0;
				if (value.is_number_unsigned())
				{
					number = static_cast<int64_t>(std::min<uint64_t>(value.get<uint64_t>(), static_cast<uint64_t>(upper)));
				}
				else if (value.is_number_integer())
				{
					number = value.get<int64_t>();
				}
				if (!ImGui::DragScalar("##value", ImGuiDataType_S64, &number, GetDragSpeed(field), &lower, &upper, "%lld",
				                       ImGuiSliderFlags_AlwaysClamp))
				{
					return false;
				}
				change.Value = field.Kind == FieldKind::UInt ? Json(static_cast<uint64_t>(std::max<int64_t>(number, 0))) : Json(number);
				return true;
			}
			case FieldKind::Float:
			case FieldKind::Double:
			{
				auto const [min, max] = GetBounds(field);
				double const current = value.is_number() ? value.get<double>() : 0.0;
				// Small values (time steps, thresholds) keep their significant digits.
				char const* format = field.Hints.Display == FieldDisplay::Angle  ? AngleFormat
				                     : current != 0.0 && std::abs(current) < 0.1 ? "%.5f"
				                                                                 : "%.3f";
				if (field.Kind == FieldKind::Double)
				{
					double number = current;
					if (!ImGui::DragScalar("##value", ImGuiDataType_Double, &number, GetDragSpeed(field), &min, &max, format,
					                       ImGuiSliderFlags_AlwaysClamp))
					{
						return false;
					}
					change.Value = number;
					return true;
				}
				float number = ReadFloat(value);
				if (!ImGui::DragFloat("##value", &number, GetDragSpeed(field), ToFloatBound(min), ToFloatBound(max), format,
				                      ImGuiSliderFlags_AlwaysClamp))
				{
					return false;
				}
				change.Value = number;
				return true;
			}
			case FieldKind::String:
			{
				std::string text = value.is_string() ? value.get<std::string>() : std::string();
				bool const changed = field.Hints.Display == FieldDisplay::MultilineText
				                         ? ImGui::InputTextMultiline("##value", &text,
				                                                     ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f +
				                                                                          ImGui::GetStyle().FramePadding.y * 2.0f))
				                         : ImGui::InputText("##value", &text);
				if (changed)
				{
					change.Value = std::move(text);
				}
				return changed;
			}
			case FieldKind::UUID:
				return DrawEntityReference(value, m_Scene, change, true);
			case FieldKind::Asset:
				return DrawAsset(field, value, mixed, change);
			case FieldKind::Vec2:
			case FieldKind::Vec3:
			case FieldKind::Vec4:
			{
				int const size = GetVectorSize(field.Kind);
				std::array<float, 4> values = ReadFloats(value);
				auto const [min, max] = GetBounds(field);
				if (IsColor(field))
				{
					return EditColor(field, value, change,
					                 [](float* display, int components)
					                 {
										 return components == 4 ? ImGui::ColorEdit4("##value", display, GetColorFlags(components))
						                                        : ImGui::ColorEdit3("##value", display, GetColorFlags(components));
									 });
				}

				std::array<float, 4> const defaults = ReadFloats(field.Default);
				int changedComponent = -1;
				if (!DrawVectorComponents(values.data(), size, GetDragSpeed(field), ToFloatBound(min), ToFloatBound(max), "%.3f",
				                          defaults.data(), changedComponent))
				{
					return false;
				}
				change.Value = WriteFloats(values.data(), size);
				change.Component = static_cast<uint32_t>(changedComponent);
				return true;
			}
			case FieldKind::Quat:
				return DrawQuaternion(value, change, FieldInlineMode::Normal);
			case FieldKind::BVec3:
			{
				std::array<bool, 3> flags{};
				for (size_t i = 0; i < flags.size(); i++)
				{
					flags[i] = value.is_array() && value.size() == 3 && value[i].is_boolean() && value[i].get<bool>();
				}
				bool changed = false;
				for (size_t i = 0; i < flags.size(); i++)
				{
					if (i > 0)
					{
						ImGui::SameLine();
					}
					if (ImGui::Checkbox(AxisLabels[i], &flags[i]))
					{
						changed = true;
						change.Component = static_cast<uint32_t>(i);
					}
				}
				if (changed)
				{
					change.Value = Json::array({flags[0], flags[1], flags[2]});
				}
				return changed;
			}
			case FieldKind::Enum:
			{
				if (field.IsFlags)
				{
					return DrawFlags(field, value, mixed, change);
				}
				std::string const current = value.is_string() ? value.get<std::string>() : std::string();
				std::string const preview = mixed ? std::string("--") : UI::FormatDisplayName(current);
				bool changed = false;
				// A filtered list is a filter above a list of its own height: the combo's popup takes both whole.
				ImGuiComboFlags const flags =
					field.EnumValues.size() > MaxUnfilteredEnumerators ? ImGuiComboFlags_HeightLargest : ImGuiComboFlags_None;
				if (ImGui::BeginCombo("##value", preview.c_str(), flags))
				{
					// A filtered list's rows are in a child window, which does not widen the popup to its longest name as rows
					// directly in it do: it is given that width, and the combo's at least.
					float const width = field.EnumValues.size() > MaxUnfilteredEnumerators
					                        ? std::max(GetEnumListWidth(field), ImGui::GetContentRegionAvail().x)
					                        : 0.0f;
					changed = DrawEnumList(field, current, width, change);
					ImGui::EndCombo();
				}
				return changed;
			}
			case FieldKind::Array:
				return DrawArray(field, value, change);
			case FieldKind::Struct:
			case FieldKind::Custom:
				break;
		}

		// Values with their own editors (script fields) or nested structs inside arrays.
		std::string summary = "Edited elsewhere";
		if (value.is_object())
		{
			summary = value.empty() ? "Empty" : fmt::format("{} entries", value.size());
		}
		else if (value.is_array())
		{
			summary = fmt::format("{} items", value.size());
		}
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", summary.c_str());
		return false;
	}

	bool FieldEditor::DrawQuaternion(Json const& value, FieldChange& change, FieldInlineMode mode)
	{
		std::array<float, 4> const components = ReadFloats(value);
		glm::quat const rotation = Math::NormalizeRotation(glm::quat::wxyz(components[3], components[0], components[1], components[2]));

		uint32_t const id = ImGui::GetID("##euler");
		auto [iterator, inserted] = m_EulerStates.try_emplace(id);
		EulerState& state = iterator->second;
		// q and -q are the same rotation.
		if (inserted || std::abs(glm::dot(state.Rotation, rotation)) < 1.0f - 1e-6f)
		{
			state.Rotation = rotation;
			// Adding zero turns -0 into +0, which would otherwise show as "-0.0".
			state.Degrees = Math::QuaternionToEulerDegrees(rotation) + glm::vec3(0.0f);
		}
		state.LastFrame = ImGui::GetFrameCount();

		glm::vec3 degrees = state.Degrees;
		std::array<float, 3> const defaults{};
		int changedComponent = -1;
		ImGui::PushID("##euler");
		bool const changed =
			mode == FieldInlineMode::Compact
				? DrawComponentCells(&degrees.x, 3, 0.5f, -FLT_MAX, FLT_MAX, CompactFloatFormat, changedComponent)
				: DrawVectorComponents(&degrees.x, 3, 0.5f, -FLT_MAX, FLT_MAX, AngleFormat, defaults.data(), changedComponent);
		ImGui::PopID();
		if (!changed)
		{
			return false;
		}

		glm::quat const updated = Math::EulerDegreesToQuaternion(degrees);
		state.Rotation = updated;
		state.Degrees = degrees;
		change.Value = Json::array({updated.x, updated.y, updated.z, updated.w});
		change.Component = static_cast<uint32_t>(changedComponent);
		change.EulerDegrees = degrees;
		return true;
	}

	bool FieldEditor::DrawArray(FieldDescriptor const& field, Json const& value, FieldChange& change)
	{
		Json elements = value.is_array() ? value : Json::array();
		FieldDescriptor const element = MakeElementDescriptor(field);
		bool changed = false;
		float const buttonWidth = ImGui::GetFrameHeight();
		float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
		for (size_t i = 0; i < elements.size(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
			ImGui::PushItemWidth(-(buttonWidth + spacing));
			FieldChange elementChange;
			if (DrawValue(element, elements[i], false, elementChange))
			{
				elements[i] = std::move(elementChange.Value);
				changed = true;
			}
			ImGui::PopItemWidth();
			ImGui::SameLine(0.0f, spacing);
			bool const removed = ImGui::Button("-", ImVec2(buttonWidth, 0.0f));
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Remove element %zu", i);
			}
			ImGui::PopID();
			if (removed)
			{
				elements.erase(i);
				changed = true;
				break;
			}
		}
		if (ImGui::Button(elements.empty() ? "Add Element" : "+", ImVec2(elements.empty() ? ImGui::CalcItemWidth() : buttonWidth, 0.0f)))
		{
			elements.push_back(element.Default);
			changed = true;
		}
		if (changed)
		{
			change.Value = std::move(elements);
		}
		return changed;
	}

	bool FieldEditor::DrawAsset(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change)
	{
		AssetHandle const handle(ReadID(value));
		AssetReferenceOptions options;
		options.Type = AssetTypeFromString(field.Hints.AssetTypeName).value_or(AssetType::None);
		options.Mixed = mixed;
		options.CanClear = handle.IsValid() || mixed;
		std::optional<AssetHandle> const chosen = DrawAssetReference(handle, options);
		if (!chosen)
		{
			return false;
		}
		change.Value = chosen->ToString();
		return true;
	}

	std::optional<AssetHandle> FieldEditor::DrawAssetReference(AssetHandle current, AssetReferenceOptions const& options)
	{
		// [image and path of the asset] [...] [x]: the box and "..." open the picker, "x" clears the reference.
		ImGuiStyle const& style = ImGui::GetStyle();
		float const spacing = style.ItemInnerSpacing.x;
		float const buttonWidth = ImGui::GetFrameHeight();
		float const width = ImGui::CalcItemWidth();
		ImVec2 const start = ImGui::GetCursorScreenPos();
		std::optional<AssetHandle> chosen;
		bool openPicker = DrawAssetReferenceBox(current, options.Mixed, width - 2.0f * (buttonWidth + spacing), m_AssetPreviews);
		if (ImGui::BeginDragDropTarget())
		{
			std::span<AssetType const> const types =
				options.Type == AssetType::None ? std::span<AssetType const>() : std::span<AssetType const>(&options.Type, 1);
			if (AssetHandle const dropped = UI::AcceptAssetDrop(types); dropped.IsValid())
			{
				chosen = dropped;
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::SameLine(0.0f, spacing);
		openPicker = ImGui::Button("...", ImVec2(buttonWidth, 0.0f)) || openPicker;
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Choose an asset");
		}
		ImGui::SameLine(0.0f, spacing);
		ImGui::BeginDisabled(!options.CanClear);
		if (ImGui::Button("x", ImVec2(buttonWidth, 0.0f)))
		{
			chosen = AssetHandle();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("%.*s", static_cast<int>(options.ClearTooltip.size()), options.ClearTooltip.data());
		}

		ImGuiID const picker = ImGui::GetID("##assetPicker");
		if (openPicker)
		{
			m_AssetFilter.clear();
			ImGui::OpenPopupEx(picker);
		}
		if (ImGui::IsPopupOpen(picker, ImGuiPopupFlags_None))
		{
			// Drops down from the field like a combo box, as wide as the longest path needs within the screen; rows that still
			// do not fit are shortened.
			float const popupWidth = GetPickerWidth(options.Type, width);
			ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0.0f), ImVec2(popupWidth, FLT_MAX));
		}
		ImRect const bounds(start, ImVec2(start.x + width, start.y + ImGui::GetFrameHeight()));
		if (ImGui::BeginComboPopup(picker, bounds, ImGuiComboFlags_None))
		{
			if (std::optional<AssetHandle> const picked = DrawAssetPicker(options.Type, current, options.AllowNone))
			{
				chosen = picked;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndCombo();
		}
		if (chosen && *chosen == current && !options.Mixed)
		{
			return std::nullopt;
		}
		return chosen;
	}

	float FieldEditor::GetPickerWidth(AssetType type, float minWidth)
	{
		UpdatePickerAssets(type);
		ImGuiStyle const& style = ImGui::GetStyle();
		float const rowWidth = ImGui::GetTextLineHeight() + style.ItemInnerSpacing.x + m_PickerLabelWidth;
		float const needed = rowWidth + style.FramePadding.x * 2.0f + style.WindowPadding.x * 2.0f + style.ScrollbarSize;
		float const screen = ImGui::GetWindowViewport()->WorkSize.x - style.WindowPadding.x * 2.0f;
		return std::max(1.0f, std::min(std::max({minWidth, ImGui::GetFontSize() * MinPickerWidthInFontSizes, needed}), screen));
	}

	std::optional<AssetHandle> FieldEditor::DrawAssetPickerPopup(char const* popupId, AssetType type, AssetHandle current, bool allowNone)
	{
		if (!ImGui::IsPopupOpen(ImGui::GetID(popupId), ImGuiPopupFlags_None))
		{
			return std::nullopt;
		}
		float const width = GetPickerWidth(type, 0.0f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
		std::optional<AssetHandle> chosen;
		if (ImGui::BeginPopup(popupId))
		{
			if (ImGui::IsWindowAppearing())
			{
				m_AssetFilter.clear();
			}
			chosen = DrawAssetPicker(type, current, allowNone);
			if (chosen)
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		if (chosen && *chosen == current)
		{
			return std::nullopt;
		}
		return chosen;
	}

	bool FieldEditor::DrawInlineValue(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change,
	                                  FieldInlineMode mode, bool* popupRequested)
	{
		PruneEulerStates();
		if (mode == FieldInlineMode::Normal)
		{
			return DrawValue(field, value, mixed, change);
		}
		bool requested = false;
		bool const changed = DrawCompactValue(field, value, mixed, change, requested);
		if (requested && popupRequested != nullptr)
		{
			*popupRequested = true;
		}
		return changed;
	}

	bool FieldEditor::DrawCompactValue(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change,
	                                   bool& popupRequested)
	{
		switch (field.Kind)
		{
			case FieldKind::Float:
			case FieldKind::Double:
			{
				auto const [min, max] = GetBounds(field);
				double number = value.is_number() ? value.get<double>() : 0.0;
				if (field.Kind == FieldKind::Float)
				{
					float single = static_cast<float>(number);
					if (!ImGui::DragFloat("##value", &single, GetDragSpeed(field), ToFloatBound(min), ToFloatBound(max), CompactFloatFormat,
					                      ImGuiSliderFlags_AlwaysClamp))
					{
						return false;
					}
					change.Value = single;
					return true;
				}
				if (!ImGui::DragScalar("##value", ImGuiDataType_Double, &number, GetDragSpeed(field), &min, &max, CompactFloatFormat,
				                       ImGuiSliderFlags_AlwaysClamp))
				{
					return false;
				}
				change.Value = number;
				return true;
			}
			case FieldKind::String:
			{
				// One line, as the canvas has room for.
				std::string text = value.is_string() ? value.get<std::string>() : std::string();
				if (!ImGui::InputText("##value", &text))
				{
					return false;
				}
				change.Value = std::move(text);
				return true;
			}
			case FieldKind::UUID:
				return DrawEntityReference(value, m_Scene, change, false);
			case FieldKind::Asset:
			{
				AssetHandle const handle(ReadID(value));
				popupRequested = DrawAssetReferenceBox(handle, mixed, ImGui::CalcItemWidth(), m_AssetPreviews, true) || popupRequested;
				if (ImGui::BeginDragDropTarget())
				{
					std::optional<AssetType> const type = AssetTypeFromString(field.Hints.AssetTypeName);
					std::span<AssetType const> const types =
						type && *type != AssetType::None ? std::span<AssetType const>(&*type, 1) : std::span<AssetType const>();
					AssetHandle const dropped = UI::AcceptAssetDrop(types);
					ImGui::EndDragDropTarget();
					if (dropped.IsValid() && (dropped != handle || mixed))
					{
						change.Value = dropped.ToString();
						return true;
					}
				}
				return false;
			}
			case FieldKind::Vec2:
			case FieldKind::Vec3:
			case FieldKind::Vec4:
			{
				int const size = GetVectorSize(field.Kind);
				if (IsColor(field))
				{
					std::array<float, 4> const display = ToDisplayColor(value);
					// No tooltip: it would take the canvas's zoomed font and spacing (spec §7.8 rule 3).
					ImGuiColorEditFlags const flags = ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoTooltip |
					                                  (size == 4 ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha);
					ImVec4 const color(display[0], display[1], display[2], size == 4 ? display[3] : 1.0f);
					popupRequested = ImGui::ColorButton("##value", color, flags, ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight())) ||
					                 popupRequested;
					return false;
				}
				std::array<float, 4> values = ReadFloats(value);
				auto const [min, max] = GetBounds(field);
				int changedComponent = -1;
				if (!DrawComponentCells(values.data(), size, GetDragSpeed(field), ToFloatBound(min), ToFloatBound(max), CompactFloatFormat,
				                        changedComponent))
				{
					return false;
				}
				change.Value = WriteFloats(values.data(), size);
				change.Component = static_cast<uint32_t>(changedComponent);
				return true;
			}
			case FieldKind::Quat:
				return DrawQuaternion(value, change, FieldInlineMode::Compact);
			case FieldKind::Enum:
			{
				popupRequested = DrawChoiceFrame(mixed ? std::string("--") : DescribeCompactText(field, value)) || popupRequested;
				return false;
			}
			case FieldKind::Array:
			{
				// Lists are edited in Details, whose elements may open popups of their own.
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("%zu items", value.is_array() ? value.size() : size_t(0));
				return false;
			}
			case FieldKind::Bool:
			case FieldKind::Int:
			case FieldKind::UInt:
			case FieldKind::BVec3:
			case FieldKind::Struct:
			case FieldKind::Custom:
				break;
		}
		// Check boxes, integers ("%lld") and what has no widget open no popup in their normal form either.
		return DrawValue(field, value, mixed, change);
	}

	std::string FieldEditor::DescribeCompactText(FieldDescriptor const& field, Json const& value)
	{
		if (field.Kind == FieldKind::Enum)
		{
			// The stored names, as the canvas chip shows them.
			return field.IsFlags ? DescribeCompactFlags(field, value, false)
			                     : std::string(value.is_string() ? value.get_ref<std::string const&>() : "");
		}
		if (field.Kind == FieldKind::Asset)
		{
			return DescribeCompactAsset(AssetHandle(ReadID(value)));
		}
		return {};
	}

	bool FieldEditor::DrawValuePopup(FieldDescriptor const& field, Json const& value, FieldChange& change)
	{
		PruneEulerStates();
		ImGuiStyle const& style = ImGui::GetStyle();
		if (field.Kind == FieldKind::Enum)
		{
			if (field.IsFlags)
			{
				return DrawFlagList(field, value, change);
			}
			return DrawEnumList(field, value.is_string() ? value.get<std::string>() : std::string(), GetEnumListWidth(field), change);
		}
		if (IsColor(field))
		{
			return EditColor(field, value, change,
			                 [](float* display, int components)
			                 {
								 return components == 4 ? ImGui::ColorPicker4("##picker", display, GetColorFlags(components))
				                                        : ImGui::ColorPicker3("##picker", display, GetColorFlags(components));
							 });
		}
		if (field.Kind == FieldKind::Asset)
		{
			AssetType const type = AssetTypeFromString(field.Hints.AssetTypeName).value_or(AssetType::None);
			AssetHandle const handle(ReadID(value));
			if (ImGui::IsWindowAppearing())
			{
				m_AssetFilter.clear();
			}
			float const width = GetPickerWidth(type, 0.0f) - style.WindowPadding.x * 2.0f;
			std::optional<AssetHandle> const picked = DrawAssetPicker(type, handle, true, width);
			if (!picked)
			{
				return false;
			}
			ImGui::CloseCurrentPopup();
			if (*picked == handle)
			{
				return false;
			}
			change.Value = picked->ToString();
			return true;
		}
		// The others open no popup of their own: their normal widget, at a width of its own in a popup that fits its content.
		ImGui::PushItemWidth(ImGui::GetFontSize() * MinEnumListWidthInFontSizes);
		bool const changed = DrawValue(field, value, false, change);
		ImGui::PopItemWidth();
		return changed;
	}

	int FieldEditor::OnEnumFilterKey(ImGuiInputTextCallbackData* data)
	{
		FieldEditor* const editor = static_cast<FieldEditor*>(data->UserData);
		if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
		{
			editor->m_EnumMove += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
		}
		else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
		{
			PlaceCursorAtEnd(data, editor->m_PlaceFilterCursor);
		}
		return 0;
	}

	bool FieldEditor::DrawEnumList(FieldDescriptor const& field, std::string const& current, float width, FieldChange& change)
	{
		std::optional<std::string_view> picked;
		bool const appearing = ImGui::IsWindowAppearing();
		if (field.EnumValues.size() <= MaxUnfilteredEnumerators)
		{
			for (std::string_view const name : field.EnumValues)
			{
				bool const selected = name == current;
				if (ImGui::Selectable(UI::FormatDisplayName(name).c_str(), selected, ImGuiSelectableFlags_None, ImVec2(width, 0.0f)))
				{
					picked = name;
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
		}
		else
		{
			// ImGui's typing-select is internal and combos do not use it: the filter is the editor's own.
			// The filter is asked for the keyboard until it has it (a popup's first frame is laid out hidden, and may not
			// take it), and never while it has it.
			if (appearing)
			{
				m_EnumFilter.clear();
				m_FocusEnumFilter = true;
			}
			bool typed = false;
			if (m_FocusEnumFilter && ImGui::GetActiveID() != ImGui::GetID("##filter"))
			{
				typed = TakeTypedCharacters(m_EnumFilter);
				m_PlaceFilterCursor = true;
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::SetNextItemWidth(width > 0.0f ? width : -FLT_MIN);
			m_EnumMove = 0;
			bool confirmed = ImGui::InputTextWithHint("##filter", "Search", &m_EnumFilter,
			                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
			                                              ImGuiInputTextFlags_CallbackAlways,
			                                          &FieldEditor::OnEnumFilterKey, this);
			bool const edited = ImGui::IsItemEdited() || typed;
			if (ImGui::IsItemActive())
			{
				m_FocusEnumFilter = false;
				confirmed = confirmed || IsEnterPressed();
			}
			ImGui::Separator();

			// Exact names first, then those starting with the filter, then those holding it, each in declaration order.
			std::string const filter = Trim(m_EnumFilter);
			std::vector<std::pair<int, size_t>> matches;
			matches.reserve(field.EnumValues.size());
			for (size_t i = 0; i < field.EnumValues.size(); i++)
			{
				if (filter.empty())
				{
					matches.emplace_back(0, i);
				}
				else if (std::optional<int> const rank =
				             RankEnumerator(std::string(field.EnumValues[i]), UI::FormatDisplayName(field.EnumValues[i]), filter))
				{
					matches.emplace_back(*rank, i);
				}
			}
			std::stable_sort(matches.begin(), matches.end(),
			                 [](auto const& a, auto const& b)
			                 {
								 return a.first < b.first;
							 });

			int const rowCount = static_cast<int>(matches.size());
			if (appearing)
			{
				auto const currentMatch = std::find_if(matches.begin(), matches.end(),
				                                       [&field, &current](auto const& match)
				                                       {
														   return field.EnumValues[match.second] == current;
													   });
				m_EnumHighlight = currentMatch != matches.end() ? static_cast<int>(currentMatch - matches.begin()) : 0;
			}
			else if (edited)
			{
				m_EnumHighlight = 0;
			}
			m_EnumHighlight = std::clamp(m_EnumHighlight + m_EnumMove, 0, std::max(rowCount - 1, 0));
			bool const scroll = (appearing || edited || m_EnumMove != 0) && rowCount > 0;

			// Enter takes the keyboard's row; without a row it ends the text input, so the filter gets the keyboard back.
			if (confirmed && rowCount > 0)
			{
				picked = field.EnumValues[matches[static_cast<size_t>(m_EnumHighlight)].second];
			}
			else if (confirmed)
			{
				m_FocusEnumFilter = true;
			}

			float const rowHeight = ImGui::GetTextLineHeightWithSpacing();
			size_t const shownRows = std::clamp<size_t>(matches.size(), 1, MaxPickerRows);
			ImGui::BeginChild("##enumerators", ImVec2(width, rowHeight * static_cast<float>(shownRows)), ImGuiChildFlags_None,
			                  ImGuiWindowFlags_NoNavInputs);
			ImGuiListClipper clipper;
			clipper.Begin(rowCount, rowHeight);
			if (scroll)
			{
				clipper.IncludeItemByIndex(m_EnumHighlight);
			}
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				{
					std::string_view const name = field.EnumValues[matches[static_cast<size_t>(row)].second];
					bool const highlighted = row == m_EnumHighlight;
					ImGui::PushID(row);
					if (ImGui::Selectable(UI::FormatDisplayName(name).c_str(), highlighted))
					{
						picked = name;
					}
					ImGui::PopID();
					if (highlighted && scroll)
					{
						if (appearing)
						{
							ImGui::SetScrollHereY();
						}
						else
						{
							ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
						}
					}
				}
			}
			if (rowCount == 0)
			{
				ImGui::TextDisabled("No values match");
			}
			ImGui::EndChild();
		}
		if (!picked)
		{
			return false;
		}
		change.Value = std::string(*picked);
		// The rows of a filtered list are in a child window, whose selectables do not close the popup they are in.
		ImGui::CloseCurrentPopup();
		return true;
	}

	void FieldEditor::UpdatePickerAssets(AssetType type)
	{
		// Listed again when assets change; labels are measured again when the font changes (display scale).
		uint64_t const version = AssetManager::IsInitialized() ? AssetManager::GetVersion() : 0;
		float const fontSize = ImGui::GetFontSize();
		if (m_PickerVersion && version == *m_PickerVersion && type == m_PickerType && fontSize == m_PickerFontSize)
		{
			return;
		}
		m_PickerAssets.clear();
		m_PickerLabelWidth = ImGui::CalcTextSize("None").x;
		if (AssetManager::IsInitialized())
		{
			for (AssetMetadata const& asset : AssetManager::GetAssets(type))
			{
				std::string label = UI::DescribeAssetReference(asset.Handle);
				m_PickerLabelWidth = std::max(m_PickerLabelWidth, ImGui::CalcTextSize(label.c_str()).x);
				m_PickerAssets.emplace_back(asset.Handle, std::move(label));
			}
		}
		m_PickerVersion = version;
		m_PickerType = type;
		m_PickerFontSize = fontSize;
	}

	int FieldEditor::OnPickerSearchKey(ImGuiInputTextCallbackData* data)
	{
		FieldEditor* const editor = static_cast<FieldEditor*>(data->UserData);
		if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
		{
			editor->m_PickerMove += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
		}
		else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
		{
			PlaceCursorAtEnd(data, editor->m_PlaceFilterCursor);
		}
		return 0;
	}

	std::optional<AssetHandle> FieldEditor::DrawAssetPicker(AssetType type, AssetHandle current, bool allowNone, float width)
	{
		// The search field is asked for the keyboard until it has it (a popup's first frame is laid out hidden, and may not
		// take it), and never while it has it.
		bool const appearing = ImGui::IsWindowAppearing();
		if (appearing)
		{
			m_FocusAssetSearch = true;
		}
		if (m_FocusAssetSearch && ImGui::GetActiveID() != ImGui::GetID("##search"))
		{
			TakeTypedCharacters(m_AssetFilter);
			m_PlaceFilterCursor = true;
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(width > 0.0f ? width : -FLT_MIN);
		m_PickerMove = 0;
		bool confirmed = ImGui::InputTextWithHint("##search", "Search", &m_AssetFilter,
		                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
		                                              ImGuiInputTextFlags_CallbackAlways,
		                                          &FieldEditor::OnPickerSearchKey, this);
		if (ImGui::IsItemActive())
		{
			m_FocusAssetSearch = false;
			confirmed = confirmed || IsEnterPressed();
		}
		ImGui::Separator();
		UpdatePickerAssets(type);

		// None (when allowed, without search text), then the assets whose paths contain the search text (ignoring case and
		// surrounding spaces); the list shrinks to its rows.
		size_t const first = m_AssetFilter.find_first_not_of(' ');
		std::string const search =
			first == std::string::npos ? std::string() : m_AssetFilter.substr(first, m_AssetFilter.find_last_not_of(' ') - first + 1);
		bool const searching = !search.empty();
		std::vector<size_t> matches;
		matches.reserve(m_PickerAssets.size());
		for (size_t i = 0; i < m_PickerAssets.size(); i++)
		{
			if (!searching || ImStristr(m_PickerAssets[i].second.c_str(), nullptr, search.c_str(), nullptr) != nullptr)
			{
				matches.push_back(i);
			}
		}
		int const noneRows = allowNone && !searching ? 1 : 0;
		int const rowCount = noneRows + static_cast<int>(matches.size());
		auto const getAsset = [&](int row)
		{
			return row < noneRows ? AssetHandle() : m_PickerAssets[matches[static_cast<size_t>(row - noneRows)]].first;
		};

		// The keyboard is on the current asset when the picker opens and on the first match when the search changes; Up and
		// Down move it, and the list follows.
		if (appearing)
		{
			auto const currentMatch = std::find_if(matches.begin(), matches.end(),
			                                       [this, current](size_t index)
			                                       {
													   return m_PickerAssets[index].first == current;
												   });
			m_PickerHighlight = currentMatch != matches.end() ? noneRows + static_cast<int>(currentMatch - matches.begin()) : 0;
			m_PickerHighlightFilter = m_AssetFilter;
		}
		else if (m_AssetFilter != m_PickerHighlightFilter)
		{
			m_PickerHighlight = 0;
			m_PickerHighlightFilter = m_AssetFilter;
			m_ScrollToPickerHighlight = true;
		}
		if (m_PickerMove != 0)
		{
			m_PickerHighlight += m_PickerMove;
			m_ScrollToPickerHighlight = true;
		}
		m_PickerHighlight = std::clamp(m_PickerHighlight, 0, std::max(rowCount - 1, 0));

		std::optional<AssetHandle> picked;
		// Enter takes the keyboard's row; without a row it ends the text input, so the search field gets the keyboard back.
		if (confirmed && rowCount > 0)
		{
			picked = getAsset(m_PickerHighlight);
		}
		else if (confirmed)
		{
			m_FocusAssetSearch = true;
		}

		// Only the rows in view are drawn, so long lists stay quick and only the images that show are made; the keyboard's row
		// is drawn when the list scrolls to it (centered when the picker opens).
		float const rowHeight = ImGui::GetTextLineHeightWithSpacing();
		size_t const shownRows = std::clamp<size_t>(static_cast<size_t>(rowCount), 1, MaxPickerRows);
		ImGui::BeginChild("##assets", ImVec2(width, rowHeight * static_cast<float>(shownRows)), ImGuiChildFlags_None,
		                  ImGuiWindowFlags_NoNavInputs);
		bool const scroll = (appearing || m_ScrollToPickerHighlight) && rowCount > 0;
		m_ScrollToPickerHighlight = false;
		ImGuiListClipper clipper;
		clipper.Begin(rowCount, rowHeight);
		if (scroll)
		{
			clipper.IncludeItemByIndex(m_PickerHighlight);
		}
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				bool const highlighted = row == m_PickerHighlight;
				if (row < noneRows)
				{
					if (ImGui::Selectable("None", highlighted))
					{
						picked = AssetHandle();
					}
				}
				else
				{
					AssetHandle const asset = getAsset(row);
					ImGui::PushID(reinterpret_cast<void const*>(static_cast<uintptr_t>(asset.GetUUID().GetValue())));
					if (DrawAssetPickerRow(m_AssetPreviews, asset, m_PickerAssets[matches[static_cast<size_t>(row - noneRows)]].second,
					                       highlighted))
					{
						picked = asset;
					}
					ImGui::PopID();
				}
				if (highlighted && scroll)
				{
					if (appearing)
					{
						ImGui::SetScrollHereY();
					}
					else
					{
						ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
					}
				}
			}
		}
		if (searching && matches.empty())
		{
			ImGui::TextDisabled("No assets match");
		}
		ImGui::EndChild();
		return picked;
	}

	void FieldEditor::PruneEulerStates()
	{
		int const frame = ImGui::GetFrameCount();
		if (frame == m_LastPruneFrame)
		{
			return;
		}
		m_LastPruneFrame = frame;
		std::erase_if(m_EulerStates,
		              [frame](auto const& entry)
		              {
						  return entry.second.LastFrame < frame - 1;
					  });
	}
}
