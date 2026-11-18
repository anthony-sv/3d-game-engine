#pragma once

#include "Editor/InputBindingCapture.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Core/InputActions.h"
#include "Strada/ImGui/ImGuiLayer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	class EditorOperations;

	// The Input Actions panel (Docs/Editor.md, "Input Actions"): an InputActions asset's actions and their bindings as a
	// table. It shows the asset it has open: the one chosen in its asset field or opened from the content browser, else the
	// project's (ProjectSettings::InputActions). Each action's row edits its name and type and adds or removes it; each
	// binding's row edits its controls as text, captures them by pressing them (Bind, or one button per part of a
	// composite: InputBindingCapture), lists the other actions it conflicts with (FindInputBindingConflicts) and removes it.
	// Below the table, the action selected (click its row) shows all its fields in the generic field editor. While the
	// editor plays the project's actions, the Value column shows each action's value in the running scene. Every edit is one
	// undo step of the asset (EditorOperations::SetDataAssetFields): text fields commit once, when left or on Enter (Escape
	// keeps the asset as it was); a capture commits when it ends with a control; a field editor interaction is one step.
	// An edit the asset's validation refuses changes nothing, and the panel shows why until the next edit. An asset without
	// a file of its own, or a read-only one, is shown disabled. Main thread only, inside an ImGui frame (but the getters).
	class InputActionsPanel
	{
	public:
		static constexpr char const* WindowName = "Input Actions";
		// The merge keys of its steps, apart from every other panel's keys: "INAC" plus a counter for the field editor's
		// interactions (EditSession), "INAD" plus a counter for every other edit (a committed text, a type, a button, a
		// capture), each a step of its own.
		static constexpr uint64_t MergeKeyPrefix = 0x494E414300000000ull;
		static constexpr uint64_t StepMergeKeyBase = 0x494E414400000000ull;

		// What the last draw showed, read by tests. Positions are screen coordinates of the middle of a widget.
		struct DrawRecord
		{
			using Point = std::array<float, 2>;

			struct BindingRow
			{
				std::string Controls;
				// The other actions this binding shares a control with, in the order of the map.
				std::vector<std::string> Conflicts;
				// The binding's capture is listening (Part: the whole binding -1, else the part).
				bool Listening = false;
				int32_t ListeningPart = -1;
				Point ControlsField = {0.0f, 0.0f};
				// The Bind button (the whole binding), and one button per part of a composite (empty otherwise).
				Point Bind = {0.0f, 0.0f};
				std::vector<Point> Parts;
				Point Remove = {0.0f, 0.0f};
			};

			struct ActionRow
			{
				std::string Name;
				InputActionType Type = InputActionType::Button;
				// Where a click selects the action (its row, outside its widgets).
				Point Row = {0.0f, 0.0f};
				Point NameField = {0.0f, 0.0f};
				Point TypeCombo = {0.0f, 0.0f};
				Point AddBinding = {0.0f, 0.0f};
				Point Remove = {0.0f, 0.0f};
				// The value in the running scene ("0.75", "-1.00", "0.00, 1.00"), empty without a live game.
				std::string Value;
				bool Down = false;
				std::vector<BindingRow> Bindings;
			};

			// The window's contents were drawn (not collapsed or behind another tab).
			bool Drawn = false;
			// The asset shown, and whether it is the project's because none was opened.
			AssetHandle Asset;
			bool FollowsProject = true;
			bool Editable = false;
			// A line about the asset (none named, not loaded, not editable) or the last refusal, empty otherwise.
			std::string Status;
			// The Value column shows a running game's values.
			bool Live = false;
			std::vector<ActionRow> Actions;
			Point AddAction = {0.0f, 0.0f};
			// The selected action whose fields show below the table, if any.
			std::optional<uint32_t> Selected;
			// The capture's prompt and its text, while one listens.
			std::string Prompt;
		};

		InputActionsPanel();

		void OnImGuiRender(EditorOperations& operations, bool& open);
		// Ends a capture in progress: the layer calls it when the panel is not drawn (closed, the start screen).
		void OnHidden();

		// Opens an InputActions asset in the panel (the content browser's Open); invalid follows the project's again.
		void OpenAsset(AssetHandle asset);
		// The asset opened in the panel (invalid when it follows the project's).
		AssetHandle GetOpenedAsset() const { return m_Opened; }

		// The game input router's capture filter (GameInputRouter::SetCaptureFilter): takes a window's key and mouse presses
		// while a binding is being captured (InputBindingCapture::TakeEvent). A capture whose panel was not drawn in the last frame is
		// cancelled instead.
		bool TakeCaptureEvent(Event const& event, ImGuiLayer::InputContext const& context);
		InputBindingCapture const& GetCapture() const { return m_Capture; }

		DrawRecord const& GetLastDraw() const { return m_Record; }

	private:
		// A text field that commits once: returns the text when the field is left or Enter is pressed and it differs from
		// `current`. While the field is active it keeps what is typed; otherwise it shows `current`.
		std::optional<std::string> DrawCommittedText(char const* id, std::string const& current);
		// Applies a new map as one step named `description`, under the merge key given (0: a new step of its own).
		void Commit(EditorOperations& operations, InputActionMap const& map, std::string description, uint64_t mergeKey);
		void UpdateCapture(EditorOperations& operations, InputActionMap const& map, float seconds);
		// Copies the asset's map and works out its bindings' facts when the asset shown or its version changed since the last
		// copy; otherwise nothing (an idle panel copies, parses and compares nothing per frame).
		void RefreshMap(AssetHandle asset, uint64_t version, InputActionMap const& data);
		void DrawTable(EditorOperations& operations, InputActionMap const& map, bool editable);
		void DrawBindingRow(EditorOperations& operations, InputActionMap const& map, uint32_t action, uint32_t binding, bool editable,
		                    DrawRecord::ActionRow& row);
		void DrawDetails(EditorOperations& operations, InputActionMap const& map, bool editable);
		void StartCapture(InputActionMap const& map, uint32_t action, uint32_t binding, int32_t part);

		FieldEditor m_FieldEditor;
		EditSession m_EditSession;
		DrawRecord m_Record;
		// The asset opened in the panel; invalid follows the project's.
		AssetHandle m_Opened;
		// The project the panel last drew for: another forgets what was opened.
		std::filesystem::path m_ProjectFile;
		// The asset shown in the last draw.
		AssetHandle m_Shown;
		std::optional<uint32_t> m_Selected;
		// The text field being typed into, and what it holds.
		uint32_t m_TextId = 0;
		std::string m_Text;
		// The last refusal, shown until the next edit that is made.
		std::string m_Error;
		// What the last capture said (cancelled, timed out), shown until the next one starts.
		std::string m_CaptureMessage;
		InputBindingCapture m_Capture;
		// The capture a mouse press away from its prompt ended, until the mouse buttons are up again: when that press was on
		// the capture's own button, its click ends the capture rather than starting it again.
		std::optional<InputBindingCaptureTarget> m_ClickedAway;
		// Steps made with keys of StepMergeKeyBase.
		uint64_t m_Steps = 0;
		// The ImGui frame the panel's contents were last drawn in.
		int m_DrawnFrame = -1;
		// What a binding row shows that only the map decides.
		struct BindingFacts
		{
			std::vector<std::string> Conflicts;
			// 2 or 4 for a composite, else 0.
			uint32_t Parts = 0;
		};
		// The map drawn: the asset's data, copied when the asset shown or its version (drawn from the data asset registry's
		// epoch, never repeated) changes. An edit made while drawing replaces the asset's data, so the frame draws this copy.
		InputActionMap m_Map;
		AssetHandle m_MapAsset;
		uint64_t m_MapVersion = 0;
		// Per action and binding of m_Map.
		std::vector<std::vector<BindingFacts>> m_Facts;
		std::vector<FieldDescriptor> m_ActionFields;
	};
}
