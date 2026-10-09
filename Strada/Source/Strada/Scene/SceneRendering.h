#pragma once

#include "Strada/Renderer/SceneRenderer.h"

namespace Strada
{
	class Scene;

	// Renders the scene from an explicit camera (the editor camera): submits visible meshes with their materials (per-slot
	// overrides first, then the mesh's defaults), lights and ambient light, then ends the frame.
	void RenderScene(Scene& scene, SceneRenderer& renderer, SceneRendererCamera const& camera);

	// Renders the scene from its primary camera entity. Returns false (rendering nothing) when the scene has none.
	bool RenderSceneFromPrimaryCamera(Scene& scene, SceneRenderer& renderer);
}
