#include "Strada/Math/Math.h"
#include "Strada/Renderer/SceneRendererSettings.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/ComponentSerialization.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/PostProcessVolumes.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <string_view>

using namespace Strada;

namespace
{
	Json SettingsToJson(SceneRendererSettings const& settings)
	{
		return SerializeFields<StructTraits<SceneRendererSettings>>(settings);
	}

	// A value of the field's type that differs from its default and lies in its range.
	Json MakeOtherValue(FieldDescriptor const& field)
	{
		switch (field.Kind)
		{
			case FieldKind::Bool:
				return !field.Default.get<bool>();
			case FieldKind::Float:
				return field.Default.get<double>() != field.Hints.Max ? field.Hints.Max : field.Hints.Min;
			case FieldKind::Enum:
				for (std::string_view const value : field.EnumValues)
				{
					if (value != field.Default.get<std::string>())
					{
						return std::string(value);
					}
				}
				break;
			case FieldKind::Vec3:
				return Json::array({0.5, 0.25, 1.5});
			default:
				break;
		}
		FAIL("unexpected kind of field " << std::string(field.Name));
		return Json();
	}

	Entity AddVolume(Scene& scene, std::string const& name, PostProcessComponent const& volume)
	{
		Entity entity = scene.CreateEntity(name);
		entity.AddComponent<PostProcessComponent>(volume);
		return entity;
	}

	PostProcessComponent MakeSaturationVolume(float saturation, float priority = 0.0f)
	{
		PostProcessComponent volume;
		volume.OverrideSaturation = true;
		volume.Saturation = saturation;
		volume.Priority = priority;
		return volume;
	}
}

TEST_CASE("PostProcess: a volume changes exactly the settings it overrides")
{
	ComponentInfo const& info = ComponentRegistry::Get<PostProcessComponent>();
	Json const defaults = SettingsToJson(SceneRendererSettings());

	// Without overrides nothing changes, whatever the values.
	PostProcessComponent untouched;
	untouched.Saturation = 0.0f;
	untouched.Tonemapper = TonemapOperator::None;
	untouched.Bloom = false;
	SceneRendererSettings settings;
	PostProcessVolumes::Apply(settings, untouched, 1.0f);
	CHECK(settings == SceneRendererSettings());

	// Every switched field overrides the renderer setting of the same name, and only that one.
	size_t switched = 0;
	for (FieldDescriptor const& field : info.Fields)
	{
		if (field.Hints.EnabledBy.empty())
		{
			continue;
		}
		CAPTURE(field.Name);
		switched++;
		std::string const name(field.Name);
		Json const value = MakeOtherValue(field);
		PostProcessComponent volume;
		REQUIRE(DeserializeComponent(Json::object({{std::string(field.Hints.EnabledBy), true}, {name, value}}), volume,
		                             DeserializationContext{})
		            .IsOk());

		SceneRendererSettings result;
		PostProcessVolumes::Apply(result, volume, 1.0f);
		Json const resolved = SettingsToJson(result);
		REQUIRE(resolved.contains(name));
		CHECK(resolved[name] == value);
		for (auto const& item : defaults.items())
		{
			if (item.key() != name)
			{
				CAPTURE(item.key());
				CHECK(resolved[item.key()] == item.value());
			}
		}
	}
	CHECK(switched == 15);
}

