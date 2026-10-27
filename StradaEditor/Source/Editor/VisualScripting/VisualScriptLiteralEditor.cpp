#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"

#include "Editor/ScriptFieldInspection.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
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
