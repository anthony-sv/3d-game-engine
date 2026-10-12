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
		// The image of an asset in the tooltip of its field fits a square this large (in font sizes).
		constexpr float TooltipPreviewSizeInFontSizes = 9.0f;
		// The share of a table's width the name column starts with: what the names need, but never so much that the values
		// get less than 40%, and tables of short names line up at the same share.
		constexpr float MinNameShare = 0.4f;
		constexpr float MaxNameShare = 0.6f;
		constexpr char const* AngleFormat = "%.1f\xc2\xb0";
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

		// The range numeric widgets clamp to: the hinted range within what the C++ type represents.
		std::pair<double, double> GetBounds(FieldDescriptor const& field)
		{
			return {std::max(field.Hints.Min, field.TypeMin), std::min(field.Hints.Max, field.TypeMax)};
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
				if (IsSwitch(fields, field))
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

		// A row of the asset picker: the asset's image (when `previews` makes one) and its path; returns true when clicked.
		bool DrawAssetPickerRow(AssetThumbnails* previews, AssetHandle asset, std::string const& label, bool selected)
		{
			ImVec2 const position = ImGui::GetCursorScreenPos();
			bool const clicked = ImGui::Selectable("##asset", selected);
			float const imageSize = ImGui::GetTextLineHeight();
			float const textX = DrawAssetImage(previews, asset, position, imageSize)
			                        ? position.x + imageSize + ImGui::GetStyle().ItemInnerSpacing.x
			                        : position.x;
			ImGui::GetWindowDrawList()->AddText(ImVec2(textX, position.y), GetAssetReferenceColor(asset, false), label.c_str());
			return clicked;
		}

		// A box framed like a text field showing the referenced asset (long paths shortened), after a small image of it when
		// `previews` makes one (the place stays while the image is made); returns true when clicked. The tooltip has the whole
		// path and a larger image.
		bool DrawAssetReferenceBox(AssetHandle handle, bool mixed, float width, AssetThumbnails* previews)
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
			std::string const text =
				UI::FitPath(mixed ? std::string("--") : UI::DescribeAssetReference(handle), max.x - style.FramePadding.x - textX);
			drawList->PushClipRect(min, max, true);
			drawList->AddText(ImVec2(textX, min.y + style.FramePadding.y), GetAssetReferenceColor(handle, mixed), text.c_str());
			drawList->PopClipRect();

			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
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

		// Flags enums: a combo of check boxes; the value is the array of the set names, in declaration order.
		bool DrawFlags(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change)
		{
			auto const isSet = [&value](std::string_view name)
			{
				return value.is_array() && std::any_of(value.begin(), value.end(),
				                                       [name](Json const& element)
				                                       {
														   return element.is_string() && element.get_ref<std::string const&>() == name;
													   });
			};
			std::string preview;
			for (std::string_view const name : field.EnumValues)
			{
				if (isSet(name))
				{
					preview += (preview.empty() ? "" : ", ") + UI::FormatDisplayName(name);
				}
			}
			if (mixed)
			{
				preview = "--";
			}
			else if (preview.empty())
			{
				preview = "None";
			}

			bool changed = false;
			if (ImGui::BeginCombo("##value", preview.c_str()))
			{
				Json names = Json::array();
				for (std::string_view const name : field.EnumValues)
				{
					bool set = isSet(name);
					if (ImGui::Checkbox(UI::FormatDisplayName(name).c_str(), &set))
					{
						changed = true;
					}
					if (set)
					{
						names.push_back(std::string(name));
					}
				}
				ImGui::EndCombo();
				if (changed)
				{
					change.Value = std::move(names);
				}
			}
			return changed;
		}

		// Entity reference: the entity's name, an entity drag-and-drop target and a context menu to clear it.
		bool DrawEntityReference(Json const& value, Scene* scene, FieldChange& change)
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
			if (ImGui::BeginPopupContextItem("##entityMenu"))
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
		return std::clamp((MeasureNames(fields, 0) + 1.0f) / contentWidth, MinNameShare, MaxNameShare);
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
			// hidden.
			if (IsSwitch(fields, field) ||
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
				int64_t const lower = static_cast<int64_t>(std::max(min, static_cast<double>(-IntegerWidgetLimit)));
				int64_t const upper = static_cast<int64_t>(std::min(max, static_cast<double>(IntegerWidgetLimit)));
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
				return DrawEntityReference(value, m_Scene, change);
			case FieldKind::Asset:
				return DrawAsset(field, value, mixed, change);
			case FieldKind::Vec2:
			case FieldKind::Vec3:
			case FieldKind::Vec4:
			{
				int const size = GetVectorSize(field.Kind);
				std::array<float, 4> values = ReadFloats(value);
				auto const [min, max] = GetBounds(field);
				if (field.Hints.Display == FieldDisplay::Color && size >= 3)
				{
					// Colors are linear in the data and picked in sRGB, so the swatch shows what the renderer displays.
					std::array<float, 4> display = values;
					for (size_t i = 0; i < 3; i++)
					{
						display[i] = Math::LinearToSrgb(std::clamp(values[i], 0.0f, 1.0f));
					}
					ImGuiColorEditFlags flags = ImGuiColorEditFlags_None;
					if (size == 4)
					{
						flags |= ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
					}
					bool const changed = size == 4 ? ImGui::ColorEdit4("##value", display.data(), flags)
					                               : ImGui::ColorEdit3("##value", display.data(), flags);
					if (!changed)
					{
						return false;
					}
					for (size_t i = 0; i < static_cast<size_t>(size); i++)
					{
						float const linear = i < 3 ? Math::SrgbToLinear(display[i]) : display[i];
						values[i] = std::clamp(linear, ToFloatBound(min), ToFloatBound(max));
					}
					change.Value = WriteFloats(values.data(), size);
					return true;
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
				return DrawQuaternion(value, change);
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
				if (ImGui::BeginCombo("##value", preview.c_str()))
				{
					for (std::string_view const name : field.EnumValues)
					{
						bool const selected = name == current;
						if (ImGui::Selectable(UI::FormatDisplayName(name).c_str(), selected))
						{
							change.Value = std::string(name);
							changed = true;
						}
						if (selected)
						{
							ImGui::SetItemDefaultFocus();
						}
					}
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

	bool FieldEditor::DrawQuaternion(Json const& value, FieldChange& change)
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
		bool const changed = DrawVectorComponents(&degrees.x, 3, 0.5f, -FLT_MAX, FLT_MAX, AngleFormat, defaults.data(), changedComponent);
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
		AssetType const expected = AssetTypeFromString(field.Hints.AssetTypeName).value_or(AssetType::None);
		bool changed = false;

		// [path of the asset] [...] [x]: the box and "..." open the picker, "x" clears the reference.
		float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
		float const buttonWidth = ImGui::GetFrameHeight();
		ImVec2 const start = ImGui::GetCursorScreenPos();
		bool openPicker = DrawAssetReferenceBox(handle, mixed, ImGui::CalcItemWidth() - 2.0f * (buttonWidth + spacing), m_AssetPreviews);
		if (ImGui::BeginDragDropTarget())
		{
			std::span<AssetType const> const types =
				expected == AssetType::None ? std::span<AssetType const>() : std::span<AssetType const>(&expected, 1);
			if (AssetHandle const dropped = UI::AcceptAssetDrop(types); dropped.IsValid())
			{
				change.Value = dropped.ToString();
				changed = true;
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
		ImGui::BeginDisabled(!handle.IsValid() && !mixed);
		if (ImGui::Button("x", ImVec2(buttonWidth, 0.0f)))
		{
			change.Value = UUID::Invalid().ToString();
			changed = true;
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::SetTooltip("Clear");
		}

		ImGuiID const picker = ImGui::GetID("##assetPicker");
		if (openPicker)
		{
			m_AssetFilter.clear();
			ImGui::OpenPopupEx(picker);
		}
		// Drops down from the field like a combo box, wide enough for paths.
		ImRect const bounds(start, ImVec2(ImGui::GetItemRectMax().x, start.y + ImGui::GetFrameHeight()));
		ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(bounds.GetWidth(), ImGui::GetFontSize() * MinPickerWidthInFontSizes), 0.0f),
		                                    ImVec2(FLT_MAX, FLT_MAX));
		if (ImGui::BeginComboPopup(picker, bounds, ImGuiComboFlags_None))
		{
			std::optional<AssetHandle> picked = DrawAssetPicker(expected, handle);
			if (picked)
			{
				change.Value = picked->ToString();
				changed = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	std::optional<AssetHandle> FieldEditor::DrawAssetPicker(AssetType type, AssetHandle current)
	{
		if (ImGui::IsWindowAppearing() || m_FocusAssetSearch)
		{
			ImGui::SetKeyboardFocusHere();
			m_FocusAssetSearch = false;
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		bool const confirmed = ImGui::InputTextWithHint("##search", "Search", &m_AssetFilter, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::Separator();

		// Every registered asset of the type, as asset fields show them (listed again when assets change).
		uint64_t const version = AssetManager::IsInitialized() ? AssetManager::GetVersion() : 0;
		if (!m_PickerVersion || version != *m_PickerVersion || type != m_PickerType)
		{
			m_PickerAssets.clear();
			if (AssetManager::IsInitialized())
			{
				for (AssetMetadata const& asset : AssetManager::GetAssets(type))
				{
					m_PickerAssets.emplace_back(asset.Handle, UI::DescribeAssetReference(asset.Handle));
				}
			}
			m_PickerVersion = version;
			m_PickerType = type;
		}

		// None, then the assets whose paths contain the search text (ignoring case and surrounding spaces); the list shrinks to
		// its rows.
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
		size_t const rows = std::clamp<size_t>(matches.size() + (searching ? 0 : 1), 1, MaxPickerRows);
		std::optional<AssetHandle> picked;
		// Enter takes the first match; without one it ends the text input, so the search field gets the keyboard back.
		if (confirmed && searching && !matches.empty())
		{
			picked = m_PickerAssets[matches.front()].first;
		}
		else if (confirmed)
		{
			m_FocusAssetSearch = true;
		}
		float const rowHeight = ImGui::GetTextLineHeightWithSpacing();
		ImGui::BeginChild("##assets", ImVec2(0.0f, rowHeight * static_cast<float>(rows)), ImGuiChildFlags_None,
		                  ImGuiWindowFlags_NoNavInputs);
		// None (without search text), then the matches. Only the rows in view are drawn, so long lists stay quick and only the
		// images that show are made; the current asset's row is drawn when the picker opens, to scroll to it.
		int const noneRows = searching ? 0 : 1;
		bool const appearing = ImGui::IsWindowAppearing();
		ImGuiListClipper clipper;
		clipper.Begin(noneRows + static_cast<int>(matches.size()), rowHeight);
		if (appearing)
		{
			auto const currentMatch = std::find_if(matches.begin(), matches.end(),
			                                       [this, current](size_t index)
			                                       {
													   return m_PickerAssets[index].first == current;
												   });
			if (currentMatch != matches.end())
			{
				clipper.IncludeItemByIndex(noneRows + static_cast<int>(currentMatch - matches.begin()));
			}
		}
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				if (row < noneRows)
				{
					if (ImGui::Selectable("None", !current.IsValid()))
					{
						picked = AssetHandle();
					}
					continue;
				}
				auto const& [asset, label] = m_PickerAssets[matches[static_cast<size_t>(row - noneRows)]];
				ImGui::PushID(reinterpret_cast<void const*>(static_cast<uintptr_t>(asset.GetUUID().GetValue())));
				bool const selected = asset == current;
				if (DrawAssetPickerRow(m_AssetPreviews, asset, label, selected))
				{
					picked = asset;
				}
				if (selected && appearing)
				{
					ImGui::SetScrollHereY();
				}
				ImGui::PopID();
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
