#pragma once

#include "Strada/Core/UUID.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace Strada
{
	class Scene;

	// Where a canvas's UI is drawn in the world (the editor's scene view shows canvases this way): laid out on a screen of
	// Size pixels, on the plane that Transform maps those pixels onto (x to the right, y downwards), with the alpha of its
	// colors multiplied by Opacity.
	struct WorldCanvasPlacement
	{
		glm::vec2 Size = glm::vec2(1.0f);
		glm::mat4 Transform = glm::mat4(1.0f);
		float Opacity = 1.0f;
	};

	// What a rendered frame shows besides the scene, mostly for editors, and how large it is seen (all optional).
	struct SceneRenderOptions
	{
		// Picking ID of an entity's meshes (1 to SceneRenderer::MaxPickingId), or 0 when it cannot be picked.
		std::function<uint32_t(UUID)> GetPickingId;
		// Meshes of selected entities get the selection outline.
		std::function<bool(UUID)> IsSelected;
		// Called after the scene was submitted, before the frame ends: overlays (SetOverlays, SubmitLine).
		std::function<void(SceneRenderer&)> SubmitOverlays;
		// Lines scripts drew with Scene::DrawDebugLine.
		bool DrawDebugLines = true;
		// Pixels per point of the display the image is shown on (2 on most high-density displays): the lines scripts draw
		// are as wide on every display, DebugLineWidth points with a dark outline of DebugLineOutlineWidth points.
		float DisplayScale = 1.0f;
		static constexpr float DebugLineWidth = 1.5f;
		static constexpr float DebugLineOutlineWidth = 0.75f;
		// The scene's meshes, lights, sky, particles and world-space sprites and text. Without them the frame shows the
		// screen-space sprites, text and UI alone over black, with no post effects: what scenes without a camera show.
		bool DrawWorld = true;
		// The screen-space UI of the scene's canvases, laid out on the renderer's viewport and drawn over the image.
		bool DrawUI = true;
		// Draws each top-level canvas's UI in the world instead (UILayout::GetCanvases), where this places it; canvases it
		// places nowhere are left out. Requires DrawUI.
		std::function<std::optional<WorldCanvasPlacement>(UUID canvas)> PlaceCanvas;
	};

	// Renders the scene from an explicit camera (the editor camera) with the scene's renderer settings as its post-process
	// volumes change them at the camera's position: submits visible meshes with their materials (per-slot overrides first,
	// then the mesh's defaults), lights and ambient light, sprites, text and particles, and the screen-space UI (buttons
	// in the color of their state), then ends the frame.
	void RenderScene(Scene& scene, SceneRenderer& renderer, SceneRendererCamera const& camera, SceneRenderOptions const& options = {});

	// Whether the scene shows something without a camera: a UI canvas, or screen-space sprites or text (menus, loading
	// screens).
	bool HasScreenSpaceContent(Scene& scene);

	// Renders the scene from its primary camera entity. Without one, a scene with screen-space content (HasScreenSpaceContent)
	// is drawn over black (SceneRenderOptions::DrawWorld off). Returns false, rendering nothing, when the scene has neither.
	bool RenderSceneFromPrimaryCamera(Scene& scene, SceneRenderer& renderer, SceneRenderOptions options = {});
}
