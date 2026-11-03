#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"

#include "Editor/ScriptFieldInspection.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr std::string_view MatrixType = "Strada.Matrix4";
		constexpr char const* FieldName = "Value";
		constexpr size_t MatrixSize = 4;

		VisualScriptEnumLookup MakeEnumLookup(VisualScriptLibrary const* library)
		{
			if (library == nullptr)
			{
				return {};
			}
			return [library](std::string_view type)
			{
				return library->FindEnum(type);
			};
		}

		// The script field of literals of the type: its field type, accepted asset type and enumerators. Nothing for the
		// types without a widget.
		std::optional<ScriptFieldInfo> MakeFieldInfo(std::string_view type, VisualScriptLibrary const* library)
		{
			ScriptFieldInfo info;
			info.Name = FieldName;
			if (library != nullptr)
			{
				if (std::optional<VisualScriptEnumInfo> const enumInfo = library->FindEnum(type))
				{
					if (!enumInfo->IsUsable())
					{
						return std::nullopt;
					}
					info.Type = enumInfo->UnderlyingType;
					info.Enumerators = enumInfo->Enumerators;
					info.IsFlags = enumInfo->IsFlags;
					return info;
				}
				if (VisualScriptTypeInfo const* const typeInfo = library->FindType(type))
				{
					info.Type = typeInfo->FieldType;
					info.AcceptedAssetType = typeInfo->AcceptedAssetType;
				}
			}
			if (info.Type == ScriptFieldType::None)
			{
				std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(type);
				if (!exposable)
				{
					return std::nullopt;
				}
				info.Type = exposable->FieldType;
				info.AcceptedAssetType = exposable->AcceptedAssetType;
			}
			// An entity's only literal is none: graphs are shared assets, so entities come from public variables.
			if (info.Type == ScriptFieldType::None || info.Type == ScriptFieldType::Entity)
			{
				return std::nullopt;
			}
			return info;
		}

		std::optional<std::array<float, 16>> ReadMatrix(Json const& literal)
		{
			if (!literal.is_array() || literal.size() != 16)
			{
				return std::nullopt;
			}
			std::array<float, 16> values{};
			for (size_t i = 0; i < values.size(); i++)
			{
				std::optional<float> const number = VisualScriptSerializer::ReadFloat(literal[i]);
				if (!number)
				{
					return std::nullopt;
				}
				values[i] = *number;
			}
			return values;
		}
	}

	std::optional<VisualScriptLiteralField> DescribeVisualScriptLiteral(std::string_view type, Json const& literal, Json const& fallback,
	                                                                    VisualScriptLibrary const* library)
	{
		std::optional<ScriptFieldInfo> info = MakeFieldInfo(type, library);
		if (!info)
		{
			return std::nullopt;
		}
		VisualScriptEnumLookup const enumLookup = MakeEnumLookup(library);
		VisualScriptLiteralField field;
		field.Type = std::string(type);
		std::optional<ScriptFieldValue> value;
		if (!literal.is_null())
		{
			value = DecodeVisualScriptLiteral(literal, type, enumLookup);
			field.Invalid = !value;
		}
		if (!value && !fallback.is_null())
		{
			value = DecodeVisualScriptLiteral(fallback, type, enumLookup);
		}
		info->DefaultValue = value ? *value : ScriptFieldValue::CreateDefault(info->Type);
		field.Class.Name = "Literal";
		field.Class.Fields.push_back(std::move(*info));
		std::vector<FieldDescriptor> descriptors = ScriptFieldInspection::DescribeFields(field.Class);
		field.Descriptor = std::move(descriptors.front());
		field.Value = field.Descriptor.Default;
		return field;
	}

	std::optional<Json> EncodeVisualScriptLiteralChange(VisualScriptLiteralField const& field, FieldChange const& change,
	                                                    VisualScriptLibrary const* library)
	{
		FieldChange edit = change;
		edit.Path = {field.Descriptor.Name};
		Json const values = Json::object({{FieldName, field.Value}});
		Json const patch = edit.MakePatch(values);
		Result<Json> stored = ScriptFieldInspection::SetEditorValue(field.Class, Json::object(), FieldName, patch[FieldName]);
		if (!stored)
		{
			return std::nullopt;
		}
		ScriptFieldInfo const& info = field.Class.Fields.front();
		Result<ScriptFieldValue> value =
			ScriptFieldValueFromJson(info.Type, stored.GetValue()[FieldName]["Value"], DeserializationContext{});
		if (!value)
		{
			return std::nullopt;
		}
		return EncodeVisualScriptLiteral(value.GetValue(), field.Type, MakeEnumLookup(library));
	}

	std::array<float, 16> ReadVisualScriptMatrixLiteral(Json const& literal, Json const& fallback)
	{
		if (std::optional<std::array<float, 16>> const values = ReadMatrix(literal))
		{
			return *values;
		}
		if (std::optional<std::array<float, 16>> const values = ReadMatrix(fallback))
		{
			return *values;
		}
		std::array<float, 16> identity{};
		for (size_t i = 0; i < MatrixSize; i++)
		{
			identity[i * MatrixSize + i] = 1.0f;
		}
		return identity;
	}

	Json WriteVisualScriptMatrixLiteral(std::array<float, 16> const& values)
	{
		Json literal = Json::array();
		for (float const value : values)
		{
			// Written as the codec writes a float (0.2f is 0.2), or as 0 for what no file can hold.
			std::optional<Json> number = EncodeVisualScriptLiteral(ScriptFieldValue::FromFloat(value), "float", {});
			literal.push_back(number ? std::move(*number) : Json(0));
		}
		return literal;
	}

	bool VisualScriptLiteralEditor::CanEdit(std::string_view type, VisualScriptLibrary const* library)
	{
		return type == MatrixType || MakeFieldInfo(type, library).has_value();
	}

	std::optional<Json> VisualScriptLiteralEditor::Draw(std::string_view type, Json const& literal, Json const& fallback,
	                                                    VisualScriptLibrary const* library, FieldInlineMode mode, bool* popupRequested)
	{
		if (type == MatrixType)
		{
			if (mode == FieldInlineMode::Compact)
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Matrix");
				return std::nullopt;
			}
			return DrawMatrix(literal, fallback);
		}
		std::optional<VisualScriptLiteralField> const field = DescribeVisualScriptLiteral(type, literal, fallback, library);
		if (!field)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("None");
			return std::nullopt;
		}
		FieldChange change;
		if (!m_FieldEditor.DrawInlineValue(field->Descriptor, field->Value, false, change, mode, popupRequested))
		{
			return std::nullopt;
		}
		return EncodeVisualScriptLiteralChange(*field, change, library);
	}

	std::optional<Json> VisualScriptLiteralEditor::DrawPopup(std::string_view type, Json const& literal, Json const& fallback,
	                                                         VisualScriptLibrary const* library)
	{
		std::optional<VisualScriptLiteralField> const field = DescribeVisualScriptLiteral(type, literal, fallback, library);
		if (!field)
		{
			return std::nullopt;
		}
		FieldChange change;
		if (!m_FieldEditor.DrawValuePopup(field->Descriptor, field->Value, change))
		{
			return std::nullopt;
		}
		return EncodeVisualScriptLiteralChange(*field, change, library);
	}

	std::string_view GetVisualScriptArrayElementType(std::string_view type)
	{
		constexpr std::string_view Suffix = "[]";
		if (type.size() <= Suffix.size() || !type.ends_with(Suffix))
		{
			return {};
		}
		std::string_view const element = type.substr(0, type.size() - Suffix.size());
		return element.ends_with(Suffix) ? std::string_view() : element;
	}

	std::optional<Json> MakeVisualScriptDefaultLiteral(std::string_view type, VisualScriptLibrary const* library)
	{
		if (type == MatrixType)
		{
			return WriteVisualScriptMatrixLiteral(ReadVisualScriptMatrixLiteral(Json(), Json()));
		}
		std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(type);
		if (exposable && exposable->FieldType == ScriptFieldType::Entity)
		{
			return Json("0");
		}
		std::optional<ScriptFieldInfo> const info = MakeFieldInfo(type, library);
		if (!info)
		{
			return std::nullopt;
		}
		return EncodeVisualScriptLiteral(ScriptFieldValue::CreateDefault(info->Type), type, MakeEnumLookup(library));
	}

	VisualScriptArrayLiteralOutcome DrawVisualScriptArrayLiteral(VisualScriptLiteralEditor& editor, std::string_view elementType,
	                                                             Json const& literal, VisualScriptLibrary const* library)
	{
		VisualScriptArrayLiteralOutcome outcome;
		auto const record = [&outcome](std::string part)
		{
			ImVec2 const min = ImGui::GetItemRectMin();
			ImVec2 const max = ImGui::GetItemRectMax();
			outcome.Parts.emplace_back(std::move(part), std::array<float, 4>{min.x, min.y, max.x, max.y});
		};
		std::optional<Json> const added = MakeVisualScriptDefaultLiteral(elementType, library);
		std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(elementType);
		// Entities' only literal is none: their elements show it, and are added and removed.
		bool const entities = exposable && exposable->FieldType == ScriptFieldType::Entity;
		if (!added || (!VisualScriptLiteralEditor::CanEdit(elementType, library) && !entities))
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("None (starts empty)");
			return outcome;
		}
		Json const elements = literal.is_array() ? literal : Json::array();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(fmt::format("{} {}", elements.size(), elements.size() == 1 ? "element" : "elements").c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(elements.size() >= MaxVisualScriptArrayLiteralElements);
		if (ImGui::SmallButton("+ Add Element"))
		{
			Json changed = elements;
			changed.push_back(*added);
			outcome.Edit = VisualScriptArrayLiteralEdit{VisualScriptArrayLiteralAction::Add, std::move(changed), elements.size()};
		}
		record("Add");
		ImGui::EndDisabled();

		float const handleWidth = ImGui::GetFrameHeight();
		// The list's identity in the payload: the caller's ID scope, which tells this array from any other drawn.
		ImGuiID const list = ImGui::GetID("##arrayElements");
		float const removeWidth = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		for (size_t index = 0; index < elements.size(); index++)
		{
			ImGui::PushID(static_cast<int>(index));
			ImGui::Button("=##handle", ImVec2(handleWidth, 0.0f));
			record(fmt::format("{} Handle", index));
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				VisualScriptArrayElementDrag const drag{list, static_cast<uint32_t>(index)};
				ImGui::SetDragDropPayload(VisualScriptArrayElementPayload, &drag, sizeof(drag));
				ImGui::Text("Element %zu", index);
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTarget())
			{
				// Only this list's elements are taken (peeked first, so another list's handle is not even offered the drop).
				VisualScriptArrayElementDrag drag;
				bool own = false;
				if (ImGuiPayload const* const offered = ImGui::GetDragDropPayload(); offered != nullptr &&
				                                                                     offered->IsDataType(VisualScriptArrayElementPayload) &&
				                                                                     offered->DataSize == static_cast<int>(sizeof(drag)))
				{
					std::memcpy(&drag, offered->Data, sizeof(drag));
					own = drag.List == list;
				}
				if (own && ImGui::AcceptDragDropPayload(VisualScriptArrayElementPayload) != nullptr)
				{
					size_t const from = drag.Index;
					if (from < elements.size() && from != index)
					{
						Json changed = elements;
						Json moved = changed[from];
						changed.erase(from);
						changed.insert(changed.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
						outcome.Edit = VisualScriptArrayLiteralEdit{VisualScriptArrayLiteralAction::Move, std::move(changed), index};
					}
				}
				ImGui::EndDragDropTarget();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Drag onto another element to move it there");
			}
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%zu", index);
			ImGui::SameLine();
			ImGui::BeginGroup();
			if (entities)
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("None");
			}
			else
			{
				ImGui::PushItemWidth(
					std::max(ImGui::GetContentRegionAvail().x - removeWidth - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFontSize()));
				std::optional<Json> const element = editor.Draw(elementType, elements[index], Json(), library);
				ImGui::PopItemWidth();
				if (element && !outcome.Edit)
				{
					Json changed = elements;
					changed[index] = *element;
					outcome.Edit = VisualScriptArrayLiteralEdit{VisualScriptArrayLiteralAction::Element, std::move(changed), index};
				}
			}
			ImGui::EndGroup();
			record(std::to_string(index));
			outcome.Active = outcome.Active || ImGui::IsItemActive();
			outcome.Deactivated = outcome.Deactivated || ImGui::IsItemDeactivated();
			ImGui::SameLine();
			bool const removed = ImGui::SmallButton("x");
			record(fmt::format("{} Remove", index));
			if (removed && !outcome.Edit)
			{
				Json changed = elements;
				changed.erase(index);
				outcome.Edit = VisualScriptArrayLiteralEdit{VisualScriptArrayLiteralAction::Remove, std::move(changed), index};
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Remove the element");
			}
			ImGui::PopID();
		}
		return outcome;
	}

	std::optional<Json> VisualScriptLiteralEditor::DrawMatrix(Json const& literal, Json const& fallback)
	{
		std::array<float, 16> values = ReadVisualScriptMatrixLiteral(literal, fallback);
		float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
		float const cellWidth =
			std::max(1.0f, (ImGui::CalcItemWidth() - spacing * static_cast<float>(MatrixSize - 1)) / static_cast<float>(MatrixSize));
		bool changed = false;
		// Shown as the matrix is written, row by row; stored column by column.
		for (size_t row = 0; row < MatrixSize; row++)
		{
			for (size_t column = 0; column < MatrixSize; column++)
			{
				size_t const index = column * MatrixSize + row;
				ImGui::PushID(static_cast<int>(index));
				if (column > 0)
				{
					ImGui::SameLine(0.0f, spacing);
				}
				ImGui::SetNextItemWidth(cellWidth);
				changed =
					ImGui::DragFloat("##cell", &values[index], 0.01f, -FLT_MAX, FLT_MAX, "%g", ImGuiSliderFlags_AlwaysClamp) || changed;
				ImGui::PopID();
			}
		}
		if (!changed)
		{
			return std::nullopt;
		}
		return WriteVisualScriptMatrixLiteral(values);
	}
}
