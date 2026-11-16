#include "Core/ProfilerTestUtilities.h"
#include "Renderer/RenderTestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr uint32_t Width = 1920;
	constexpr uint32_t Height = 1080;
	constexpr int Roots = 100;
	constexpr int ChildrenPerRoot = 119;
	constexpr int MeshesPerRoot = 100;

	// A mesh of two submeshes with a material slot each: a cube, and a smaller one on top of it.
	AssetHandle AddTwoPartMesh()
	{
		Ref<MeshSource> const cube = AssetManager::GetAsset<MeshSource>(GetBuiltInHandle(BuiltInAsset::CubeMesh));
		REQUIRE(cube);
		std::vector<Vertex> vertices = cube->GetVertices();
		std::vector<uint32_t> indices = cube->GetIndices();
		uint32_t const vertexCount = static_cast<uint32_t>(vertices.size());
		uint32_t const indexCount = static_cast<uint32_t>(indices.size());
		for (uint32_t index = 0; index < vertexCount; index++)
		{
			Vertex top = vertices[index];
			top.Position = top.Position * 0.5f + glm::vec3(0.0f, 0.75f, 0.0f);
			vertices.push_back(top);
		}
		for (uint32_t index = 0; index < indexCount; index++)
		{
			indices.push_back(indices[index]);
		}
		std::vector<Submesh> submeshes(2);
		submeshes[0].VertexCount = vertexCount;
		submeshes[0].IndexCount = indexCount;
		submeshes[1].BaseVertex = vertexCount;
		submeshes[1].VertexCount = vertexCount;
		submeshes[1].BaseIndex = indexCount;
		submeshes[1].IndexCount = indexCount;
		submeshes[1].MaterialIndex = 1;
		Result<Ref<MeshSource>> mesh =
			MeshSource::Create(std::move(vertices), std::move(indices), std::move(submeshes), std::vector<AssetHandle>(2));
		REQUIRE(mesh.IsOk());
		return AssetManager::AddMemoryAsset(mesh.GetValue(), "Two parts");
	}

	// 12,000 entities: 100 group roots on a grid of 12 meters, each with 119 children, 100 of which carry one of four meshes
	// (one of two submeshes) with one of three materials (opaque, masked, double-sided): 10,000 instances. A sun casts
	// shadows in four cascades.
	struct BenchmarkScene
	{
		Scene World{"Instancing benchmark"};
		std::vector<Entity> RootEntities;
		std::vector<Entity> MeshChildren;

		BenchmarkScene()
		{
			std::vector<AssetHandle> materials;
			for (int kind = 0; kind < 3; kind++)
			{
				MaterialData data;
				data.BaseColor = {0.3f + 0.2f * static_cast<float>(kind), 0.5f, 0.6f, kind == 1 ? 0.8f : 1.0f};
				data.Roughness = 0.7f;
				data.AlphaMode = kind == 1 ? MaterialAlphaMode::Mask : MaterialAlphaMode::Opaque;
				data.DoubleSided = kind == 2;
				materials.push_back(AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(data), "Benchmark material"));
			}
			std::vector<AssetHandle> const meshes = {GetBuiltInHandle(BuiltInAsset::CubeMesh), GetBuiltInHandle(BuiltInAsset::CylinderMesh),
			                                         GetBuiltInHandle(BuiltInAsset::ConeMesh), AddTwoPartMesh()};
			for (int root = 0; root < Roots; root++)
			{
				Entity group = World.CreateEntity("Group");
				group.GetComponent<TransformComponent>().Translation = {static_cast<float>(root % 10) * 12.0f - 54.0f, 0.0f,
				                                                        static_cast<float>(root / 10) * 12.0f - 54.0f};
				RootEntities.push_back(group);
				for (int child = 0; child < ChildrenPerRoot; child++)
				{
					Entity entity = World.CreateEntity("Child", group);
					TransformComponent& transform = entity.GetComponent<TransformComponent>();
					transform.Translation = {static_cast<float>(child % 10) * 1.1f - 5.0f, 0.5f,
					                         static_cast<float>(child / 10) * 1.1f - 5.0f};
					transform.Scale = glm::vec3(0.5f);
					if (child >= MeshesPerRoot)
					{
						continue;
					}
					MeshComponent& mesh = entity.AddComponent<MeshComponent>();
					size_t const kind = static_cast<size_t>(child) % meshes.size();
					mesh.Mesh = meshes[kind];
					mesh.Materials = {materials[static_cast<size_t>(child) % 3], materials[static_cast<size_t>(child + 1) % 3]};
					MeshChildren.push_back(entity);
				}
			}
			Entity sun = World.CreateEntity("Sun");
			sun.GetComponent<TransformComponent>().SetRotationEuler({-50.0f, -30.0f, 0.0f});
			sun.AddComponent<DirectionalLightComponent>().CastShadows = true;
		}
	};

	double FindZone(ProfilerMedians const& medians, std::string_view name)
	{
		for (ProfilerZoneMedian const& zone : medians.Zones)
		{
			if (zone.Name == name)
			{
				return zone.MillisecondsPerFrame;
			}
		}
		return 0.0;
	}

	double FindCounter(ProfilerMedians const& medians, std::string_view name)
	{
		for (ProfilerCounterMedian const& counter : medians.Counters)
		{
			if (counter.Name == name)
			{
				return counter.Value;
			}
		}
		return 0.0;
	}
}

