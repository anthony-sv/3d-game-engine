#pragma once

#include "Strada/Core/UUID.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <cstdint>
#include <functional>

namespace Strada
{
	class Scene;

	// Editor additions to a rendered frame (all optional).
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
		// The screen-space UI of the scene's canvases (laid out on the renderer's viewport). Editor scene views leave it out.
		bool DrawUI = true;
	};

	// Renders the scene from an explicit camera (the editor camera): submits visible meshes with their materials (per-slot
	// overrides first, then the mesh's defaults), lights and ambient light, sprites, text and particles, and the
	// screen-space UI (buttons in the color of their state), then ends the frame.
	void RenderScene(Scene& scene, SceneRenderer& renderer, SceneRendererCamera const& camera, SceneRenderOptions const& options = {});

	// Renders the scene from its primary camera entity. Returns false (rendering nothing) when the scene has none.
	bool RenderSceneFromPrimaryCamera(Scene& scene, SceneRenderer& renderer);
}
