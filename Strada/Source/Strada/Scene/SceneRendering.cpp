#include "stpch.h"
#include "Strada/Scene/SceneRendering.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Math/Math.h"
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

		void SubmitScene(Scene& scene, SceneRenderer& renderer, SceneRenderOptions const& options)
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
				UUID const id = entity.GetUUID();
				uint32_t const pickingId = options.GetPickingId ? options.GetPickingId(id) : 0u;
				bool const selected = options.IsSelected && options.IsSelected(id);
				renderer.SubmitMesh(mesh, materials, scene.GetWorldTransform(entity), component.CastShadows, pickingId, selected);
			}

			for (entt::entity const handle : scene.GetAllEntitiesWith<SpriteRendererComponent>())
			{
				Entity const entity(handle, &scene);
				SpriteRendererComponent const& component = entity.GetComponent<SpriteRendererComponent>();
				UUID const id = entity.GetUUID();
				SpriteSubmission submission;
				submission.Transform = scene.GetWorldTransform(entity);
				submission.Color = component.Color;
				submission.Texture = component.Texture;
				submission.Tiling = component.Tiling;
				submission.ScreenSpace = component.ScreenSpace;
				submission.PickingId = options.GetPickingId ? options.GetPickingId(id) : 0u;
				submission.Selected = options.IsSelected && options.IsSelected(id);
				renderer.SubmitSprite(submission);
			}

			for (entt::entity const handle : scene.GetAllEntitiesWith<TextComponent>())
			{
				Entity const entity(handle, &scene);
				TextComponent const& component = entity.GetComponent<TextComponent>();
				if (component.Text.empty())
				{
					continue;
				}
				UUID const id = entity.GetUUID();
				TextSubmission submission;
				submission.Text = component.Text;
				// Fonts that cannot be loaded fall back to the default font.
				submission.Font = component.Font.IsValid() ? AssetManager::GetAsset<FontAsset>(component.Font) : nullptr;
				submission.Transform = scene.GetWorldTransform(entity);
				submission.Color = component.Color;
				submission.FontSize = component.FontSize;
				submission.Layout.Alignment = component.Alignment;
				submission.Layout.LineSpacing = component.LineSpacing;
				submission.ScreenSpace = component.ScreenSpace;
				submission.PickingId = options.GetPickingId ? options.GetPickingId(id) : 0u;
				submission.Selected = options.IsSelected && options.IsSelected(id);
				renderer.SubmitText(std::move(submission));
			}

			for (entt::entity const handle : scene.GetAllEntitiesWith<DirectionalLightComponent>())
			{
				Entity const entity(handle, &scene);
				DirectionalLightComponent const& light = entity.GetComponent<DirectionalLightComponent>();
				DirectionalLightSubmission submission;
				submission.Direction = GetForward(scene.GetWorldTransform(entity));
				submission.Color = light.Color;
				submission.Intensity = light.Intensity;
				submission.CastShadows = light.CastShadows;
				submission.LightSize = light.LightSize;
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
				submission.CastShadows = light.CastShadows;
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
				submission.CastShadows = light.CastShadows;
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

	void RenderScene(Scene& scene, SceneRenderer& renderer, SceneRendererCamera const& camera, SceneRenderOptions const& options)
	{
		renderer.BeginScene(camera, scene.GetSettings().Renderer);
		SubmitScene(scene, renderer, options);
		// Lines scripts drew for debugging have linear colors; lines are drawn after tonemapping, in sRGB.
		for (SceneDebugLine const& line : scene.GetDebugLines())
		{
			glm::vec4 const color(Math::LinearToSrgb(line.Color.r), Math::LinearToSrgb(line.Color.g), Math::LinearToSrgb(line.Color.b),
			                      line.Color.a);
			renderer.SubmitLine(line.From, line.To, color);
		}
		if (options.SubmitOverlays)
		{
			options.SubmitOverlays(renderer);
		}
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
		CameraComponent const& component = cameraEntity.GetComponent<CameraComponent>();
		camera.MaxDistance = component.Projection == ProjectionType::Perspective ? component.PerspectiveFar : component.OrthographicFar;
		RenderScene(scene, renderer, camera);
		return true;
	}
}
