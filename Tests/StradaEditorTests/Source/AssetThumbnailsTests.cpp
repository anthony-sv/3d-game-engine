#include "Editor/AssetThumbnails.h"
#include "GpuTestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/Renderer.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace Strada;

namespace
{
	// The average color of the image's central quarter.
	glm::vec3 CenterColor(Image const& image)
	{
		glm::vec3 sum(0.0f);
		uint32_t count = 0;
		for (uint32_t y = image.GetHeight() * 3 / 8; y < image.GetHeight() * 5 / 8; y++)
		{
			for (uint32_t x = image.GetWidth() * 3 / 8; x < image.GetWidth() * 5 / 8; x++)
			{
				uint8_t const* pixel = image.GetPixel(x, y);
				sum += glm::vec3(pixel[0], pixel[1], pixel[2]);
				count++;
			}
		}
		return sum / static_cast<float>(count);
	}
}

TEST_CASE("AssetThumbnails: framing puts the bounds' center in view with the bounding sphere inside the field of view")
{
	AABB bounds;
	bounds.Expand(glm::vec3(1.0f, 0.0f, -1.0f));
	bounds.Expand(glm::vec3(3.0f, 2.0f, 1.0f));
	float const fov = glm::radians(30.0f);
	ThumbnailCamera const camera = FrameThumbnailBounds(bounds, fov);
	CHECK(camera.Target.x == doctest::Approx(2.0f));
	CHECK(camera.Target.y == doctest::Approx(1.0f));
	CHECK(camera.Target.z == doctest::Approx(0.0f));
	float const radius = std::sqrt(3.0f);
	float const distance = glm::length(camera.Position - camera.Target);
	// The sphere's silhouette fits: sin(half angle it covers) = radius / distance <= sin(fov / 2).
	CHECK(radius / distance <= std::sin(fov * 0.5f));
	CHECK(distance < radius / std::sin(fov * 0.5f) * 1.2f);
	// Above and in front (+Z) of the bounds, looking down at them.
	CHECK(camera.Position.y > camera.Target.y);
	CHECK(camera.Position.z > camera.Target.z);
	CHECK(camera.NearPlane > 0.0f);
	CHECK(camera.NearPlane < distance - radius);

	// Invalid bounds (nothing to show) frame the origin.
	ThumbnailCamera const empty = FrameThumbnailBounds(AABB(), fov);
	CHECK(empty.Target == glm::vec3(0.0f));
	CHECK(std::isfinite(empty.Position.x));
}

TEST_CASE("AssetThumbnails: which assets have thumbnails, and none without a renderer")
{
	CHECK(AssetThumbnails::Supports(AssetType::Texture));
	CHECK(AssetThumbnails::Supports(AssetType::Material));
	CHECK(AssetThumbnails::Supports(AssetType::Mesh));
	CHECK(AssetThumbnails::Supports(AssetType::Prefab));
	CHECK(AssetThumbnails::Supports(AssetType::Environment));
	CHECK_FALSE(AssetThumbnails::Supports(AssetType::Scene));
	CHECK_FALSE(AssetThumbnails::Supports(AssetType::AudioClip));
	CHECK_FALSE(AssetThumbnails::Supports(AssetType::Font));

	AssetManager::Init();
	AssetThumbnails thumbnails;
	CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::DefaultMaterial)) == nullptr);
	thumbnails.Update();
	CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::DefaultMaterial)) == nullptr);
	AssetManager::Shutdown();
}

TEST_CASE("AssetThumbnails: material thumbnails show the material and follow its edits")
{
	ST_REQUIRE_GPU();
	AssetManager::Init();
	REQUIRE(Renderer::Init().IsOk());
	{
		MaterialData red;
		red.BaseColor = glm::vec4(0.9f, 0.05f, 0.05f, 1.0f);
		red.Roughness = 0.6f;
		Ref<MaterialAsset> const material = CreateRef<MaterialAsset>(red);
		AssetHandle const handle = AssetManager::AddMemoryAsset(material, "Red");

		AssetThumbnails thumbnails;
		// Rendered on the next update, one per update.
		CHECK(thumbnails.Get(handle) == nullptr);
		CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::SphereMesh)) == nullptr);
		thumbnails.Update();
		nvrhi::ITexture* const texture = thumbnails.Get(handle);
		REQUIRE(texture != nullptr);
		CHECK(texture->getDesc().width == AssetThumbnails::Size);
		CHECK(texture->getDesc().height == AssetThumbnails::Size);
		CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::SphereMesh)) == nullptr);
		thumbnails.Update();
		CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::SphereMesh)) != nullptr);

		Result<Image> const before = ReadbackTexture(texture);
		REQUIRE(before);
		glm::vec3 const redCenter = CenterColor(before.GetValue());
		CHECK(redCenter.r > redCenter.g * 2.0f);
		CHECK(redCenter.r > redCenter.b * 2.0f);

		// Editing the material renders it again.
		MaterialData blue = red;
		blue.BaseColor = glm::vec4(0.05f, 0.05f, 0.9f, 1.0f);
		material->SetData(blue);
		thumbnails.Get(handle);
		thumbnails.Update();
		nvrhi::ITexture* const updated = thumbnails.Get(handle);
		REQUIRE(updated != nullptr);
		Result<Image> const after = ReadbackTexture(updated);
		REQUIRE(after);
		glm::vec3 const blueCenter = CenterColor(after.GetValue());
		CHECK(blueCenter.b > blueCenter.r * 2.0f);

		// Textures are their own thumbnails.
		nvrhi::ITexture* const white = thumbnails.Get(GetBuiltInHandle(BuiltInAsset::WhiteTexture));
		REQUIRE(white != nullptr);
		CHECK(white != updated);

		AssetManager::RemoveMemoryAsset(handle);
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
	AssetManager::Shutdown();
}
