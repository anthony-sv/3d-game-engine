#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Serialization/StructSerialization.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <limits>
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
	class FieldEditor;
	class Scene;

	// One step of the path from an edited object down to a changed value: a field name, and the index of the element the path
	// goes into when the field is an array.
	struct FieldPathStep
	{
		std::string_view Field;
		std::optional<uint32_t> Element = std::nullopt;

		bool operator==(FieldPathStep const& other) const = default;
	};

	// How FieldArrayEdit changes an array.
	enum class FieldArrayEditKind : uint8_t
	{
		// Puts FieldChange::Value at Index (FieldArrayEdit::End, or any index at or past the array's size, appends).
		Insert = 0,
		// Removes the element at Index.
		Remove,
		// Takes the element at Index out and puts it back at Target.
		Move,
		// Puts a copy of the element at Index right after it.
		Duplicate
	};

	// A change of an array as a list, by index, so that each edited object keeps its own elements.
	struct FieldArrayEdit
	{
		// An Insert's Index that appends to every array, whatever its size.
		static constexpr uint32_t End = std::numeric_limits<uint32_t>::max();

		FieldArrayEditKind Kind = FieldArrayEditKind::Insert;
		uint32_t Index = 0;
		uint32_t Target = 0;

		bool operator==(FieldArrayEdit const& other) const = default;
	};

	// What a struct drawer (FieldEditor::SetStructDrawer) draws from.
	struct FieldStructDrawContext
	{
		FieldDescriptor const& Field;
		// The struct's value (an object of its fields).
		Json const& Value;
		// The edited objects' values differ: the drawer shows the first's, and an edit sets every object's.
		bool Mixed = false;
		// Disabled fields report no change: drawers draw their widgets disabled otherwise.
		bool Editable = true;
		// The field editor drawing the struct, for the asset references and values the drawer draws with it.
		FieldEditor& Editor;
	};

	// A struct drawer's change: the struct's whole new value, the undo step's name (empty for the caller's own) and a merge
	// key (0 for the caller's, its interaction): a widget that commits at its own cadence (a curve editor's nudges) gives its
	// own key.
	struct FieldStructEdit
	{
		Json Value;
		std::string Description;
		uint64_t MergeKey = 0;
	};

	// Draws a reflected struct of one type wherever it appears in a field table (a field, a nested struct, an array element)
	// instead of its fields' rows, and returns the user's change of this frame. Main thread only, inside an ImGui frame, in
	// the value column of the struct's row.
	using FieldStructDrawer = std::function<std::optional<FieldStructEdit>(FieldStructDrawContext const& context)>;

	// The editor-wide struct drawers every FieldEditor starts with, by the struct's name (StructTraits<T>::Name): one list in
	// FieldStructDrawers.cpp, where a type's track adds its line.
	std::vector<std::pair<std::string_view, FieldStructDrawer>> GetFieldStructDrawers();

	// A value the user changed through a FieldEditor in the current frame.
	struct FieldChange
	{
		// From the edited object down to the changed value: field names (more than one for fields of nested structs and of
		// array elements), each with the index of the element the path goes into when the field is an array. An element of
		// an array of plain values is changed by a path whose last step names its index.
		std::vector<FieldPathStep> Path;
		// The new value in the scene-file JSON format (an inserted element for ArrayEdit's Insert).
		Json Value;
		// Vectors, bool vectors and quaternions edited as Euler angles: the single component the user changed, so applying
		// the change to several objects keeps their other components. Empty when the whole value changed.
		std::optional<uint32_t> Component;
		// Quaternions: the edited Euler angles in degrees.
		std::optional<glm::vec3> EulerDegrees;
		// The array the path ends at changed as a list (an element added, removed, moved or duplicated) instead of taking
		// Value.
		std::optional<FieldArrayEdit> ArrayEdit;
		// A struct drawer's name for the undo step and its merge key; empty and 0 take the caller's own.
		std::string Description;
		uint64_t MergeKey = 0;

		// A partial patch applying this change to an object whose current fields are `values` (its whole JSON object):
		// { "<Path[0].Field>": <that field's value with the change applied> }. Elements are changed by index in the object's
		// own arrays: an element edit, a removal, a move or a duplication at an index the object's array does not have leaves
		// the array as it is, and an insertion past its end appends. Empty without a path, and when the path goes into an
		// element of a field `values` does not have.
		Json MakePatch(Json const& values) const;
	};

	// How FieldEditor::DrawInlineValue draws a value.
	enum class FieldInlineMode : uint8_t
	{
		// As in a field table's row.
		Normal = 0,
		// The visual script canvas' live widget, showing what the chip it replaces shows: integers as "%lld", floats as "%g",
		// vectors and quaternions (in Euler degrees, "%g") as one drag per component with its axis's stripe at its left edge and
		// no axis buttons, and colours, enums (the stored name), [Flags] enums (the stored names joined with " | ") and assets
		// (the asset's display name) as a frame only, whose click asks the caller for a popup (drawn at the window's normal size
		// with DrawValuePopup) instead of opening one. Its widgets show no tooltips, which would take the canvas's zoomed font
		// and spacing.
		Compact
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
	// undoable operation (Docs/Architecture.md §11). Arrays of plain values are a row of widgets per element with "-",
	// then "+"; arrays of reflected structs fold open into a row per element ("<index>: <title>", with a menu to move,
	// duplicate, insert above and remove it) over the element's own field rows, then Add Element; buttons that add are
	// disabled at the field's MaxCount. Every array change reports the element's index or the list edit
	// (FieldChange::ArrayEdit), never the whole array, so each edited object keeps its own elements. A reflected struct
	// with a struct drawer is drawn by it instead of its rows. Values are the scene-file JSON of the edited object. Colors are shown and
	// picked in sRGB while the data stays linear; quaternions are shown as Euler angles in degrees. A field's switch
	// (FieldHints::EnabledBy) is a check box in front of its name instead of a row of its own, and the field's value is disabled while the
	// switch is off; fields hidden in editors (FieldHints::HiddenInEditors) get no row. The name column starts as wide as the names need
	// (GetNameColumnShare) and users can resize it; names that do not fit are shortened with an ellipsis and shown in full in their
	// tooltips. Main thread only, inside an ImGui frame.
	class FieldEditor
	{
	public:
		// With the editor-wide struct drawers (GetFieldStructDrawers).
		FieldEditor();

		// Draws every struct named structName (StructTraits<T>::Name) with the drawer in this editor's tables, replacing the
		// editor-wide drawer of that struct; an empty drawer removes it.
		void SetStructDrawer(std::string structName, FieldStructDrawer drawer);
		bool HasStructDrawer(std::string_view structName) const;

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

		// Draws the widget of one value without a table row, as wide as the next item (ImGui::SetNextItemWidth or the item
		// width pushed), in the current ID scope; returns true and fills change (except Path) when the user changed it (an
		// array's whole new value, as no path names its elements here). mixed
		// shows the value as differing between edited objects. In Compact mode a click on the frame of a colour, an enum, a
		// [Flags] enum or an asset sets *popupRequested and opens nothing: the caller opens a popup of its own at the window's
		// normal size and draws its body with DrawValuePopup. popupRequested must not be null in Compact mode (a click is then
		// lost); Normal mode leaves it unchanged.
		bool DrawInlineValue(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change,
		                     FieldInlineMode mode = FieldInlineMode::Normal, bool* popupRequested = nullptr);
		// The text a compact frame shows for an enum ("LeftShift"), a [Flags] enum ("Read | Write", "None") or an asset (its
		// display name, "None"): what the visual script canvas' chip it replaces shows. Empty for the other kinds, which show
		// drags or check boxes. Main thread only (it reads the asset registry).
		[[nodiscard]] static std::string DescribeCompactText(FieldDescriptor const& field, Json const& value);
		// The body of the popup a compact widget asked for, between the caller's BeginPopup and EndPopup: the colour picker,
		// the enum list (with its filter), the [Flags] check list or the asset picker; other values draw their normal widget.
		// Returns true and fills change (except Path) when the user changed the value. Picking an enumerator or an asset
		// closes the popup.
		bool DrawValuePopup(FieldDescriptor const& field, Json const& value, FieldChange& change);
		// The asset picker in a popup of its own (the caller opens popupId with ImGui::OpenPopup in the same ID scope):
		// a filter with the keyboard, the rows of every asset of the type (None first when allowNone), Up, Down and Enter.
		// Returns the asset chosen this frame when it differs from current (invalid for None); choosing closes the popup.
		std::optional<AssetHandle> DrawAssetPickerPopup(char const* popupId, AssetType type, AssetHandle current, bool allowNone);

		// Enums of more than this many enumerators get a filter above their list.
		static constexpr size_t MaxUnfilteredEnumerators = 12;

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
		              bool parentMixed, int depth, std::vector<FieldPathStep>& path, std::optional<FieldChange>& change);
		// The rows of the elements of an array of reflected structs, below its header row (the path's last step names the
		// array): a folding row per element with its menu, over the element's field rows at depth + 1 (or its struct drawer),
		// then Add Element.
		void DrawStructArrayElements(FieldDescriptor const& field, Json const& value, bool mixed, int depth,
		                             std::vector<FieldPathStep>& path, std::optional<FieldChange>& change);
		// The value column of a struct drawn by its drawer; returns true and fills change (except Path) when it changed.
		bool DrawWithStructDrawer(FieldStructDrawer const& drawer, FieldDescriptor const& field, Json const& value, bool mixed,
		                          FieldChange& change);
		// The drawer of the struct type of that name, or null.
		FieldStructDrawer const* FindStructDrawer(std::string_view structName) const;
		// Draws the widget of one value; returns true and fills `change` (except Path) when the user changed it.
		bool DrawValue(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change);
		bool DrawCompactValue(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change, bool& popupRequested);
		// The enumerators to pick from (in an enum combo's popup or a compact enum's): with more than
		// MaxUnfilteredEnumerators, a filter that has the keyboard when the list appears and keeps the enumerators whose names
		// hold its text (ignoring case), exact names first, then those that start with it, each in declaration order; Up and
		// Down move the highlighted row and Enter takes it. width is the list's width (0: the window's). Returns true and
		// fills change when one was picked, and closes the popup.
		bool DrawEnumList(FieldDescriptor const& field, std::string const& current, float width, FieldChange& change);
		static int OnEnumFilterKey(ImGuiInputTextCallbackData* data);
		bool DrawQuaternion(Json const& value, FieldChange& change, FieldInlineMode mode);
		// An array of plain values: a row of widgets per element with "-", then "+" (disabled at the field's MaxCount). Returns
		// true and fills change when the user changed it: an element's value with its index in *element, or the list edit in
		// change.ArrayEdit.
		bool DrawArrayElements(FieldDescriptor const& field, Json const& value, FieldChange& change, std::optional<uint32_t>& element);
		// DrawArrayElements for widgets drawn without a path (DrawInlineValue, DrawValuePopup): the whole new array.
		bool DrawArray(FieldDescriptor const& field, Json const& value, FieldChange& change);
		// An asset field: DrawAssetReference, "x" clearing the reference.
		bool DrawAsset(FieldDescriptor const& field, Json const& value, bool mixed, FieldChange& change);
		// Lists every registered asset of the type for the picker, with the width of the longest label at the current font,
		// unless that list is up to date.
		void UpdatePickerAssets(AssetType type);
		// The open picker's search field and list (None when allowed and nothing is searched, then every asset of the type
		// matching the search, with their images); returns the asset chosen this frame (invalid for None).
		// width is the picker's (0: the window's).
		std::optional<AssetHandle> DrawAssetPicker(AssetType type, AssetHandle current, bool allowNone, float width = 0.0f);
		// The width the picker of the type's assets needs: as wide as its longest path within the screen, at least minWidth.
		float GetPickerWidth(AssetType type, float minWidth);
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
		// The open enum list's filter, the row the keyboard is on (Enter takes it) and the rows Up and Down moved it by this
		// frame.
		std::string m_EnumFilter;
		int m_EnumHighlight = 0;
		int m_EnumMove = 0;
		// Asks for the keyboard for the enum filter until it has it (as the list opens, and after Enter without a match).
		bool m_FocusEnumFilter = false;
		// The enum filter or the asset picker's search field takes the keyboard with text typed before: its cursor goes after
		// the text.
		bool m_PlaceFilterCursor = false;
		std::unordered_map<uint32_t, EulerState> m_EulerStates;
		std::unordered_map<std::string, FieldStructDrawer> m_StructDrawers;
		// The element an array edit put in place (DrawStructArrayElements' scope and the element's index): it opens when it
		// shows, as an open element stays open where it moves. 0 for none.
		uint32_t m_OpenElementScope = 0;
		uint32_t m_OpenElement = 0;
		int m_LastPruneFrame = -1;
	};
}
