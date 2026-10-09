#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Scene/Scene.h"

namespace Strada
{
	// The scene a new editor session starts with: a camera, a sun, a sky light, a floor and a few primitives. Only built-in
	// assets are used, so the scene can be saved anywhere.
	Ref<Scene> CreateDefaultScene();
}
