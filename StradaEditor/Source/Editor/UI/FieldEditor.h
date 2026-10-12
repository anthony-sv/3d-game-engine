#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Serialization/StructSerialization.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace Strada
{
	class AssetThumbnails;
	class Scene;

	// A value the user changed through a FieldEditor in the current frame.
	struct FieldChange
	{
		// Field names from the edited object down to the changed field (more than one for fields of nested structs).
		std::vector<std::string_view> Path;
		// The new value of that field in the scene-file JSON format.
		Json Value;
		// Vectors, bool vectors and quaternions edited as Euler angles: the single component the user changed, so applying
		// the change to several objects keeps their other components. Empty when the whole value changed.
		std::optional<uint32_t> Component;
		// Quaternions: the edited Euler angles in degrees.
		std::optional<glm::vec3> EulerDegrees;

		// A partial patch applying this change to an object whose current fields are `values` (its whole JSON object):
		// { "<Path[0]>": <that field's value with the change applied> }.
		Json MakePatch(Json const& values) const;
	};

	// What an asset reference drawn by FieldEditor::DrawAssetReference offers.
	struct AssetReferenceOptions
	{
		// The assets the picker lists and the box takes when dropped: those of one type, or of any with AssetType::None.
		AssetType Type = AssetType::None;
		// The edited objects reference different assets: the box shows "--".
		bool Mixed = false;
		// The picker lists None (no asset) above the assets.
		bool AllowNone = true;
		// "x" is enabled; its tooltip says what it does.
		bool CanClear = true;
		std::string_view ClearTooltip = "Clear";
	};

	// Draws editing widgets for reflected fields as the rows of a two-column property table (labels with descriptions as
	// tooltips, widgets chosen by the field kind and hints) and reports the user's change; callers turn it into an
	// undoable operation. Values are the scene-file JSON of the edited object. Colors are shown and picked in sRGB while
	// the data stays linear; quaternions are shown as Euler angles in degrees. A field's switch (FieldHints::EnabledBy) is
	// a check box in front of its name instead of a row of its own, and the field's value is disabled while the switch is
	// off; fields hidden in editors (FieldHints::HiddenInEditors) get no row. The name column starts as wide as the names
	// need (GetNameColumnShare) and users can resize it; names that do not fit are shortened with an ellipsis and shown in
	// full in their tooltips. Main thread only, inside an ImGui frame.
	class FieldEditor
	{
	public:
		// Draws one row per field into a new table. `mixedFields` names fields whose values differ between the edited
		// objects (several selected entities): they are flagged, and editing them sets the same value everywhere. `scene`
		// resolves entity references to names (may be null).
		std::optional<FieldChange> Draw(std::span<FieldDescriptor const> fields, Json const& values,
		                                std::span<std::string const> mixedFields = {}, Scene* scene = nullptr);
		// Asset fields show images of the assets these thumbnails have (textures, materials, meshes, prefabs and environments):
		// a small one in front of the path, a larger one in its tooltip and one in each row of the picker. The thumbnails must
		// outlive the editor; null (the default) shows paths only.
		void SetAssetPreviews(AssetThumbnails* previews) { m_AssetPreviews = previews; }

		// The share of a table's width the name column of these fields' table starts with when the table is tableWidth wide:
		// enough for the names with their switches and indentation, from 0.3 up to 0.6, or up to 0.5 when the table has
		// asset fields, whose paths need the room. Tables that line up with a field table (rows of their own above it) start
		// with the same share.
		static float GetNameColumnShare(std::span<FieldDescriptor const> fields, float tableWidth);

		// An asset reference as asset fields show it, as wide as the next item: a box with the asset's image (with
		// SetAssetPreviews) and its path, shortened to fit (UI::FitPath), that opens a searchable picker when clicked and
		// takes assets dropped from the content browser, then "..." (the picker) and "x". The picker is as wide as its
		// longest path within the window's screen, filters as the user types, and Up and Down move through the rows that
		// Enter takes. Returns the asset the user chose this frame when it differs from `current` (or the objects are mixed):
		// picked or dropped, invalid for None and "x".
		std::optional<AssetHandle> DrawAssetReference(AssetHandle current, AssetReferenceOptions const& options);

	private:
		// Euler angles last shown for a quaternion widget: kept while the quaternion is unchanged, so dragging one angle
		// never flips the others to an equivalent set.
		struct EulerState
		{
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Degrees = glm::vec3(0.0f);
			int LastFrame = 0;
		};

		void DrawRows(std::span<FieldDescriptor const> fields, Json const& values, std::span<std::string const> mixedFields,
		              bool parentMixed, std::vector<std::string_view>& path, std::optional<FieldChange>& change);
		// Draws the widget of one value; returns true and fills `change` (except Path) when the user changed it.
		bool DrawValue(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change);
		bool DrawQuaternion(Json const& value, FieldChange& change);
		bool DrawArray(FieldDescriptor const& field, Json const& value, FieldChange& change);
		// An asset field: DrawAssetReference, "x" clearing the reference.
		bool DrawAsset(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change);
		// Lists every registered asset of the type for the picker, with the width of the longest label at the current font,
		// unless that list is up to date.
		void UpdatePickerAssets(AssetType type);
		// The open picker's search field and list (None when allowed and nothing is searched, then every asset of the type
		// matching the search, with their images); returns the asset chosen this frame (invalid for None).
		std::optional<AssetHandle> DrawAssetPicker(AssetType type, AssetHandle current, bool allowNone);
		// The picker's search field reports Up and Down here (ImGuiInputTextFlags_CallbackHistory), so they move the row the
		// keyboard is on instead of leaving the field.
		static int OnPickerSearchKey(ImGuiInputTextCallbackData* data);
		void PruneEulerStates();

		Scene* m_Scene = nullptr;
		AssetThumbnails* m_AssetPreviews = nullptr;
		// Search text of the open asset picker.
		std::string m_AssetFilter;
		// Gives the picker's search field the keyboard again next frame (Enter without a match ended the input).
		bool m_FocusAssetSearch = false;
		// What the open picker lists: the assets of m_PickerType with their labels, as of asset version m_PickerVersion (none
		// before the first picker opens), and the width of the longest label at font size m_PickerFontSize.
		std::vector<std::pair<AssetHandle, std::string>> m_PickerAssets;
		AssetType m_PickerType = AssetType::None;
		std::optional<uint64_t> m_PickerVersion;
		float m_PickerLabelWidth = 0.0f;
		float m_PickerFontSize = 0.0f;
		// The picker's row the keyboard is on (Enter takes it), set for the search text m_PickerHighlightFilter; the rows
		// Up and Down moved it by this frame; whether the list scrolls to it.
		int m_PickerHighlight = 0;
		std::string m_PickerHighlightFilter;
		int m_PickerMove = 0;
		bool m_ScrollToPickerHighlight = false;
		std::unordered_map<uint32_t, EulerState> m_EulerStates;
		int m_LastPruneFrame = -1;
	};
}
