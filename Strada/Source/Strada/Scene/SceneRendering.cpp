#include "stpch.h"
#include "Strada/Scene/SceneRendering.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/PostProcessVolumes.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneCamera.h"
#include "Strada/Scene/UILayout.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
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

		// Every live particle as a sprite facing the camera.
		void SubmitParticles(Scene& scene, SceneRenderer& renderer, SceneRendererCamera const& camera)
		{
			glm::mat3 const cameraBasis(glm::inverse(camera.View));
			for (entt::entity const handle : scene.GetAllEntitiesWith<ParticleSystemComponent>())
			{
				Entity const entity(handle, &scene);
				std::span<Particle const> const particles = scene.GetParticles(entity);
				if (particles.empty())
				{
					continue;
				}
				ParticleSystemComponent const& component = entity.GetComponent<ParticleSystemComponent>();
				glm::mat4 const frame = component.WorldSpace ? glm::mat4(1.0f) : scene.GetWorldTransform(entity);
				SpriteSubmission submission;
				submission.Texture = component.Texture.IsValid() ? component.Texture : GetBuiltInHandle(BuiltInAsset::ParticleTexture);
				submission.Additive = component.BlendMode == ParticleBlendMode::Additive;
				for (Particle const& particle : particles)
				{
					float const size = ParticleEmitter::GetSize(component, particle);
					glm::vec4 const color = ParticleEmitter::GetColor(component, particle);
					if (size <= 0.0f || color.a <= 0.0f)
					{
						continue;
					}
					glm::vec3 const position(frame * glm::vec4(particle.Position, 1.0f));
					submission.Transform = glm::mat4(glm::vec4(cameraBasis[0] * size, 0.0f), glm::vec4(cameraBasis[1] * size, 0.0f),
					                                 glm::vec4(cameraBasis[2], 0.0f), glm::vec4(position, 1.0f));
					submission.Color = color;
					renderer.SubmitSprite(submission);
				}
			}
		}

		glm::vec4 GetButtonColor(UIButtonComponent const& button, UIButtonState state)
		{
			switch (state)
			{
				case UIButtonState::Normal:
					break;
				case UIButtonState::Hovered:
					return button.HoveredColor;
				case UIButtonState::Pressed:
					return button.PressedColor;
				case UIButtonState::Disabled:
					return button.DisabledColor;
			}
			return button.NormalColor;
		}

		// The UI draws onto the 8-bit final image: scripts may set colors outside the fields' ranges, whose negative alphas
		// would brighten what is behind.
		glm::vec4 GetUIColor(glm::vec4 const& color, float opacity)
		{
			glm::vec4 result = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
			result.a *= opacity;
			return result;
		}

		// An element's image, then its text: over the final image, or in the world through a canvas's transform.
		void SubmitUIElement(Scene& scene, SceneRenderer& renderer, SceneRenderOptions const& options, UIElement const& element,
		                     std::optional<glm::mat4> const& worldTransform, float opacity)
		{
			Entity entity(element.Handle, &scene);
			UUID const id = entity.GetUUID();
			uint32_t const pickingId = options.GetPickingId ? options.GetPickingId(id) : 0u;
			bool const selected = options.IsSelected && options.IsSelected(id);
			if (UIImageComponent const* image = entity.TryGetComponent<UIImageComponent>())
			{
				UIRectSubmission submission;
				submission.Min = element.Rectangle.Min;
				submission.Max = element.Rectangle.Max;
				UIButtonComponent const* button = entity.TryGetComponent<UIButtonComponent>();
				submission.Color = GetUIColor(button != nullptr ? GetButtonColor(*button, scene.GetUIButtonState(entity)) : image->Color,
				                              element.Opacity * opacity);
				submission.Texture = image->Texture;
				submission.PreserveAspect = image->PreserveAspect;
				submission.CornerRadius = image->CornerRadius * element.Scale;
				submission.WorldTransform = worldTransform;
				submission.PickingId = pickingId;
				submission.Selected = selected;
				renderer.SubmitUIRect(submission);
			}
			UITextComponent const* text = entity.TryGetComponent<UITextComponent>();
			if (text != nullptr && !text->Text.empty())
			{
				UITextSubmission submission;
				submission.Text = text->Text;
				// Fonts that cannot be loaded fall back to the default font.
				submission.Font = text->Font.IsValid() ? AssetManager::GetAsset<FontAsset>(text->Font) : nullptr;
				submission.Min = element.Rectangle.Min;
				submission.Max = element.Rectangle.Max;
				submission.Color = GetUIColor(text->Color, element.Opacity * opacity);
				submission.FontSize = text->FontSize * element.Scale;
				submission.Layout.Alignment = text->Alignment;
				submission.Layout.LineSpacing = text->LineSpacing;
				submission.VerticalAlignment = text->VerticalAlignment;
				submission.Wrap = text->Wrap;
				submission.WorldTransform = worldTransform;
				submission.PickingId = pickingId;
				submission.Selected = selected;
				renderer.SubmitUIText(std::move(submission));
			}
		}

		// Every canvas's UI in hierarchy order: over the renderer's viewport, or where the options place each canvas in the
		// world.
		void SubmitUI(Scene& scene, SceneRenderer& renderer, SceneRenderOptions const& options)
		{
			if (!options.PlaceCanvas)
			{
				glm::vec2 const viewport(static_cast<float>(renderer.GetViewportWidth()), static_cast<float>(renderer.GetViewportHeight()));
				for (UIElement const& element : UILayout::LayoutScene(scene, viewport))
				{
					SubmitUIElement(scene, renderer, options, element, std::nullopt, 1.0f);
				}
				return;
			}
			std::vector<UIElement> elements;
			for (entt::entity const canvas : UILayout::GetCanvases(scene))
			{
				std::optional<WorldCanvasPlacement> const placement = options.PlaceCanvas(Entity(canvas, &scene).GetUUID());
				if (!placement)
				{
					continue;
				}
				elements.clear();
				UILayout::LayoutCanvas(scene, canvas, placement->Size, elements);
				float const opacity = std::isfinite(placement->Opacity) ? std::clamp(placement->Opacity, 0.0f, 1.0f) : 1.0f;
				for (UIElement const& element : elements)
				{
					SubmitUIElement(scene, renderer, options, element, placement->Transform, opacity);
				}
			}
		}

		void SubmitMeshes(Scene& scene, SceneRenderer& renderer, SceneRenderOptions const& options)
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
		}

		// With screenSpaceOnly, only the sprites drawn over the image.
		void SubmitSprites(Scene& scene, SceneRenderer& renderer, SceneRenderOptions const& options, bool screenSpaceOnly)
		{
			for (entt::entity const handle : scene.GetAllEntitiesWith<SpriteRendererComponent>())
			{
				Entity const entity(handle, &scene);
				SpriteRendererComponent const& component = entity.GetComponent<SpriteRendererComponent>();
				if (screenSpaceOnly && !component.ScreenSpace)
				{
					continue;
				}
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
		}

		// With screenSpaceOnly, only the text drawn over the image.
		void SubmitTexts(Scene& scene, SceneRenderer& renderer, SceneRenderOptions const& options, bool screenSpaceOnly)
		{
			for (entt::entity const handle : scene.GetAllEntitiesWith<TextComponent>())
			{
				Entity const entity(handle, &scene);
				TextComponent const& component = entity.GetComponent<TextComponent>();
				if (component.Text.empty() || (screenSpaceOnly && !component.ScreenSpace))
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
		}

		void SubmitLights(Scene& scene, SceneRenderer& renderer)
		{
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

		// Screen-space content alone has nothing to shade: every pass that works on the world is off, and the black background
		// stays black.
		SceneRendererSettings GetScreenSpaceOnlySettings()
		{
			SceneRendererSettings settings;
			settings.Shadows = false;
			settings.AmbientOcclusion = false;
			settings.Bloom = false;
			settings.FXAA = false;
			settings.Dithering = false;
			return settings;
		}
	}

	void RenderScene(Scene& scene, SceneRenderer& renderer, SceneRendererCamera const& camera, SceneRenderOptions const& options)
	{
		// The scene's post-process volumes change its settings around the camera.
		renderer.BeginScene(camera, options.DrawWorld ? PostProcessVolumes::Resolve(scene, camera.Position) : GetScreenSpaceOnlySettings());
		if (options.DrawWorld)
		{
			SubmitMeshes(scene, renderer, options);
		}
		SubmitSprites(scene, renderer, options, !options.DrawWorld);
		if (options.DrawWorld)
		{
			SubmitParticles(scene, renderer, camera);
		}
		SubmitTexts(scene, renderer, options, !options.DrawWorld);
		if (options.DrawWorld)
		{
			SubmitLights(scene, renderer);
		}
		if (options.DrawUI)
		{
			SubmitUI(scene, renderer, options);
		}
		// Lines scripts drew for debugging have linear colors; lines are drawn after tonemapping, in sRGB.
		if (options.DrawWorld && options.DrawDebugLines)
		{
			for (SceneDebugLine const& line : scene.GetDebugLines())
			{
				glm::vec4 const color(Math::LinearToSrgb(line.Color.r), Math::LinearToSrgb(line.Color.g), Math::LinearToSrgb(line.Color.b),
				                      line.Color.a);
				renderer.SubmitLine(line.From, line.To, color);
			}
		}
		if (options.SubmitOverlays)
		{
			options.SubmitOverlays(renderer);
		}
		renderer.EndScene();
	}

	bool HasScreenSpaceContent(Scene& scene)
	{
		if (!scene.GetAllEntitiesWith<UICanvasComponent>().empty())
		{
			return true;
		}
		for (entt::entity const handle : scene.GetAllEntitiesWith<SpriteRendererComponent>())
		{
			if (scene.GetRegistry().get<SpriteRendererComponent>(handle).ScreenSpace)
			{
				return true;
			}
		}
		for (entt::entity const handle : scene.GetAllEntitiesWith<TextComponent>())
		{
			TextComponent const& text = scene.GetRegistry().get<TextComponent>(handle);
			if (text.ScreenSpace && !text.Text.empty())
			{
				return true;
			}
		}
		return false;
	}

	bool RenderSceneFromPrimaryCamera(Scene& scene, SceneRenderer& renderer)
	{
		Entity const cameraEntity = scene.GetPrimaryCameraEntity();
		if (!cameraEntity)
		{
			// Title, menu and loading screens are often UI alone: it needs no camera.
			if (!HasScreenSpaceContent(scene))
			{
				return false;
			}
			SceneRenderOptions options;
			options.DrawWorld = false;
			RenderScene(scene, renderer, SceneRendererCamera(), options);
			return true;
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