TEST_CASE("PostProcess: numbers blend by the volume's blend, switches change from one half")
{
	PostProcessComponent volume = MakeSaturationVolume(0.0f);
	volume.OverrideEV100 = true;
	volume.EV100 = 4.0f;
	volume.OverrideColorFilter = true;
	volume.ColorFilter = {0.0f, 1.0f, 2.0f};
	volume.OverrideTonemapper = true;
	volume.Tonemapper = TonemapOperator::AgX;
	volume.OverrideBloom = true;
	volume.Bloom = false;
	volume.OverrideAmbientOcclusion = true;
	volume.AmbientOcclusion = false;

	SceneRendererSettings quarter;
	PostProcessVolumes::Apply(quarter, volume, 0.25f);
	CHECK(quarter.Saturation == doctest::Approx(0.75f));
	CHECK(quarter.EV100 == doctest::Approx(1.0f));
	CHECK(quarter.ColorFilter.r == doctest::Approx(0.75f));
	CHECK(quarter.ColorFilter.g == doctest::Approx(1.0f));
	CHECK(quarter.ColorFilter.b == doctest::Approx(1.25f));
	CHECK(quarter.Tonemapper == TonemapOperator::ACES);
	CHECK(quarter.Bloom);
	CHECK(quarter.AmbientOcclusion);

	SceneRendererSettings justBelowHalf;
	PostProcessVolumes::Apply(justBelowHalf, volume, std::nextafter(PostProcessVolumes::SwitchBlend, 0.0f));
	CHECK(justBelowHalf.Tonemapper == TonemapOperator::ACES);
	SceneRendererSettings half;
	PostProcessVolumes::Apply(half, volume, PostProcessVolumes::SwitchBlend);
	CHECK(half.Saturation == doctest::Approx(0.5f));
	CHECK(half.Tonemapper == TonemapOperator::AgX);
	CHECK_FALSE(half.Bloom);
	CHECK_FALSE(half.AmbientOcclusion);

	// The whole volume at 1 (and beyond, which counts as 1), nothing at 0 or less.
	for (float const full : {1.0f, 3.0f})
	{
		SceneRendererSettings result;
		PostProcessVolumes::Apply(result, volume, full);
		CHECK(result.Saturation == 0.0f);
		CHECK(result.EV100 == 4.0f);
		CHECK(result.ColorFilter == glm::vec3(0.0f, 1.0f, 2.0f));
	}
	for (float const none : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()})
	{
		SceneRendererSettings result;
		PostProcessVolumes::Apply(result, volume, none);
		CHECK(result == SceneRendererSettings());
	}
}

TEST_CASE("PostProcess: a local volume's box follows the entity's transform")
{
	// Moved to (10, 0, 0), turned 90 degrees around Y (local X points to -Z) and sized 4 x 2 x 6.
	glm::mat4 const world =
		Math::ComposeTransform({10.0f, 0.0f, 0.0f}, Math::EulerDegreesToQuaternion({0.0f, 90.0f, 0.0f}), {4.0f, 2.0f, 6.0f});
	PostProcessVolumes::Box const box = PostProcessVolumes::GetBox(world);
	CHECK(box.Center == glm::vec3(10.0f, 0.0f, 0.0f));
	CHECK(box.HalfExtents.x == doctest::Approx(2.0f));
	CHECK(box.HalfExtents.y == doctest::Approx(1.0f));
	CHECK(box.HalfExtents.z == doctest::Approx(3.0f));
	CHECK(box.Axes[0].z == doctest::Approx(-1.0f));

	CHECK(PostProcessVolumes::GetDistance(box, {10.0f, 0.0f, 0.0f}) == 0.0f);
	// Inside: 1.9 of 2 along local X (world -Z), 2.9 of 3 along local Z (world X).
	CHECK(PostProcessVolumes::GetDistance(box, {12.9f, 0.9f, -1.9f}) == 0.0f);
	// Outside one face, then outside an edge.
	CHECK(PostProcessVolumes::GetDistance(box, {10.0f, 0.0f, -3.0f}) == doctest::Approx(1.0f));
	CHECK(PostProcessVolumes::GetDistance(box, {10.0f, 4.0f, 6.0f}) == doctest::Approx(5.0f));

	// An axis scaled to nothing makes the box flat: the whole offset along it is outside.
	PostProcessVolumes::Box const flat =
		PostProcessVolumes::GetBox(Math::ComposeTransform(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {2.0f, 0.0f, 2.0f}));
	CHECK(flat.HalfExtents.y == 0.0f);
	CHECK(std::abs(flat.Axes[1].y) == doctest::Approx(1.0f));
	CHECK(PostProcessVolumes::GetDistance(flat, {0.5f, 0.0f, 0.5f}) == 0.0f);
	CHECK(PostProcessVolumes::GetDistance(flat, {0.5f, 0.75f, 0.5f}) == doctest::Approx(0.75f));
	// A point: every offset is outside.
	PostProcessVolumes::Box const point = PostProcessVolumes::GetBox(glm::mat4(0.0f));
	CHECK(PostProcessVolumes::GetDistance(point, {3.0f, 4.0f, 0.0f}) == doctest::Approx(5.0f));
	// Huge scales do not overflow while the box is measured.
	PostProcessVolumes::Box const huge = PostProcessVolumes::GetBox(glm::scale(glm::mat4(1.0f), glm::vec3(1.0e30f)));
	CHECK(huge.Axes[0].x == doctest::Approx(1.0f));
	CHECK(PostProcessVolumes::GetDistance(huge, {1.0e20f, 0.0f, 0.0f}) == 0.0f);
}

