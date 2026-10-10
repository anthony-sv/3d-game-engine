#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiPayload;

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
	}

	namespace UI
	{
		// "PerspectiveFOV" -> "Perspective FOV", "UVTiling" -> "UV Tiling", "EV100" -> "EV100": serialized PascalCase names as
		// labels.
		std::string FormatDisplayName(std::string_view name);

		// Menu items for every entity preset, categories as submenus; returns the preset chosen this frame. Call inside an
		// open menu or popup.
		EntityPreset const* DrawEntityPresetMenuItems();

		// Makes the last item a source of an asset drag (DragDropPayload::Asset) showing the label while dragging.
		void AssetDragSource(AssetHandle asset, std::string_view label);
		// Call between ImGui::BeginDragDropTarget and EndDragDropTarget. The target highlights only for dragged assets that
		// are registered and accepted by the test; returns the asset dropped this frame, else invalid.
		AssetHandle AcceptAssetDrop(std::function<bool(AssetMetadata const&)> const& accepts);
		// Accepts registered assets of one of the types (any type when empty).
		AssetHandle AcceptAssetDrop(std::span<AssetType const> types = {});
		// The entities of a DragDropPayload::Entities payload.
		std::vector<UUID> ReadEntityPayload(ImGuiPayload const& payload);

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
