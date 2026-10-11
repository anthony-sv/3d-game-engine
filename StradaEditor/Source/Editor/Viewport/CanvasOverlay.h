#pragma once

#include "Editor/Viewport/ViewportOverlays.h"

#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace Strada
{
	class Scene;

	// The scene view's outline of screen-space UI canvases: a canvas is drawn as a rectangle in its entity's XY plane, centered
	// on the entity, CanvasUnitSize meters per canvas unit (y up on the plane is up on the screen), laid out at its reference
	// resolution (the screen it is designed for).
	namespace CanvasOverlay
	{
		inline constexpr float CanvasUnitSize = 0.01f;
		// sRGB-encoded with straight alpha; selected elements in the color of the selection outline.
		inline constexpr glm::vec4 CanvasColor = glm::vec4(0.25f, 0.8f, 1.0f, 1.0f);
		inline constexpr glm::vec4 ElementColor = glm::vec4(0.25f, 0.8f, 1.0f, 0.55f);
		inline constexpr glm::vec4 SelectedColor = glm::vec4(1.0f, 0.55f, 0.1f, 1.0f);

		// For every canvas that holds a selected entity (the canvas itself or one of its UI elements): the canvas's outline,
		// the rectangles of its elements, and those of the selected elements highlighted. Drawn over the scene.
		void AppendSelectedCanvases(Scene& scene, std::span<UUID const> selection, std::vector<OverlayLine>& lines);
		// Whether the entity is an element of a canvas's UI (a UIRect below a canvas). Its UIRect places it and its Transform
		// is not used, so the scene view gives it no transform gizmo.
		bool IsUIElement(Scene& scene, UUID entity);
	}
}