TEST_CASE("Instancing: 10,000 instances benchmark" * doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_INSTANCING_BENCHMARK")))
{
	// A measurement, never asserted on timings: run with STRADA_INSTANCING_BENCHMARK=1 in a Release build.
	constexpr int Frames = 300;
	constexpr int WarmUpFrames = 600;
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Testing::ProfilerScope const profiler;
	BenchmarkScene bench;
	REQUIRE(bench.MeshChildren.size() == 10000);
	SceneRendererCamera const camera = Testing::MakeTestCamera({0.0f, 30.0f, 75.0f}, {0.0f, 0.0f, -5.0f}, Width, Height);

	struct Scenario
	{
		char const* Name = nullptr;
		std::function<void(int)> Move;
	};
	std::vector<Scenario> const scenarios = {
		{"static", [](int) {}},
		{"1,000 children moved",
	     [&bench](int step)
	     {
			 for (size_t index = 0; index < 1000; index++)
			 {
				 size_t const child = (static_cast<size_t>(step) * 1000 + index) % bench.MeshChildren.size();
				 bench.MeshChildren[child].GetComponent<TransformComponent>().Translation.y += step % 2 == 0 ? 0.01f : -0.01f;
			 }
		 }},
		{"every root moved",
	     [&bench](int step)
	     {
			 for (Entity& root : bench.RootEntities)
			 {
				 root.GetComponent<TransformComponent>().Translation.y += step % 2 == 0 ? 0.01f : -0.01f;
			 }
		 }},
	};

	for (bool const entityIds : {false, true})
	{
		SceneRenderer renderer("Bench");
		renderer.SetViewportSize(Width, Height);
		renderer.SetEntityIdsEnabled(entityIds);
		// The first frames upload the meshes and make the pipelines and buffers; the next ones bring the processor and the
		// GPU to the clocks they keep while a game runs, so that the scenarios compare at a steady state.
		for (int frame = 0; frame < WarmUpFrames; frame++)
		{
			bench.World.BeginTransformFrame();
			RenderScene(bench.World, renderer, camera);
			GraphicsDevice::EndFrame();
		}
		// Every scenario twice, the second round after the first: a round that reads slower than the one before it tells a
		// machine that changed its clocks, not a cost.
		for (int round = 1; round <= 2; round++)
		{
			for (Scenario const& scenario : scenarios)
			{
				REQUIRE(Profiler::Start().IsOk());
				for (int frame = 0; frame < Frames; frame++)
				{
					Profiler::BeginFrame();
					bench.World.BeginTransformFrame();
					scenario.Move(frame);
					RenderScene(bench.World, renderer, camera);
					Profiler::EndFrame();
					// Frames in flight as the player has them, and the device's resources released as it releases them.
					GraphicsDevice::EndFrame();
				}
				Profiler::Stop();
				ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());

				SceneRendererStatistics const& statistics = renderer.GetStatistics();
				CHECK(renderer.GetFrameInstances().size() == 10000);
				CHECK(statistics.DrawCalls <= 400);
				CHECK(statistics.Instances >= 10000);
				double const submit = FindZone(medians, "Renderer.Submit");
				double const prepare = FindZone(medians, "Renderer.Prepare");
				std::string gpu;
				for (ProfilerMedian const& pass : medians.GpuPasses)
				{
					gpu += " " + pass.Name.substr(pass.Name.find('/') + 1) + " " + std::to_string(pass.Milliseconds);
				}
				MESSAGE("Round " << round << ", entity IDs " << std::string(entityIds ? "on" : "off") << ", " << std::string(scenario.Name)
				                 << ": Scene.UpdateTransforms " << FindZone(medians, "Scene.UpdateTransforms") << " ms, Renderer.Submit "
				                 << submit << " ms, Renderer.Prepare " << prepare << " ms (Renderer.Batch "
				                 << FindZone(medians, "Renderer.Batch") << " ms, Renderer.Cull " << FindZone(medians, "Renderer.Cull")
				                 << " ms), Submit + Prepare " << submit + prepare << " ms, Renderer.Record "
				                 << FindZone(medians, "Renderer.Record") << " ms; frame " << medians.FrameMilliseconds << " ms");
				MESSAGE("  DrawCalls " << FindCounter(medians, "Bench/DrawCalls") << ", Batches " << FindCounter(medians, "Bench/Batches")
				                       << ", Instances " << FindCounter(medians, "Bench/Instances") << ", ShadowDrawCalls "
				                       << FindCounter(medians, "Bench/ShadowDrawCalls") << ", Culled "
				                       << FindCounter(medians, "Bench/Culled") << "; GPU (ms):" << gpu);
			}
		}
		GraphicsDevice::WaitForIdle();
	}

	// Where submission's time goes: RenderScene's walk over the meshes with their cached world matrices, and
	// SceneRenderer's submission of an instance, in one step (SubmitMesh with the assets at hand, which resolves the mesh
	// and its materials each call) and in two (a mesh draw prepared once per frame, then SubmitMesh(draw) per instance,
	// as RenderScene submits).
	using Clock = std::chrono::steady_clock;
	std::vector<double> walks;
	std::vector<double> oneStep;
	std::vector<double> twoSteps;
	SceneRenderer renderer("Bench");
	renderer.SetViewportSize(Width, Height);
	std::vector<std::pair<Ref<MeshSource>, std::array<Ref<MaterialAsset>, 2>>> resolved;
	std::vector<glm::mat4> worlds;
	for (Entity const& child : bench.MeshChildren)
	{
		worlds.push_back(bench.World.GetWorldTransform(child));
		MeshComponent const& mesh = child.GetComponent<MeshComponent>();
		resolved.push_back(
			{AssetManager::GetAsset<MeshSource>(mesh.Mesh),
		     {AssetManager::GetAsset<MaterialAsset>(mesh.Materials[0]), AssetManager::GetAsset<MaterialAsset>(mesh.Materials[1])}});
	}
	size_t found = 0;
	glm::vec4 sink(0.0f);
	for (int repeat = 0; repeat < 21; repeat++)
	{
		auto const walkStart = Clock::now();
		bench.World.ForEachCachedWorldTransform<MeshComponent>(
			[&found, &sink](entt::entity, MeshComponent const& mesh, glm::mat4 const& world, glm::mat4 const&)
			{
				found += mesh.Visible ? 1 : 0;
				sink += world[3];
			});
		auto const walkEnd = Clock::now();

		renderer.BeginScene(camera, SceneRendererSettings());
		auto const oneStepStart = Clock::now();
		for (size_t index = 0; index < resolved.size(); index++)
		{
			renderer.SubmitMesh(resolved[index].first, resolved[index].second, worlds[index]);
		}
		auto const oneStepEnd = Clock::now();
		renderer.EndScene();
		GraphicsDevice::EndFrame();

		// The children repeat 12 pairs of mesh and materials (child % 4 and child % 3): 12 draws.
		renderer.BeginScene(camera, SceneRendererSettings());
		std::vector<MeshDraw> draws;
		for (size_t index = 0; index < 12; index++)
		{
			draws.push_back(renderer.PrepareMeshDraw(resolved[index].first, resolved[index].second));
		}
		auto const twoStepsStart = Clock::now();
		for (size_t index = 0; index < resolved.size(); index++)
		{
			MeshSubmission submission;
			submission.Transform = worlds[index];
			submission.PreviousTransform = worlds[index];
			renderer.SubmitMesh(draws[index % draws.size()], submission);
		}
		auto const twoStepsEnd = Clock::now();
		found += renderer.GetFrameInstances().size();
		renderer.EndScene();
		GraphicsDevice::EndFrame();

		walks.push_back(std::chrono::duration<double, std::milli>(walkEnd - walkStart).count());
		oneStep.push_back(std::chrono::duration<double, std::milli>(oneStepEnd - oneStepStart).count());
		twoSteps.push_back(std::chrono::duration<double, std::milli>(twoStepsEnd - twoStepsStart).count());
	}
	GraphicsDevice::WaitForIdle();
	std::sort(walks.begin(), walks.end());
	std::sort(oneStep.begin(), oneStep.end());
	std::sort(twoSteps.begin(), twoSteps.end());
	CHECK(found == 21 * 20000);
	MESSAGE("10,000 meshes walked with their cached matrices " << walks[walks.size() / 2] << " ms; 10,000 instances submitted in one step "
	                                                           << oneStep[oneStep.size() / 2] << " ms, in two (prepared mesh draws) "
	                                                           << twoSteps[twoSteps.size() / 2] << " ms (checksum " << sink.x << ")");
}