TEST_CASE("PostProcess: global volumes apply everywhere, local ones fade out over their blend distance")
{
	PostProcessComponent volume = MakeSaturationVolume(0.0f);
	glm::mat4 const world = glm::scale(glm::mat4(1.0f), glm::vec3(4.0f));
	glm::vec3 const far(100.0f, 0.0f, 0.0f);

	CHECK(PostProcessVolumes::GetBlend(volume, world, far) == 1.0f);
	volume.Weight = 0.5f;
	CHECK(PostProcessVolumes::GetBlend(volume, world, far) == 0.5f);

	volume.Global = false;
	volume.Weight = 1.0f;
	volume.BlendDistance = 2.0f;
	CHECK(PostProcessVolumes::GetBlend(volume, world, {1.0f, 1.0f, 1.0f}) == 1.0f);
	CHECK(PostProcessVolumes::GetBlend(volume, world, {2.0f, 0.0f, 0.0f}) == 1.0f);
	CHECK(PostProcessVolumes::GetBlend(volume, world, {3.0f, 0.0f, 0.0f}) == doctest::Approx(0.5f));
	CHECK(PostProcessVolumes::GetBlend(volume, world, {3.5f, 0.0f, 0.0f}) == doctest::Approx(0.25f));
	CHECK(PostProcessVolumes::GetBlend(volume, world, {4.0f, 0.0f, 0.0f}) == 0.0f);
	CHECK(PostProcessVolumes::GetBlend(volume, world, far) == 0.0f);
	volume.Weight = 0.5f;
	CHECK(PostProcessVolumes::GetBlend(volume, world, {3.0f, 0.0f, 0.0f}) == doctest::Approx(0.25f));

	// Without a blend distance the box's edge is sharp.
	volume.Weight = 1.0f;
	volume.BlendDistance = 0.0f;
	CHECK(PostProcessVolumes::GetBlend(volume, world, {2.0f, 0.0f, 0.0f}) == 1.0f);
	CHECK(PostProcessVolumes::GetBlend(volume, world, {2.001f, 0.0f, 0.0f}) == 0.0f);

	// Values scripts can set outside the ranges: weights count as 0 to 1, negative distances as 0.
	volume.Weight = 7.0f;
	volume.BlendDistance = -3.0f;
	CHECK(PostProcessVolumes::GetBlend(volume, world, glm::vec3(0.0f)) == 1.0f);
	CHECK(PostProcessVolumes::GetBlend(volume, world, {2.5f, 0.0f, 0.0f}) == 0.0f);
	volume.Weight = -1.0f;
	CHECK(PostProcessVolumes::GetBlend(volume, world, glm::vec3(0.0f)) == 0.0f);
}

