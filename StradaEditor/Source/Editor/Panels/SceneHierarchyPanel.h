#pragma once

#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	struct EntityPreset;

	// The entity tree of the edited scene. Click selects (Ctrl toggles, Shift selects the displayed range), dragging rows
	// reparents and reorders entities (one undo step per drop; drop on the upper or lower edge of a row to place entities
	// before or after it), double-click frames the entity in the viewport, F2 or the context menu renames. Context menus
	// create, duplicate and delete entities; the search field filters by name. Main thread only.
	class SceneHierarchyPanel
	{
	public:
		// Called with the entity to frame when a row is double-clicked.
		using FocusCallback = std::function<void(UUID)>;
		// Where new root entities are placed (the viewport's focal point).
		using SpawnPositionProvider = std::function<glm::vec3()>;

		void SetFocusCallback(FocusCallback callback) { m_FocusCallback = std::move(callback); }
		void SetSpawnPositionProvider(SpawnPositionProvider provider) { m_SpawnPositionProvider = std::move(provider); }

		void OnImGuiRender(EditorOperations& operations, bool& open);

	private:
		struct Click
		{
			UUID Entity;
			bool Toggle = false;
			bool Range = false;
		};

		void DrawEntity(EditorOperations& operations, Entity entity, bool showChildren);
		void DrawRowInteractions(EditorOperations& operations, Entity entity, bool selected);
		void DrawRenameField(EditorOperations& operations, Entity entity, glm::vec2 const& labelPosition);
		void DrawRowContextMenu(EditorOperations& operations, Entity entity);
		void DrawBackground(EditorOperations& operations);
		void ApplyClick(EditorOperations& operations);
		void BeginRename(UUID entity, std::string name);
		// Runs scene modifications requested while drawing, once the tree is no longer being iterated.
		void Defer(std::function<void()> action) { m_Deferred.push_back(std::move(action)); }
		void CreatePreset(EditorOperations& operations, EntityPreset const& preset, UUID parent);
		// Creates an entity drawing a dropped mesh asset under parent (invalid = root).
		void CreateMeshEntity(EditorOperations& operations, AssetHandle mesh, UUID parent);
		// The selected entities when `entity` is selected, otherwise just `entity` (what row actions apply to).
		std::vector<UUID> GetActionTargets(EditorContext& context, UUID entity) const;

		FocusCallback m_FocusCallback;
		SpawnPositionProvider m_SpawnPositionProvider;
		std::string m_Filter;
		uint64_t m_SceneVersion = 0;

		// Rows in display order this frame (for range selection).
		std::vector<UUID> m_Displayed;
		std::optional<Click> m_Click;
		// A selected row pressed without modifiers selects only itself on release, unless the press started a drag.
		UUID m_PressedSelected = UUID::Invalid();
		UUID m_RangeAnchor = UUID::Invalid();

		// Selection changes made elsewhere (the viewport) reveal the new primary entity.
		UUID m_LastPrimary = UUID::Invalid();
		bool m_SelectionFromPanel = false;
		UUID m_Reveal = UUID::Invalid();
		std::vector<UUID> m_RevealAncestors;

		UUID m_Renaming = UUID::Invalid();
		std::string m_RenameBuffer;
		bool m_FocusRenameField = false;
		bool m_RenameFieldActivated = false;
		int m_RenameStartFrame = 0;

		std::vector<std::function<void()>> m_Deferred;
	};
}
