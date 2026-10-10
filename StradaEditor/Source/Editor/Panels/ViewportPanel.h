#pragma once

#include "Editor/EditorCamera.h"
#include "Editor/EditorOperations.h"
#include "Editor/Viewport/PickingIdMap.h"

#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

namespace Strada
{
	enum class GizmoOperation : uint8_t
	{
		Translate = 0,
		Rotate,
		Scale
	};

	enum class GizmoSpace : uint8_t
	{
		Local = 0,
		World
	};

	// Shows the edited scene from the editor camera with the editor overlays: ground grid, selection outline, shapes of
	// selected cameras, lights and colliders, icons for cameras and lights, and a transform gizmo for the selection (edits
	// are undoable; one drag is one undo step). While playing (Play) it shows the game through its primary camera and,
	// focused, gives the game its input; Simulate keeps the editor view. A frame marks the view while playing. Input while hovered: right
	// mouse look + WASD/QE fly, Alt + left mouse orbit, middle mouse pan, wheel zoom, F focus, left click select (Ctrl toggles, Shift
	// adds), W/E/R gizmo operation, X local/world space, G grid; holding Ctrl while dragging the gizmo toggles snapping. Dropped assets:
	// meshes become entities on the ground under the cursor, materials go to the mesh under the cursor, environments to the sky light, and
	// scenes open.
	class ViewportPanel
	{
	public:
		// Opens a scene file (the editor asks about unsaved changes first).
		using OpenSceneCallback = std::function<void(std::filesystem::path const&)>;

		ViewportPanel();

		void SetOpenSceneCallback(OpenSceneCallback callback) { m_OpenScene = std::move(callback); }

		void OnImGuiRender(EditorOperations& operations, bool& open);

		EditorCamera& GetCamera() { return m_Camera; }
		GizmoOperation GetGizmoOperation() const { return m_GizmoOperation; }
		void SetGizmoOperation(GizmoOperation operation) { m_GizmoOperation = operation; }
		GizmoSpace GetGizmoSpace() const { return m_GizmoSpace; }
		void SetGizmoSpace(GizmoSpace space) { m_GizmoSpace = space; }
		bool IsGridVisible() const { return m_ShowGrid; }
		void SetGridVisible(bool visible) { m_ShowGrid = visible; }
		// Moves the camera so the entities (their meshes, or their positions) fill the view.
		void FocusEntities(Scene& scene, std::span<UUID const> entities);

		// The game view (Play) is focused: the game gets keyboard and mouse input.
		bool IsGameInputActive() const { return m_GameInputActive; }
		// The cursor in pixels of the game view, from its top-left corner (as the game reads it).
		glm::vec2 GetGameMousePosition() const { return m_GameMousePosition; }
		// The view's size in pixels when it was last drawn.
		glm::uvec2 GetViewportSize() const { return m_ViewportSize; }

	private:
		void HandleCameraInput(bool hovered);
		void HandleShortcuts(EditorContext& context, bool hovered);
		void Render(EditorContext& context);
		// Renders the running scene through its primary camera; false (rendering nothing) when it has none.
		bool RenderGame(EditorContext& context);
		void DrawPlayFrame(glm::vec2 const& imagePosition, glm::vec2 const& imageSize, EditorPlayState state);
		// Returns the camera or light entity whose icon is under the mouse (invalid when none).
		UUID DrawEntityIcons(EditorContext& context, glm::vec2 const& imagePosition, glm::vec2 const& imageSize, bool hovered);
		void UpdateGizmo(EditorOperations& operations, glm::vec2 const& imagePosition, glm::vec2 const& imageSize);
		void EndGizmoDrag(EditorContext& context);
		void HandleSelectionClicks(EditorOperations& operations, glm::vec2 const& imagePosition, bool hovered, UUID hoveredIcon);
		void ApplyPickResult(EditorOperations& operations);
		// Applies an asset dropped onto the viewport image at a pixel.
		void ApplyAssetDrop(EditorOperations& operations, AssetHandle asset, glm::vec2 const& pixel, glm::vec2 const& imageSize);
		void DrawToolbar(glm::vec2 const& imagePosition);

		EditorCamera m_Camera;
		// Null when no GPU renderer is available.
		Scope<SceneRenderer> m_Renderer;
		bool m_LookCaptured = false;
		bool m_PanCaptured = false;
		bool m_OrbitCaptured = false;

		// Picking IDs belong to one scene (EditorContext::GetSceneVersion).
		PickingIdMap m_PickingIds;
		uint64_t m_PickingSceneVersion = 0;
		// What the awaited pick result is for: a selection click, or a dropped material (assigned to the picked entity).
		struct PendingPick
		{
			SelectionMode Mode = SelectionMode::Replace;
			AssetHandle Material;
		};
		std::optional<PendingPick> m_PendingPick;
		// A pick requested for a previous scene is still in flight: its result is dropped.
		bool m_DiscardPick = false;
		bool m_ClickCandidate = false;

		GizmoOperation m_GizmoOperation = GizmoOperation::Translate;
		GizmoSpace m_GizmoSpace = GizmoSpace::Local;
		bool m_SnapEnabled = false;
		float m_TranslationSnap = 0.5f;
		// Degrees.
		float m_RotationSnap = 15.0f;
		float m_ScaleSnap = 0.1f;
		bool m_GizmoActive = false;
		uint64_t m_GizmoMergeKey = 0;
		uint64_t m_GizmoDragCount = 0;

		bool m_ShowGrid = true;
		bool m_GameInputActive = false;
		glm::vec2 m_GameMousePosition = glm::vec2(0.0f);
		glm::uvec2 m_ViewportSize = glm::uvec2(1280, 720);
		// Toolbar rectangle of the previous frame (clicks there do not reach the scene).
		glm::vec2 m_ToolbarMin = glm::vec2(0.0f);
		glm::vec2 m_ToolbarMax = glm::vec2(0.0f);

		OpenSceneCallback m_OpenScene;
	};
}
