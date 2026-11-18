#pragma once

#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	class EditorOperations;

	// The Mixer panel (Docs/Editor.md, "The Mixer panel"): a Mixer asset as a mixing desk. It shows the asset it has open
	// (the one chosen in its asset field), else the project's (ProjectSettings::Mixer). A strip per bus, in the mixer's order, holds
	// its name (a click selects the bus), its meter, its volume fader and its mute; below them the selected bus's name (a
	// text field that renames every reference to it), its other fields in the generic field editor (its parent, its effects
	// in list order, its ducking), Add Bus (below the selected one) and Remove Bus; then the snapshots, each with Audition,
	// and their values in the field editor; then the mixer's speed of sound and impact sounds.
	//
	// Every edit is one undo step of the asset (EditorOperations::SetDataAssetFields): a fader's drag and a field editor
	// interaction are one step each (EditSession), a name commits once when it is left or on Enter (Escape keeps it), a
	// button or a mute is a step of its own. Removing a bus, renaming it and changing its effect list keep what names it
	// right (MixerEditing.h). An edit the asset's validation refuses changes nothing, and the panel shows why until the next
	// edit. An asset without a file of its own, or a read-only one, is shown disabled.
	//
	// While the editor plays the game through the shown asset (it is the project's mixer, the scene runner applied it to
	// the audio engine, and the audio engine runs), the panel is live: each strip's meter shows its bus's peak and RMS over
	// the last block mixed (AudioEngine::GetBusMeters), and Audition blends the running mix to a snapshot
	// (AudioEngine::TransitionTo over AuditionSeconds), so that edits of its values are heard as they are made; pressed
	// again, or when the panel is hidden, another asset is shown or the project changes, the mix blends back to the snapshot
	// it had before. The game calling TransitionTo itself, or play stopping (which returns the mix to the mixer's own values),
	// ends an audition with no blend of the panel's. An audition is never an undo step and never writes the asset. Main
	// thread only, inside an ImGui frame (but the getters).
	class MixerPanel
	{
	public:
		static constexpr char const* WindowName = "Mixer";
		// The merge keys of its steps, apart from every other panel's keys: "MIXR" plus a counter for the interactions of the
		// faders and the field editor (EditSession), "MIXS" plus a counter for every other edit, each a step of its own.
		static constexpr uint64_t MergeKeyPrefix = 0x4D49585200000000ull;
		static constexpr uint64_t StepMergeKeyBase = 0x4D49585300000000ull;
		// How long an audition's blend takes, to the snapshot and back.
		static constexpr float AuditionSeconds = 0.25f;

		// What the last draw showed, read by tests. Positions are screen coordinates of the middle of a widget.
		struct DrawRecord
		{
			using Point = std::array<float, 2>;

			struct Strip
			{
				std::string Name;
				float VolumeDb = 0.0f;
				bool Muted = false;
				bool Selected = false;
				// Linear, over the last block mixed; 0 while the panel is not live.
				float Peak = 0.0f;
				float Rms = 0.0f;
				Point Label = {0.0f, 0.0f};
				// Points just inside the fader's top and bottom ends (+24 dB and -80 dB).
				Point FaderTop = {0.0f, 0.0f};
				Point FaderBottom = {0.0f, 0.0f};
				Point Mute = {0.0f, 0.0f};
			};

			struct SnapshotRow
			{
				std::string Name;
				uint32_t ValueCount = 0;
				// Audition is offered (the panel is live and the running mix has the snapshot).
				bool CanAudition = false;
				// The running mix is auditioning it from the panel.
				bool Auditioning = false;
				Point Audition = {0.0f, 0.0f};
			};

			// The window's contents were drawn (not collapsed or behind another tab).
			bool Drawn = false;
			// The asset shown, and whether it is the project's because none was opened.
			AssetHandle Asset;
			bool FollowsProject = true;
			bool Editable = false;
			// A line about the asset (none named, not loaded, not editable), the last refusal, or what makes it live;
			// empty otherwise.
			std::string Status;
			// The meters and Audition read and blend the running mix.
			bool Live = false;
			// The snapshot the running mix blends to or holds (AudioEngine::GetCurrentSnapshot); empty when not live.
			std::string CurrentSnapshot;
			std::vector<Strip> Strips;
			// The selected bus, by index, and its name field and buttons.
			std::optional<uint32_t> Selected;
			Point NameField = {0.0f, 0.0f};
			Point AddBus = {0.0f, 0.0f};
			Point RemoveBus = {0.0f, 0.0f};
			std::vector<SnapshotRow> Snapshots;
			Point AddSnapshot = {0.0f, 0.0f};
		};

		MixerPanel();

		void OnImGuiRender(EditorOperations& operations, bool& open);
		// Ends an audition in progress: the layer calls it when the panel is not drawn (closed, the start screen).
		void OnHidden();

		// Opens a Mixer asset in the panel; invalid follows the project's again.
		void OpenAsset(AssetHandle asset);
		AssetHandle GetOpenedAsset() const { return m_Opened; }
		// Selects a bus by index (the strips' clicks do the same).
		void SelectBus(uint32_t bus) { m_Selected = bus; }

		DrawRecord const& GetLastDraw() const { return m_Record; }

	private:
		struct Audition
		{
			// As the running mix names it.
			std::string Snapshot;
			// The snapshot the mix had before (empty: the mixer's own values), which it blends back to.
			std::string Previous;
		};

		// The mixer drawn: the asset's data, copied when the asset shown or its version changes. An edit made while drawing
		// replaces the asset's data, so the frame draws this copy.
		void RefreshMixer(AssetHandle asset, uint64_t version, Mixer const& data);
		// Applies a new mixer (its buses, snapshots, speed of sound and impacts) as one step named description, under the
		// merge key given (0: a step of its own).
		void Commit(EditorOperations& operations, Mixer const& mixer, std::string description, uint64_t mergeKey);
		void DrawStrips(EditorOperations& operations, bool editable);
		void DrawStrip(EditorOperations& operations, uint32_t index, bool editable, float height);
		void DrawSelectedBus(EditorOperations& operations, bool editable);
		void DrawSnapshots(EditorOperations& operations, bool editable);
		void DrawMixerFields(EditorOperations& operations, bool editable);
		// A text field that commits once: returns the text when the field is left or Enter is pressed and it differs from
		// current. While the field is active it keeps what is typed; otherwise it shows current.
		std::optional<std::string> DrawCommittedText(char const* id, std::string const& current);
		// Starts, or with the snapshot auditioned already ends, an audition.
		void ToggleAudition(std::string const& snapshot);
		// Ends an audition: blends the mix back to the snapshot it had when blend is set (the mix is still the panel's).
		void EndAudition(bool blend);
		std::string GetTitle() const;

		FieldEditor m_FieldEditor;
		EditSession m_EditSession;
		DrawRecord m_Record;
		AssetHandle m_Opened;
		// The project the panel last drew for: another forgets what was opened.
		std::filesystem::path m_ProjectFile;
		AssetHandle m_Shown;
		std::optional<uint32_t> m_Selected;
		uint32_t m_TextId = 0;
		std::string m_Text;
		std::string m_Error;
		uint64_t m_Steps = 0;
		std::optional<Audition> m_Audition;
		Mixer m_Mixer;
		AssetHandle m_MixerAsset;
		uint64_t m_MixerVersion = 0;
		std::vector<FieldDescriptor> m_BusFields;
		std::vector<FieldDescriptor> m_SnapshotFields;
		std::vector<FieldDescriptor> m_OtherFields;
		// The live meters of the frame, by bus key.
		std::vector<std::pair<std::string, std::array<float, 2>>> m_Meters;
	};
}
