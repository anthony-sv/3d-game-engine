#pragma once

#include "GpuTestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Math/Math.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/Renderer.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace Strada::Testing
{
	// Perspective camera (50 degree vertical field of view, reversed Z) for a width x height image.
	inline SceneRendererCamera MakeTestCamera(glm::vec3 const& position, glm::vec3 const& target, uint32_t width, uint32_t height)
	{
		SceneRendererCamera camera;
		camera.View = glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
		camera.Projection = Math::PerspectiveReversedZ(glm::radians(50.0f), static_cast<float>(width) / static_cast<float>(height), 0.1f);
		camera.Position = position;
		return camera;
	}

	// An entity with a built-in mesh (material is the override of its first slot when valid).
	inline Entity AddTestMesh(Scene& scene, char const* name, BuiltInAsset mesh, AssetHandle material, glm::vec3 const& position,
	                          glm::vec3 const& scale = glm::vec3(1.0f))
	{
		Entity entity = scene.CreateEntity(name);
		MeshComponent& component = entity.AddComponent<MeshComponent>();
		component.Mesh = GetBuiltInHandle(mesh);
		if (material.IsValid())
		{
			component.Materials = {material};
		}
		TransformComponent& transform = entity.GetComponent<TransformComponent>();
		transform.Translation = position;
		transform.Scale = scale;
		return entity;
	}

	// Renders the scene and reads back the final image.
	inline Image RenderToImage(Scene& scene, SceneRendererCamera const& camera, SceneRenderer& renderer)
	{
		RenderScene(scene, renderer, camera);
		Result<Image> image = ReadbackTexture(renderer.GetFinalImage());
		REQUIRE(image.IsOk());
		return std::move(image.GetValue());
	}

	// Renderer and AssetManager on top of a GpuTestScope, for the duration of a test.
	class RenderTestScope
	{
	public:
		explicit RenderTestScope(GpuTestScope const& gpu)
		{
			if (!gpu.IsAvailable())
			{
				return;
			}
			AssetManager::Init();
			REQUIRE(Renderer::Init().IsOk());
			m_Active = true;
		}

		~RenderTestScope()
		{
			if (m_Active)
			{
				Renderer::Shutdown();
				AssetManager::Shutdown();
			}
		}

		RenderTestScope(RenderTestScope const&) = delete;
		RenderTestScope& operator=(RenderTestScope const&) = delete;

	private:
		bool m_Active = false;
	};

	struct ImageDifference
	{
		// Largest per-channel difference (0-255).
		int MaxDifference = 0;
		double MeanDifference = 0.0;
		// Fraction of pixels whose largest channel difference exceeds the outlier threshold.
		double OutlierFraction = 0.0;
	};

	inline ImageDifference CompareImages(Image const& a, Image const& b, int outlierThreshold)
	{
		ImageDifference difference;
		REQUIRE(a.GetWidth() == b.GetWidth());
		REQUIRE(a.GetHeight() == b.GetHeight());
		REQUIRE(a.GetChannels() == b.GetChannels());
		size_t outliers = 0;
		double sum = 0.0;
		size_t const pixelCount = static_cast<size_t>(a.GetWidth()) * a.GetHeight();
		std::vector<uint8_t> const& pixelsA = a.GetPixels();
		std::vector<uint8_t> const& pixelsB = b.GetPixels();
		for (size_t pixel = 0; pixel < pixelCount; pixel++)
		{
			int largest = 0;
			for (uint32_t channel = 0; channel < a.GetChannels(); channel++)
			{
				size_t const index = pixel * a.GetChannels() + channel;
				int const delta = std::abs(static_cast<int>(pixelsA[index]) - static_cast<int>(pixelsB[index]));
				largest = std::max(largest, delta);
				sum += delta;
			}
			difference.MaxDifference = std::max(difference.MaxDifference, largest);
			outliers += largest > outlierThreshold ? 1 : 0;
		}
		difference.MeanDifference = sum / static_cast<double>(pixelCount * a.GetChannels());
		difference.OutlierFraction = static_cast<double>(outliers) / static_cast<double>(pixelCount);
		return difference;
	}

	// Compares a rendered image against Tests/Data/Golden/<name>.png. GPUs (and Mesa's lavapipe in CI) differ slightly in
	// floating-point evaluation and texture filtering, so the metric tolerates small noise: the mean channel difference
	// must stay below 1.0 and at most 0.5% of the pixels may differ by more than 16 levels. STRADA_UPDATE_GOLDEN=1
	// writes the rendered image as the new reference instead; failures write <name>.actual.png next to the test binary.
	inline void CheckGoldenImage(std::string const& name, Image const& image)
	{
		std::filesystem::path const goldenPath = std::filesystem::path(STRADA_TEST_DATA_DIR) / "Golden" / (name + ".png");
		if (Platform::ReadEnvironmentVariable("STRADA_UPDATE_GOLDEN").value_or("") == "1")
		{
			REQUIRE(image.WritePNG(goldenPath).IsOk());
			MESSAGE("Updated golden image " << FileSystem::PathToUtf8(goldenPath));
			return;
		}

		Result<Image> golden = Image::LoadFromFile(goldenPath);
		REQUIRE_MESSAGE(golden.IsOk(), "Missing golden image (run with STRADA_UPDATE_GOLDEN=1): " << golden.GetError());
		ImageDifference const difference = CompareImages(image, golden.GetValue(), 16);
		bool const matches = difference.MeanDifference < 1.0 && difference.OutlierFraction <= 0.005;
		if (!matches)
		{
			std::filesystem::path const actualPath = FileSystem::GetExecutableDirectory() / (name + ".actual.png");
			(void)image.WritePNG(actualPath);
			MESSAGE("Golden image mismatch; rendered image written to " << FileSystem::PathToUtf8(actualPath));
		}
		CHECK_MESSAGE(matches, name << ": mean difference " << difference.MeanDifference << ", outliers "
		                            << difference.OutlierFraction * 100.0 << "%, max " << difference.MaxDifference);
	}
}
