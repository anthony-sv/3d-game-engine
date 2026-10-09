#include "stpch.h"
#include "Strada/Scene/SceneRendering.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneCamera.h"

#include <vector>

namespace Strada
{
	namespace
	{
		// Local -Z is the forward direction of lights and cameras.
		glm::vec3 GetForward(glm::mat4 const& worldTransform)
		{
			glm::vec3 const forward = glm::vec3(worldTransform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
			float const length = glm::length(forward);
			return length > 1e-6f ? forward / length : glm::vec3(0.0f, 0.0f, -1.0f);
		}

		void SubmitScene(Scene& scene, SceneRenderer& renderer)
		{
			std::vector<Ref<MaterialAsset>> materials;
			for (entt::entity const handle : scene.GetAllEntitiesWith<MeshComponent>())
			{
				Entity const entity(handle, &scene);
				MeshComponent const& component = entity.GetComponent<MeshComponent>();
				if (!component.Visible)
				{
					continue;
				}
				Ref<MeshSource> const mesh = AssetManager::GetAsset<MeshSource>(component.Mesh);
				if (!mesh)
				{
					continue;
				}

				std::vector<AssetHandle> const& defaults = mesh->GetMaterials();
				materials.assign(defaults.size(), nullptr);
				for (size_t slot = 0; slot < defaults.size(); slot++)
				{
					AssetHandle const handleForSlot = slot < component.Materials.size() && component.Materials[slot].IsValid()
					                                      ? component.Materials[slot]
					                                      : defaults[slot];
					materials[slot] = AssetManager::GetAsset<MaterialAsset>(handleForSlot);
				}
				renderer.SubmitMesh(mesh, materials, scene.GetWorldTransform(entity));
			}

			for (entt::entity const handle : scene.GetAllEntitiesWith<DirectionalLightComponent>())
			{
				Entity const entity(handle, &scene);
				DirectionalLightComponent const& light = entity.GetComponent<DirectionalLightComponent>();
				DirectionalLightSubmission submission;
				submission.Direction = GetForward(scene.GetWorldTransform(entity));
				submission.Color = light.Color;
				submission.Intensity = light.Intensity;
				renderer.SubmitDirectionalLight(submission);
			}

			for (entt::entity const handle : scene.GetAllEntitiesWith<PointLightComponent>())
			{
				Entity const entity(handle, &scene);
				PointLightComponent const& light = entity.GetComponent<PointLightComponent>();
				PointLightSubmission submission;
				submission.Position = glm::vec3(scene.GetWorldTransform(entity)[3]);
				submission.Color = light.Color;
				submission.Intensity = light.Intensity;
				submission.Range = light.Range;
				renderer.SubmitPointLight(submission);
			}

			for (entt::entity const handle : scene.GetAllEntitiesWith<SpotLightComponent>())
			{
				Entity const entity(handle, &scene);
				SpotLightComponent const& light = entity.GetComponent<SpotLightComponent>();
				glm::mat4 const world = scene.GetWorldTransform(entity);
				SpotLightSubmission submission;
				submission.Position = glm::vec3(world[3]);
				submission.Direction = GetForward(world);
				submission.Color = light.Color;
				submission.Intensity = light.Intensity;
				submission.Range = light.Range;
				submission.InnerConeAngle = light.InnerConeAngle;
				submission.OuterConeAngle = light.OuterConeAngle;
				renderer.SubmitSpotLight(submission);
			}

			// The first sky light (in hierarchy order) provides the ambient light.
			scene.ForEachEntityInHierarchyOrder(
				[&renderer, found = false](Entity entity) mutable
				{
					if (!found && entity.HasComponent<SkyLightComponent>())
					{
						SkyLightComponent const& sky = entity.GetComponent<SkyLightComponent>();
						renderer.SetAmbientLight(sky.AmbientColor * sky.Intensity);
						if (Ref<EnvironmentAsset> environment = AssetManager::GetAsset<EnvironmentAsset>(sky.Environment))
						{
							EnvironmentSubmission submission;
							submission.Environment = std::move(environment);
							submission.Intensity = sky.Intensity;
							submission.Rotation = sky.Rotation;
							submission.SkyboxBlur = sky.SkyboxBlur;
							submission.DrawSkybox = sky.DrawSkybox;
							renderer.SetEnvironment(submission);
						}
						found = true;
					}
				});
		}
	}

	void RenderScene(Scene& scene, SceneRenderer& renderer, SceneRendererCamera const& camera)
	{
		renderer.BeginScene(camera, scene.GetSettings().Renderer);
		SubmitScene(scene, renderer);
		renderer.EndScene();
	}

	bool RenderSceneFromPrimaryCamera(Scene& scene, SceneRenderer& renderer)
	{
		Entity const cameraEntity = scene.GetPrimaryCameraEntity();
		if (!cameraEntity)
		{
			return false;
		}
		glm::mat4 const world = scene.GetWorldTransform(cameraEntity);
		float const aspectRatio = static_cast<float>(renderer.GetViewportWidth()) / static_cast<float>(renderer.GetViewportHeight());

		SceneRendererCamera camera;
		camera.View = glm::inverse(world);
		camera.Projection = ComputeCameraProjection(cameraEntity.GetComponent<CameraComponent>(), aspectRatio);
		camera.Position = glm::vec3(world[3]);
		RenderScene(scene, renderer, camera);
		return true;
	}
}
