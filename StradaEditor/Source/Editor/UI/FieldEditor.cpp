#include "Editor/UI/FieldEditor.h"

#include "Editor/UI/EditorUI.h"

#include "Strada/Asset/AssetManager.h"
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

		// Asset payloads carry the handle's 64-bit value.
		AssetHandle ReadAssetPayload(ImGuiPayload const& payload)
		{
			uint64_t value = 0;
			if (payload.DataSize == static_cast<int>(sizeof(value)))
			{
				std::memcpy(&value, payload.Data, sizeof(value));
			}
			return AssetHandle(UUID(value));
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

		void DrawLabel(std::string const& label, std::string const& tooltip, bool mixed, int depth)
		{
			ImGui::AlignTextToFramePadding();
			if (depth > 0)
			{
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(depth) * ImGui::GetStyle().IndentSpacing);
			}
			if (mixed)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, MixedLabelColor);
			}
			ImGui::TextUnformatted(label.c_str());
			if (mixed)
			{
				ImGui::PopStyleColor();
			}
			if (!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", tooltip.c_str());
			}
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

		std::string DescribeAsset(AssetHandle handle)
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
				return fmt::format("Unknown asset {}", handle);
			}
			std::string name = metadata->GetDisplayName();
			return AssetManager::IsMissing(handle) ? name + " (missing)" : name;
		}

		// AssetType::None accepts every type.
		bool IsAcceptableAsset(AssetHandle handle, AssetType expected)
		{
			return AssetManager::IsInitialized() && AssetManager::IsValid(handle) &&
			       (expected == AssetType::None || AssetManager::GetAssetType(handle) == expected);
		}

		// Asset button with a searchable picker popup, a clear button and an asset drag-and-drop target.
		bool DrawAsset(FieldDescriptor const& field, Json const& value, std::string& filter, FieldChange& change)
		{
			AssetHandle const handle(ReadID(value));
			AssetType const expected = AssetTypeFromString(field.Hints.AssetTypeName).value_or(AssetType::None);
			bool changed = false;

			float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			float const clearWidth = handle.IsValid() ? ImGui::GetFrameHeight() + spacing : 0.0f;
			std::string const label = DescribeAsset(handle);
			if (ImGui::Button(fmt::format("{}###asset", label).c_str(), ImVec2(std::max(1.0f, ImGui::CalcItemWidth() - clearWidth), 0.0f)))
			{
				filter.clear();
				ImGui::OpenPopup("##assetPicker");
			}
			if (handle.IsValid() && AssetManager::IsInitialized() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", AssetManager::GetReference(handle).c_str());
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (ImGuiPayload const* payload = ImGui::AcceptDragDropPayload(DragDropPayload::Asset, ImGuiDragDropFlags_AcceptPeekOnly))
				{
					AssetHandle const dropped = ReadAssetPayload(*payload);
					if (IsAcceptableAsset(dropped, expected) && ImGui::AcceptDragDropPayload(DragDropPayload::Asset))
					{
						change.Value = dropped.ToString();
						changed = true;
					}
				}
				ImGui::EndDragDropTarget();
			}
			if (handle.IsValid())
			{
				ImGui::SameLine(0.0f, spacing);
				if (ImGui::Button("x##clear", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
				{
					change.Value = UUID::Invalid().ToString();
					changed = true;
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
				{
					ImGui::SetTooltip("Clear");
				}
			}

			if (ImGui::BeginPopup("##assetPicker"))
			{
				if (ImGui::IsWindowAppearing())
				{
					ImGui::SetKeyboardFocusHere();
				}
				ImGui::SetNextItemWidth(260.0f);
				ImGui::InputTextWithHint("##filter", "Search", &filter);
				ImGui::Separator();
				ImGui::BeginChild("##assets", ImVec2(260.0f, 240.0f));
				if (ImGui::Selectable("None", !handle.IsValid()))
				{
					change.Value = UUID::Invalid().ToString();
					changed = true;
					ImGui::CloseCurrentPopup();
				}
				if (AssetManager::IsInitialized())
				{
					for (AssetMetadata const& asset : AssetManager::GetAssets(expected))
					{
						std::string const name = asset.GetDisplayName();
						std::string const& searchable = asset.Path.empty() ? name : asset.Path;
						if (!filter.empty() && !ImStristr(searchable.c_str(), nullptr, filter.c_str(), nullptr))
						{
							continue;
						}
						ImGui::PushID(reinterpret_cast<void const*>(static_cast<uintptr_t>(asset.Handle.GetUUID().GetValue())));
						if (ImGui::Selectable(name.c_str(), asset.Handle == handle))
						{
							change.Value = asset.Handle.ToString();
							changed = true;
							ImGui::CloseCurrentPopup();
						}
						if (!asset.Path.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
						{
							ImGui::SetTooltip("%s", asset.Path.c_str());
						}
						ImGui::PopID();
					}
				}
				ImGui::EndChild();
				ImGui::EndPopup();
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

	std::optional<FieldChange> FieldEditor::Draw(std::span<FieldDescriptor const> fields, Json const& values,
	                                             std::span<std::string const> mixedFields, Scene* scene)
	{
		PruneEulerStates();
		m_Scene = scene;
		std::optional<FieldChange> change;
		if (!ImGui::BeginTable("##fields", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings))
		{
			return change;
		}
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
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
		for (FieldDescriptor const& field : fields)
		{
			std::string const key(field.Name);
			bool const mixed = parentMixed || (depth == 0 && std::find(mixedFields.begin(), mixedFields.end(), key) != mixedFields.end());
			Json const& value = values.is_object() && values.contains(key) ? values[key] : field.Default;

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
				DrawLabel(label, MakeTooltip(field, mixed), mixed, depth);
				ImGui::TableSetColumnIndex(1);
				ImGui::PushItemWidth(-FLT_MIN);
				FieldChange fieldChange;
				if (DrawValue(field, value, mixed, fieldChange) && !change)
				{
					fieldChange.Path = path;
					change = std::move(fieldChange);
				}
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
				return DrawAsset(field, value, m_AssetFilter, change);
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
		glm::quat rotation = glm::quat::wxyz(components[3], components[0], components[1], components[2]);
		rotation = glm::length(rotation) > 1e-6f ? glm::normalize(rotation) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

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
