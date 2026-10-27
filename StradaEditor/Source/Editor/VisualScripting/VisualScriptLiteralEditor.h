#pragma once

#include "Editor/UI/FieldEditor.h"

#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace Strada
{
	class AssetThumbnails;

	// A literal of a type as FieldEditor edits it: a script class of one field ("Value") of the type's script field type
	// (the library's FieldType, AcceptedAssetType, Enumerators and IsFlags) whose default is the literal's value, and the
	// field's descriptor and editor value. The descriptor refers to the class's strings: the object is not copyable, and
	// moving it keeps them.
	class VisualScriptLiteralField
	{
	public:
		VisualScriptLiteralField() = default;
		VisualScriptLiteralField(VisualScriptLiteralField const&) = delete;
		VisualScriptLiteralField& operator=(VisualScriptLiteralField const&) = delete;
		VisualScriptLiteralField(VisualScriptLiteralField&&) = default;
		VisualScriptLiteralField& operator=(VisualScriptLiteralField&&) = default;

		std::string Type;
		ScriptClassInfo Class;
		FieldDescriptor Descriptor;
		// The value FieldEditor shows: the literal's, else the fallback's, else the type's default.
		Json Value;
		// The literal is not null and does not read as the type: Value is what the input takes instead.
		bool Invalid = false;
	};

	// The field of a literal of a type: what VisualScriptLiteralEditor draws. literal is as documents store it (null when
	// none); fallback is what stands for a missing literal (an input's library default, in the document encoding; null for the
	// type's default). Nothing for the types without a widget: those whose values are no script field values (structs,
	// classes, arrays, Strada.Matrix4, which the editor draws as its own grid), entities (whose only literal is none), and
	// enums the library does not describe. With a null library, the exposable types (FindVisualScriptExposableType).
	[[nodiscard]] std::optional<VisualScriptLiteralField>
	DescribeVisualScriptLiteral(std::string_view type, Json const& literal, Json const& fallback, VisualScriptLibrary const* library);
	// The canonical literal (EncodeVisualScriptLiteral) of the field's value with a FieldEditor change applied (a change of
	// one component keeps the others); nothing when the changed value is no value of the type.
	[[nodiscard]] std::optional<Json> EncodeVisualScriptLiteralChange(VisualScriptLiteralField const& field, FieldChange const& change,
	                                                                  VisualScriptLibrary const* library);
	// Strada.Matrix4's literal (16 finite floats, column-major) as its 16 numbers: the literal's, else the fallback's, else the
	// identity; and back, each number as the codec writes a float.
	[[nodiscard]] std::array<float, 16> ReadVisualScriptMatrixLiteral(Json const& literal, Json const& fallback);
	[[nodiscard]] Json WriteVisualScriptMatrixLiteral(std::array<float, 16> const& values);

	// Draws one literal of a typed pin, variable or local with FieldEditor's widgets (FieldEditor::DrawInlineValue), so the
	// canvas, Details and the inspector edit values alike: colours picked in sRGB over linear data, quaternions as Euler
	// degrees, enums as combos with a filter, [Flags] enums as check lists, assets as the inspector's asset boxes, and
	// Strada.Matrix4 as a 4 x 4 grid of drags (rows of the matrix; stored column-major). Types without a widget show "None".
	// Main thread only, inside an ImGui frame; each literal drawn in a frame needs an ID scope of its own (ImGui::PushID).
	class VisualScriptLiteralEditor
	{
	public:
		// Whether literals of the type get a widget.
		[[nodiscard]] static bool CanEdit(std::string_view type, VisualScriptLibrary const* library);

		// Draws the literal (null: none) of the type as wide as the next item; fallback is what stands for a missing literal
		// (DescribeVisualScriptLiteral). Returns the new literal, canonical, in the frame the user changes it. Compact is the
		// canvas's live widget (FieldInlineMode::Compact): a click on a colour, an enum or an asset sets *popupRequested, and
		// the caller draws the popup's body with DrawPopup; Strada.Matrix4 shows "Matrix" there (Details edits matrices).
		std::optional<Json> Draw(std::string_view type, Json const& literal, Json const& fallback, VisualScriptLibrary const* library,
		                         FieldInlineMode mode = FieldInlineMode::Normal, bool* popupRequested = nullptr);
		// The body of the popup a compact Draw asked for, between the caller's BeginPopup and EndPopup.
		std::optional<Json> DrawPopup(std::string_view type, Json const& literal, Json const& fallback, VisualScriptLibrary const* library);

		// Asset literals show images of the assets (FieldEditor::SetAssetPreviews).
		void SetAssetPreviews(AssetThumbnails* previews) { m_FieldEditor.SetAssetPreviews(previews); }

	private:
		std::optional<Json> DrawMatrix(Json const& literal, Json const& fallback);

		FieldEditor m_FieldEditor;
	};
}