TEST_CASE("PostProcess: volumes apply by priority, and in hierarchy order between equal priorities")
{
	Scene scene("Volumes");
	scene.GetSettings().Renderer.Saturation = 1.0f;
	glm::vec3 const camera(0.0f);

	// No volumes: the scene's settings as they are.
	CHECK(PostProcessVolumes::Resolve(scene, camera) == scene.GetSettings().Renderer);

	Entity gray = AddVolume(scene, "Gray", MakeSaturationVolume(0.2f, 1.0f));
	Entity vivid = AddVolume(scene, "Vivid", MakeSaturationVolume(1.8f, 0.0f));
	// The higher priority applies last, over the lower one.
	CHECK(PostProcessVolumes::Resolve(scene, camera).Saturation == doctest::Approx(0.2f));
	vivid.GetComponent<PostProcessComponent>().Priority = 2.0f;
	CHECK(PostProcessVolumes::Resolve(scene, camera).Saturation == doctest::Approx(1.8f));

	// Equal priorities: the later entity in the hierarchy applies last.
	vivid.GetComponent<PostProcessComponent>().Priority = 1.0f;
	CHECK(PostProcessVolumes::Resolve(scene, camera).Saturation == doctest::Approx(1.8f));
	scene.SetSiblingIndex(vivid, 0);
	CHECK(PostProcessVolumes::Resolve(scene, camera).Saturation == doctest::Approx(0.2f));
	// Children come after their parents.
	REQUIRE(scene.SetParent(gray, vivid).IsOk());
	CHECK(PostProcessVolumes::Resolve(scene, camera).Saturation == doctest::Approx(0.2f));
	REQUIRE(scene.SetParent(vivid, gray).IsError());
	REQUIRE(scene.SetParent(gray, Entity()).IsOk());

	// A partial volume on top moves halfway from what the volumes below made.
	gray.GetComponent<PostProcessComponent>().Priority = 5.0f;
	gray.GetComponent<PostProcessComponent>().Weight = 0.5f;
	CHECK(PostProcessVolumes::Resolve(scene, camera).Saturation == doctest::Approx(1.0f));

	// A local volume counts only near the camera; the scene's own settings never change.
	PostProcessComponent& local = vivid.GetComponent<PostProcessComponent>();
	local.Global = false;
	local.BlendDistance = 1.0f;
	vivid.GetComponent<TransformComponent>().Translation = {10.0f, 0.0f, 0.0f};
	vivid.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
	CHECK(PostProcessVolumes::Resolve(scene, camera).Saturation == doctest::Approx(0.6f));
	CHECK(PostProcessVolumes::Resolve(scene, {10.0f, 0.5f, 0.0f}).Saturation == doctest::Approx(1.0f));
	CHECK(PostProcessVolumes::Resolve(scene, {11.5f, 0.0f, 0.0f}).Saturation == doctest::Approx(0.8f));
	CHECK(scene.GetSettings().Renderer.Saturation == 1.0f);

	// Volumes of children are placed by their parents' transforms.
	Entity holder = scene.CreateEntity("Holder");
	holder.GetComponent<TransformComponent>().Translation = {-10.0f, 0.0f, 0.0f};
	REQUIRE(scene.SetParent(vivid, holder, false).IsOk());
	CHECK(PostProcessVolumes::Resolve(scene, {0.0f, 0.0f, 0.0f}).Saturation == doctest::Approx(1.0f));
}

TEST_CASE("PostProcess: values from scripts outside the field ranges act as the nearest bound")
{
	PostProcessComponent volume;
	volume.OverrideSaturation = true;
	volume.Saturation = 50.0f;
	volume.OverrideEV100 = true;
	volume.EV100 = -1000.0f;
	volume.OverrideVignetteSmoothness = true;
	volume.VignetteSmoothness = 0.0f;
	volume.OverrideColorFilter = true;
	volume.ColorFilter = {-1.0f, 0.5f, 9.0f};
	SceneRendererSettings settings;
	PostProcessVolumes::Apply(settings, volume, 0.5f);
	CHECK(settings.Saturation == doctest::Approx(1.5f));
	CHECK(settings.EV100 == doctest::Approx(-10.0f));
	CHECK(settings.VignetteSmoothness == doctest::Approx(0.23f));
	CHECK(settings.ColorFilter.r == doctest::Approx(0.5f));
	CHECK(settings.ColorFilter.b == doctest::Approx(1.5f));

	// Numbers that are not finite cannot come from files, the editor or scripts; native code setting them changes nothing,
	// and priorities that are not numbers still sort.
	float const nan = std::numeric_limits<float>::quiet_NaN();
	PostProcessComponent broken;
	broken.OverrideTint = true;
	broken.Tint = nan;
	broken.OverrideColorFilter = true;
	broken.ColorFilter = {nan, 1.0f, 1.0f};
	broken.Priority = nan;
	SceneRendererSettings unchanged;
	PostProcessVolumes::Apply(unchanged, broken, 1.0f);
	CHECK(unchanged == SceneRendererSettings());

	Scene scene("Broken");
	AddVolume(scene, "Broken", broken);
	AddVolume(scene, "Gray", MakeSaturationVolume(0.0f, -5.0f));
	AddVolume(scene, "Also broken", broken);
	CHECK(PostProcessVolumes::Resolve(scene, glm::vec3(0.0f)).Saturation == 0.0f);
}
