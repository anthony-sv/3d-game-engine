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

	// Draws editing widgets for reflected fields as the rows of a two-column property table (labels with descriptions as
	// tooltips, widgets chosen by the field kind and hints) and reports the user's change; callers turn it into an
	// undoable operation. Values are the scene-file JSON of the edited object. Colors are shown and picked in sRGB while
	// the data stays linear; quaternions are shown as Euler angles in degrees. Main thread only, inside an ImGui frame.
	class FieldEditor
	{
	public:
		// Draws one row per field into a new table. `mixedFields` names fields whose values differ between the edited
		// objects (several selected entities): they are flagged, and editing them sets the same value everywhere. `scene`
		// resolves entity references to names (may be null).
		std::optional<FieldChange> Draw(std::span<FieldDescriptor const> fields, Json const& values,
		                                std::span<std::string const> mixedFields = {}, Scene* scene = nullptr);
		// Asset fields show images of the assets these thumbnails have (textures, materials, meshes, prefabs and environments):
		// a small one in front of the path and a larger one in its tooltip. The thumbnails must outlive the editor; null (the
		// default) shows paths only.
		void SetAssetPreviews(AssetThumbnails* previews) { m_AssetPreviews = previews; }

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
		// An asset reference: a box showing the asset's path (opens the picker; accepts assets dropped from the content
		// browser), a "..." button opening the picker and an "x" button clearing the reference.
		bool DrawAsset(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change);
		// The open picker's search field and list (None, then every asset of the type matching the search); returns the asset
		// chosen this frame (invalid for None). Enter takes the first match.
		std::optional<AssetHandle> DrawAssetPicker(AssetType type, AssetHandle current);
		void PruneEulerStates();

		Scene* m_Scene = nullptr;
		AssetThumbnails* m_AssetPreviews = nullptr;
		// Search text of the open asset picker.
		std::string m_AssetFilter;
		// Gives the picker's search field the keyboard again next frame (Enter without a match ended the input).
		bool m_FocusAssetSearch = false;
		// What the open picker lists: the assets of m_PickerType with their labels, as of asset version m_PickerVersion (none
		// before the first picker opens).
		std::vector<std::pair<AssetHandle, std::string>> m_PickerAssets;
		AssetType m_PickerType = AssetType::None;
		std::optional<uint64_t> m_PickerVersion;
		std::unordered_map<uint32_t, EulerState> m_EulerStates;
		int m_LastPruneFrame = -1;
	};
}
