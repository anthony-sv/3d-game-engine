#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiPayload;
struct ImVec2;

namespace Strada
{
	class CommandHistory;
	struct EntityPreset;

	// Types of ImGui drag-and-drop payloads exchanged between panels.
	namespace DragDropPayload
	{
		// An array of entity UUIDs (uint64_t) of the edited scene.
		inline constexpr char const* Entities = "STRADA_ENTITIES";
		// One AssetHandle (uint64_t).
		inline constexpr char const* Asset = "STRADA_ASSET";
		// A folder of the asset directory: its relative path as UTF-8 characters (no terminator).
		inline constexpr char const* AssetFolder = "STRADA_ASSET_FOLDER";
		// A file of the asset directory that is not an asset (a model's buffer): its relative path as UTF-8 characters (no
		// terminator).
		inline constexpr char const* AssetFile = "STRADA_ASSET_FILE";
		// A C# source of the project's scripts: its absolute path as UTF-8 characters (no terminator).
		inline constexpr char const* ScriptFile = "STRADA_SCRIPT_FILE";
		// A folder of the project's scripts: its path relative to their directory (ScriptProject::GetSourceDirectory) as UTF-8
		// characters (no terminator).
		inline constexpr char const* ScriptFolder = "STRADA_SCRIPT_FOLDER";
	}

	namespace UI
	{
		// "PerspectiveFOV" -> "Perspective FOV", "UVTiling" -> "UV Tiling", "EV100" -> "EV100": serialized PascalCase names as
		// labels.
		std::string FormatDisplayName(std::string_view name);
		// How asset fields show an asset: its path in the asset directory with the extension ("Sounds/Hum.flac", followed by
		// " (missing)" when the file is gone), "builtin://<Name>" for built-in assets, the name of assets without a file of
		// their own ("Helmet.glb/Materials/Visor"), "None" without an asset and "Unknown asset <ID>" for unregistered handles.
		std::string DescribeAssetReference(AssetHandle asset);
		// The current font's ellipsis as UTF-8 text, the one ImGui shortens labels with: U+2026, or a "..." glyph made of dots
		// when the font has none. Inside an ImGui frame.
		std::string GetEllipsis();
		// The part of a file's name that tells it apart from the other files of its set: what follows the name of one of its
		// folders when the file name starts with that name and a word separator ('_', '-' or a space), ignoring case, and
		// says more than a resolution ("1k") and the extension: "diff_1k.jpg" of
		// "Models/wooden_crate_01/textures/wooden_crate_01_diff_1k.jpg". Empty when there is none.
		std::string_view GetDistinctFileNamePart(std::string_view path);
		// Shortens a path that is wider than maxWidth at the current font, with GetEllipsis() where text is left out: leading
		// folders give way first (".../Wood/Oak_Color.png"), then the name stands alone ("Oak_Color.png"; references with a
		// scheme, such as "builtin://Plane", show their name), then the files of a set keep what tells them apart
		// (GetDistinctFileNamePart: "...diff_1k.jpg"), and at last the name loses its middle, keeping its start and, in up
		// to half of the room, its end with the extension ("Oak_...or.png"); never cuts a UTF-8 character. Paths that fit
		// come back unchanged. Inside an ImGui frame.
		std::string FitPath(std::string_view path, float maxWidth);

		// Menu items for every entity preset (EntityPresets::GetMenu: Core's directly, the other categories as submenus);
		// returns the preset chosen this frame. openSubmenu, a category, opens its submenu as if the user pointed at it (it
		// then stays open like one the user opened). Call inside an open menu or popup.
		EntityPreset const* DrawEntityPresetMenuItems(std::string_view openSubmenu = {});

		// Makes the last item a source of an asset drag (DragDropPayload::Asset) showing the label while dragging.
		void AssetDragSource(AssetHandle asset, std::string_view label);
		// Call between ImGui::BeginDragDropTarget and EndDragDropTarget. The target highlights only for dragged assets that
		// are registered and accepted by the test; returns the asset dropped this frame, else invalid.
		AssetHandle AcceptAssetDrop(std::function<bool(AssetMetadata const&)> const& accepts);
		// Accepts registered assets of one of the types (any type when empty).
		AssetHandle AcceptAssetDrop(std::span<AssetType const> types = {});
		// The entities of a DragDropPayload::Entities payload.
		std::vector<UUID> ReadEntityPayload(ImGuiPayload const& payload);
		// Makes the last item a source of a script drag (DragDropPayload::ScriptFile) showing the label while dragging.
		void ScriptDragSource(std::filesystem::path const& file, std::string_view label);
		// Call between ImGui::BeginDragDropTarget and EndDragDropTarget: the C# source dropped this frame, if any.
		std::optional<std::filesystem::path> AcceptScriptDrop();

		// A file or folder name laid out in two lines of the given width with the current font, as content browser tiles show
		// it: the first line breaks at the last place that fits after a space, '_' or '-', before a '.' or before a word of a
		// PascalCase name ("Showcase|Tests.cs"), the second is shortened with "..." when the rest does not fit: the files of
		// a set keep the part that tells them apart (`distinct`, from GetDistinctFileNamePart: "...diff_1k.jpg" of
		// "wooden_crate_01_diff_1k.jpg", or its last words while they say more than a resolution) when it fits, other names
		// the given extension ("LongName.smat": "LongNa...smat"). The second line is empty for names that fit on one.
		std::array<std::string, 2> SplitTileLabel(std::string_view name, float width, std::string_view extension = {},
		                                          std::string_view distinct = {});
		// A length of time as a label: "40 ms" below a second, "2.4 s" below ten seconds, "1:05" (minutes and seconds) above.
		std::string FormatDuration(double seconds);

		// Icons of the toolbar buttons, drawn as shapes (the editor needs no icon font).
		enum class ToolbarIcon : uint8_t
		{
			Play = 0,
			Stop,
			Pause,
			Step
		};

		// The size of toolbar buttons at the current font size.
		ImVec2 GetToolbarButtonSize();
		// A button showing an icon; active buttons are highlighted. Returns true when clicked. Follows ImGui::BeginDisabled.
		bool ToolbarButton(char const* id, ToolbarIcon icon, bool active);

		// Logs a failed user action; the console panel shows it.
		template<typename T>
		void ReportFailure(Result<T> const& result, std::string_view action)
		{
			if (!result)
			{
				ST_ERROR("{} failed: {}", action, result.GetError());
			}
		}
	}

	// Groups the changes of one continuous widget interaction (dragging a value, typing into a field) into one undo step.
	// Call Update once per frame before drawing, with whether an ImGui item is active, and pass GetMergeKey to the
	// operations of every change made through the widgets.
	class EditSession
	{
	public:
		// Keys combine the prefix (one per panel) with a running count.
		explicit EditSession(uint64_t keyPrefix);

		// When the interaction that produced changes has ended, the merge is broken so the next change starts a new step.
		void Update(bool interactionActive, CommandHistory& history);
		uint64_t GetMergeKey();

	private:
		uint64_t m_KeyPrefix;
		uint64_t m_SessionCount = 0;
		uint64_t m_CurrentKey = 0;
	};
}
