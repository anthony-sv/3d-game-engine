#include "Editor/Viewport/CanvasOverlay.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/UILayout.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <unordered_set>

namespace Strada::CanvasOverlay
{
	namespace
	{
		// The canvas the entity belongs to: the nearest canvas among the entity and its ancestors.
		entt::entity FindCanvas(Scene& scene, Entity entity)
		{
			for (; entity; entity = scene.GetParent(entity))
			{
				if (entity.HasComponent<UICanvasComponent>())
				{
					return entity.GetHandle();
				}
			}
			return entt::null;
		}

		void AddRectangle(std::vector<OverlayLine>& lines, glm::mat4 const& transform, UIRectangle const& rectangle, glm::vec4 const& color)
		{
			glm::vec3 const corners[4] = {
				glm::vec3(transform * glm::vec4(rectangle.Min.x, rectangle.Min.y, 0.0f, 1.0f)),
				glm::vec3(transform * glm::vec4(rectangle.Max.x, rectangle.Min.y, 0.0f, 1.0f)),
				glm::vec3(transform * glm::vec4(rectangle.Max.x, rectangle.Max.y, 0.0f, 1.0f)),
				glm::vec3(transform * glm::vec4(rectangle.Min.x, rectangle.Max.y, 0.0f, 1.0f)),
			};
			for (uint32_t i = 0; i < 4; i++)
			{
				lines.push_back({corners[i], corners[(i + 1) % 4], color, false});
			}
		}
	}

	void AppendSelectedCanvases(Scene& scene, std::span<UUID const> selection, std::vector<OverlayLine>& lines)
	{
		std::unordered_set<entt::entity> selected;
		std::vector<entt::entity> canvases;
		for (UUID const id : selection)
		{
			Entity const entity = scene.GetEntityByUUID(id);
			entt::entity const canvas = FindCanvas(scene, entity);
			if (canvas == entt::null)
			{
				continue;
			}
			selected.insert(entity.GetHandle());
			if (std::find(canvases.begin(), canvases.end(), canvas) == canvases.end())
			{
				canvases.push_back(canvas);
			}
		}
		if (canvases.empty())
		{
			return;
		}

		for (entt::entity const canvasHandle : canvases)
		{
			Entity canvas(canvasHandle, &scene);
			UICanvasComponent const& component = canvas.GetComponent<UICanvasComponent>();
			// The screen the UI is designed for: the game view's size changes with the editor's layout (and stays as it was
			// while the view is hidden).
			glm::vec2 const viewport = glm::max(component.ReferenceResolution, glm::vec2(1.0f));
			std::vector<UIElement> const elements = UILayout::LayoutScene(scene, viewport);
			// Pixels to the canvas entity's plane: centered, in canvas units of CanvasUnitSize meters, y up.
			float const unit = CanvasUnitSize / UILayout::GetCanvasScale(component, viewport);
			glm::mat4 const pixels =
				glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f * viewport.x * unit, 0.5f * viewport.y * unit, 0.0f)),
			               glm::vec3(unit, -unit, 1.0f));
			glm::mat4 const transform = scene.GetWorldTransform(canvas) * pixels;

			for (UIElement const& element : elements)
			{
				if (element.Canvas != canvasHandle)
				{
					continue;
				}
				bool const isSelected = selected.contains(element.Handle);
				if (element.Handle == canvasHandle)
				{
					AddRectangle(lines, transform, element.Rectangle, isSelected ? SelectedColor : CanvasColor);
				}
				else if (Entity(element.Handle, &scene).HasComponent<UIRectComponent>())
				{
					AddRectangle(lines, transform, element.Rectangle, isSelected ? SelectedColor : ElementColor);
				}
			}
		}
	}

	bool IsUIElement(Scene& scene, UUID id)
	{
		Entity const entity = scene.GetEntityByUUID(id);
		return entity && entity.HasComponent<UIRectComponent>() && !entity.HasComponent<UICanvasComponent>() &&
		       FindCanvas(scene, scene.GetParent(entity)) != entt::null;
	}
}
