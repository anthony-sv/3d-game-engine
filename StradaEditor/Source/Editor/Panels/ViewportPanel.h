#pragma once

#include "Editor/EditorCamera.h"
#include "Editor/EditorOperations.h"
#include "Editor/Viewport/PickingIdMap.h"

#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>
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
	// are undoable; one drag is one undo step). Input while hovered: right mouse look + WASD/QE fly, Alt + left mouse orbit,
	// middle mouse pan, wheel zoom, F focus, left click select (Ctrl toggles, Shift adds), W/E/R gizmo operation, X
	// local/world space, G grid; holding Ctrl while dragging the gizmo toggles snapping.
	class ViewportPanel
	{
	public:
		ViewportPanel();

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

	private:
		void HandleCameraInput(bool hovered);
		void HandleShortcuts(EditorContext& context, bool hovered);
		void Render(EditorContext& context);
		// Returns the camera or light entity whose icon is under the mouse (invalid when none).
		UUID DrawEntityIcons(EditorContext& context, glm::vec2 const& imagePosition, glm::vec2 const& imageSize, bool hovered);
		void UpdateGizmo(EditorOperations& operations, glm::vec2 const& imagePosition, glm::vec2 const& imageSize);
		void EndGizmoDrag(EditorContext& context);
		void HandleSelectionClicks(EditorOperations& operations, glm::vec2 const& imagePosition, bool hovered, UUID hoveredIcon);
		void ApplyPickResult(EditorOperations& operations);
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
		// Selection mode of the click whose pick result is awaited.
		std::optional<SelectionMode> m_PendingPickMode;
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
		// Toolbar rectangle of the previous frame (clicks there do not reach the scene).
		glm::vec2 m_ToolbarMin = glm::vec2(0.0f);
		glm::vec2 m_ToolbarMax = glm::vec2(0.0f);
	};
}
