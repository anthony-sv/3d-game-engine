#pragma once

#include "Editor/Viewport/ViewportOverlays.h"

#include "Strada/Core/UUID.h"
#include "Strada/Math/AABB.h"
#include "Strada/Scene/SceneRendering.h"

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <vector>

namespace Strada
{
	class Scene;

	// How the scene view shows screen-space UI canvases: each top-level canvas's UI is drawn in its entity's XY plane, standing
	// on the entity (the middle of its bottom edge there, so a canvas made on the ground stands on it), CanvasUnitSize meters
	// per canvas unit (y up on the plane is up on the screen), laid out at its reference resolution (the screen it is designed
	// for), with its outline over the scene.
	namespace CanvasOverlay
	{
		inline constexpr float CanvasUnitSize = 0.01f;
		// The UI of a canvas that holds no selected entity is faint: it shows where the UI is without hiding the scene (and is
		// too faint to be picked: selecting the canvas or one of its elements in the hierarchy brings it forward).
		inline constexpr float UnselectedOpacity = 0.35f;
		// sRGB-encoded with straight alpha; selected elements in the color of the selection outline.
		inline constexpr glm::vec4 CanvasColor = glm::vec4(0.25f, 0.8f, 1.0f, 1.0f);
		inline constexpr glm::vec4 ElementColor = glm::vec4(0.25f, 0.8f, 1.0f, 0.55f);
		inline constexpr glm::vec4 SelectedColor = glm::vec4(1.0f, 0.55f, 0.1f, 1.0f);

		// Where the scene view draws a top-level canvas's UI (SceneRenderOptions::PlaceCanvas): at full opacity while the canvas
		// or one of its descendants is selected, UnselectedOpacity otherwise. Nothing for entities that are not canvases.
		std::optional<WorldCanvasPlacement> PlaceCanvas(Scene& scene, UUID canvas, std::span<UUID const> selection);
		// The outline of every top-level canvas; for canvases that hold a selected entity, the rectangles of their elements too,
		// those of the selected elements (and the canvas, when selected) highlighted. Drawn over the scene.
		void AppendCanvasOutlines(Scene& scene, std::span<UUID const> selection, std::vector<OverlayLine>& lines);
		// Where the scene view draws a canvas or an entity below one: the box around its rectangle in the canvas's plane (the
		// whole canvas for the canvas itself, and for elements that are hidden). Nothing for entities outside canvases. Focus
		// frames UI with it (ComputeEntityBounds).
		std::optional<AABB> GetUIBounds(Scene& scene, UUID entity);
		// Whether the entity is an element of a canvas's UI (a UIRect below a canvas). Its UIRect places it and its Transform
		// is not used, so the scene view gives it no transform gizmo and the inspector folds its Transform away.
		bool IsUIElement(Scene& scene, UUID entity);
	}
}
