#include "Editor/Viewport/CanvasOverlay.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/UILayout.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <unordered_set>

namespace Strada::CanvasOverlay
{
	namespace
	{
		// The largest reference resolution the inspector accepts: scripts can set any value.
		constexpr float MaxReferenceSize = 100000.0f;

		// The canvas whose UI an entity is drawn with: the outermost canvas among the entity and its ancestors (canvases
		// nested in another are drawn with it). Null outside canvases.
		entt::entity FindTopLevelCanvas(Scene& scene, Entity entity)
		{
			entt::entity canvas = entt::null;
			for (; entity; entity = scene.GetParent(entity))
			{
				if (entity.HasComponent<UICanvasComponent>())
				{
					canvas = entity.GetHandle();
				}
			}
			return canvas;
		}

		// The screen the canvas is designed for (its reference resolution) and the transform from its pixels to the canvas
		// entity's plane: standing on the entity (the middle of the bottom edge there), CanvasUnitSize meters per canvas unit,
		// y up.
		WorldCanvasPlacement GetPlacement(Scene& scene, Entity canvas)
		{
			UICanvasComponent const& component = canvas.GetComponent<UICanvasComponent>();
			auto const length = [](float value)
			{
				return std::isfinite(value) ? std::clamp(value, 1.0f, MaxReferenceSize) : 1.0f;
			};
			glm::vec2 const size(length(component.ReferenceResolution.x), length(component.ReferenceResolution.y));
			float const unit = CanvasUnitSize / UILayout::GetCanvasScale(component, size);
			glm::mat4 const pixels = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f * size.x * unit, size.y * unit, 0.0f)),
			                                    glm::vec3(unit, -unit, 1.0f));
			WorldCanvasPlacement placement;
			placement.Size = size;
			placement.Transform = scene.GetWorldTransform(canvas) * pixels;
			return placement;
		}

		// Whether a selected entity is the canvas or part of its UI.
		bool HoldsSelection(Scene& scene, entt::entity canvas, std::span<UUID const> selection)
		{
			return std::any_of(selection.begin(), selection.end(),
			                   [&scene, canvas](UUID id)
			                   {
								   return FindTopLevelCanvas(scene, scene.GetEntityByUUID(id)) == canvas;
							   });
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

	std::optional<WorldCanvasPlacement> PlaceCanvas(Scene& scene, UUID canvas, std::span<UUID const> selection)
	{
		Entity const entity = scene.GetEntityByUUID(canvas);
		if (!entity || !entity.HasComponent<UICanvasComponent>())
		{
			return std::nullopt;
		}
		WorldCanvasPlacement placement = GetPlacement(scene, entity);
		placement.Opacity = HoldsSelection(scene, entity.GetHandle(), selection) ? 1.0f : UnselectedOpacity;
		return placement;
	}

	void AppendCanvasOutlines(Scene& scene, std::span<UUID const> selection, std::vector<OverlayLine>& lines)
	{
		std::unordered_set<entt::entity> selected;
		for (UUID const id : selection)
		{
			if (Entity const entity = scene.GetEntityByUUID(id))
			{
				selected.insert(entity.GetHandle());
			}
		}

		std::vector<UIElement> elements;
		for (entt::entity const handle : UILayout::GetCanvases(scene))
		{
			WorldCanvasPlacement const placement = GetPlacement(scene, Entity(handle, &scene));
			UIRectangle const screen{glm::vec2(0.0f), placement.Size};
			AddRectangle(lines, placement.Transform, screen, selected.contains(handle) ? SelectedColor : CanvasColor);
			if (!HoldsSelection(scene, handle, selection))
			{
				continue;
			}
			elements.clear();
			UILayout::LayoutCanvas(scene, handle, placement.Size, elements);
			for (UIElement const& element : elements)
			{
				if (element.Handle != handle && Entity(element.Handle, &scene).HasComponent<UIRectComponent>())
				{
					AddRectangle(lines, placement.Transform, element.Rectangle,
					             selected.contains(element.Handle) ? SelectedColor : ElementColor);
				}
			}
		}
	}

	std::optional<AABB> GetUIBounds(Scene& scene, UUID id)
	{
		Entity const entity = scene.GetEntityByUUID(id);
		entt::entity const canvas = entity ? FindTopLevelCanvas(scene, entity) : entt::null;
		if (canvas == entt::null)
		{
			return std::nullopt;
		}
		WorldCanvasPlacement const placement = GetPlacement(scene, Entity(canvas, &scene));
		UIRectangle rectangle{glm::vec2(0.0f), placement.Size};
		std::vector<UIElement> elements;
		UILayout::LayoutCanvas(scene, canvas, placement.Size, elements);
		auto const element = std::find_if(elements.begin(), elements.end(),
		                                  [handle = entity.GetHandle()](UIElement const& candidate)
		                                  {
											  return candidate.Handle == handle;
										  });
		if (element != elements.end())
		{
			rectangle = element->Rectangle;
		}
		AABB bounds;
		for (glm::vec2 const corner :
		     {rectangle.Min, glm::vec2(rectangle.Max.x, rectangle.Min.y), rectangle.Max, glm::vec2(rectangle.Min.x, rectangle.Max.y)})
		{
			bounds.Expand(glm::vec3(placement.Transform * glm::vec4(corner, 0.0f, 1.0f)));
		}
		return bounds;
	}

	bool IsUIElement(Scene& scene, UUID id)
	{
		Entity const entity = scene.GetEntityByUUID(id);
		return entity && entity.HasComponent<UIRectComponent>() && !entity.HasComponent<UICanvasComponent>() &&
		       FindTopLevelCanvas(scene, scene.GetParent(entity)) != entt::null;
	}
}
