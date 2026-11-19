#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Renderer/SceneRendererSettings.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/PostProcessVolumes.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <random>

using namespace Strada;

// The player's graphics quality (Docs/Architecture.md §6.2, "Quality presets"): presets of the scene renderer's existing
// settings, applied over a view's resolved settings in the game's views alone.
namespace
{
	// Settings anywhere within the fields' ranges, each switch on or off.
	SceneRendererSettings MakeRandomSettings(std::mt19937& random)
	{
		std::uniform_int_distribution<int> coin(0, 1);
		std::uniform_int_distribution<uint32_t> mapSize(256, 8192);
		std::uniform_int_distribution<uint32_t> cascades(1, 4);
		std::uniform_real_distribution<float> distance(0.1f, 2000.0f);
		SceneRendererSettings settings;
		settings.Shadows = coin(random) == 1;
		settings.SoftShadows = coin(random) == 1;
		settings.ShadowDistance = coin(random) == 1 ? distance(random) : 0.1f;
		settings.CascadeCount = cascades(random);
		settings.ShadowMapSize = mapSize(random);
		settings.LocalShadowMapSize = mapSize(random);
		settings.AmbientOcclusion = coin(random) == 1;
		settings.Bloom = coin(random) == 1;
		settings.FXAA = coin(random) == 1;
		settings.Dithering = coin(random) == 1;
		settings.EV100 = distance(random) / 200.0f;
		settings.Saturation = distance(random) / 2000.0f;
		return settings;
	}
}

TEST_CASE("QualityPreset: High leaves the settings as they are, Medium and Low lower them as the table says")
{
	SceneRendererSettings const scene;
	CHECK(ApplyQualityPreset(scene, QualityPreset::High) == scene);

	SceneRendererSettings expected = scene;
	expected.ShadowMapSize = 1024;
	expected.LocalShadowMapSize = 512;
	expected.CascadeCount = 3;
	expected.ShadowDistance = 75.0f;
	CHECK(ApplyQualityPreset(scene, QualityPreset::Medium) == expected);

	expected.ShadowMapSize = 512;
	expected.LocalShadowMapSize = 256;
	expected.CascadeCount = 2;
	expected.ShadowDistance = 50.0f;
	expected.SoftShadows = false;
	expected.AmbientOcclusion = false;
	expected.Bloom = false;
	expected.FXAA = false;
	CHECK(ApplyQualityPreset(scene, QualityPreset::Low) == expected);

	// Values already below a preset's stay: no map below 256, no distance below the field's minimum, no cascade count
	// raised. Shadows stay on at every preset, and the scene's look (exposure, grading, lens) is the scene's.
	SceneRendererSettings small;
	small.ShadowMapSize = 256;
	small.LocalShadowMapSize = 300;
	small.ShadowDistance = 0.1f;
	small.CascadeCount = 1;
	SceneRendererSettings const low = ApplyQualityPreset(small, QualityPreset::Low);
	CHECK(low.ShadowMapSize == 256);
	CHECK(low.LocalShadowMapSize == 256);
	CHECK(low.ShadowDistance == 0.1f);
	CHECK(low.CascadeCount == 1);
	CHECK(low.Shadows);
	CHECK(low.EV100 == small.EV100);
	CHECK(low.Tonemapper == small.Tonemapper);
	CHECK(low.Dithering == small.Dithering);
	CHECK(low.ColorFilter == small.ColorFilter);
}

TEST_CASE("QualityPreset: over any settings, Medium and Low never raise a value or turn a switch on, and Low never above Medium")
{
	std::mt19937 random(4242);
	for (int round = 0; round < 1000; round++)
	{
		CAPTURE(round);
		SceneRendererSettings const settings = MakeRandomSettings(random);
		CHECK(ApplyQualityPreset(settings, QualityPreset::High) == settings);
		SceneRendererSettings const medium = ApplyQualityPreset(settings, QualityPreset::Medium);
		SceneRendererSettings const low = ApplyQualityPreset(settings, QualityPreset::Low);
		for (SceneRendererSettings const* preset : {&medium, &low})
		{
			CHECK(preset->ShadowMapSize <= settings.ShadowMapSize);
			CHECK(preset->LocalShadowMapSize <= settings.LocalShadowMapSize);
			CHECK(preset->ShadowMapSize >= std::min(settings.ShadowMapSize, QualityPresetMinShadowMapSize));
			CHECK(preset->CascadeCount <= settings.CascadeCount);
			CHECK(preset->CascadeCount >= 1);
			CHECK(preset->ShadowDistance <= settings.ShadowDistance);
			CHECK(preset->ShadowDistance >= 0.1f);
			CHECK((!preset->SoftShadows || settings.SoftShadows));
			CHECK((!preset->AmbientOcclusion || settings.AmbientOcclusion));
			CHECK((!preset->Bloom || settings.Bloom));
			CHECK((!preset->FXAA || settings.FXAA));
			CHECK(preset->Shadows == settings.Shadows);
			CHECK(preset->Dithering == settings.Dithering);
			CHECK(preset->EV100 == settings.EV100);
			CHECK(preset->Saturation == settings.Saturation);
		}
		CHECK(low.ShadowMapSize <= medium.ShadowMapSize);
		CHECK(low.CascadeCount <= medium.CascadeCount);
		CHECK(low.ShadowDistance <= medium.ShadowDistance);
	}
}

TEST_CASE("SceneRendering: a view's settings are the resolved volumes at its quality, and the scene's own at High")
{
	Scene scene("Volumes");
	scene.GetSettings().Renderer.Bloom = false;
	scene.GetSettings().Renderer.ShadowMapSize = 4096;
	// A global volume turns bloom on and darkens the image.
	PostProcessComponent volume;
	volume.OverrideBloom = true;
	volume.Bloom = true;
	volume.OverrideEV100 = true;
	volume.EV100 = 2.0f;
	scene.CreateEntity("Volume").AddComponent<PostProcessComponent>(volume);
	glm::vec3 const camera(1.0f, 2.0f, 3.0f);
	SceneRendererSettings const resolved = PostProcessVolumes::Resolve(scene, camera);
	REQUIRE(resolved.Bloom);

	// Options that set no quality (the Scene View, thumbnails) render exactly what the volumes resolve.
	CHECK(ResolveViewSettings(scene, camera, SceneRenderOptions{}) == resolved);
	SceneRenderOptions options;
	options.Quality = QualityPreset::High;
	CHECK(ResolveViewSettings(scene, camera, options) == resolved);

	// The game's views lower the resolved settings: the volume's bloom goes at Low, its exposure stays.
	options.Quality = QualityPreset::Low;
	SceneRendererSettings const low = ResolveViewSettings(scene, camera, options);
	CHECK(low == ApplyQualityPreset(resolved, QualityPreset::Low));
	CHECK_FALSE(low.Bloom);
	CHECK(low.EV100 == resolved.EV100);
	CHECK(low.ShadowMapSize == 1024);
	options.Quality = QualityPreset::Medium;
	CHECK(ResolveViewSettings(scene, camera, options).ShadowMapSize == 2048);

	// Screen-space content alone keeps its own settings at every preset.
	options.DrawWorld = false;
	SceneRendererSettings const screen = ResolveViewSettings(scene, camera, options);
	options.Quality = QualityPreset::High;
	CHECK(ResolveViewSettings(scene, camera, options) == screen);
	CHECK_FALSE(screen.Shadows);
	CHECK_FALSE(screen.Bloom);
}
