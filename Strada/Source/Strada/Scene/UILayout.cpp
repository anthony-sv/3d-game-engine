#include "stpch.h"
#include "Strada/Scene/UILayout.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace Strada
{
	namespace
	{
		bool IsFinite(glm::vec2 const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		// What a canvas passes to its descendants while it is laid out.
		struct LayoutParent
		{
			UIRectangle Rectangle;
			float Scale = 1.0f;
			float Opacity = 1.0f;
			entt::entity Canvas = entt::null;
			entt::entity Button = entt::null;
			// Below a canvas (entities outside canvases are not UI).
			bool InCanvas = false;
		};

		struct PendingEntity
		{
			entt::entity Handle = entt::null;
			LayoutParent Parent;
		};
	}

	namespace UILayout
	{
		float GetCanvasScale(UICanvasComponent const& canvas, glm::vec2 viewportSize)
		{
			if (canvas.ScaleMode != UIScaleMode::ScaleWithScreenHeight || !(viewportSize.y > 0.0f))
			{
				return 1.0f;
			}
			// Scripts can set any finite reference: tiny ones would blow the UI up beyond any screen.
			float const referenceHeight = std::isfinite(canvas.ReferenceResolution.y) ? std::max(canvas.ReferenceResolution.y, 1.0f) : 1.0f;
			return viewportSize.y / referenceHeight;
		}

		UIRectangle ComputeRectangle(UIRectComponent const& rect, UIRectangle const& parent, float scale)
		{
			glm::vec2 const parentSize = parent.GetSize();
			glm::vec2 const anchorMin = parent.Min + rect.AnchorMin * parentSize;
			glm::vec2 const anchorMax = parent.Min + rect.AnchorMax * parentSize;
			glm::vec2 const span = anchorMax - anchorMin;
			glm::vec2 const size = glm::max(span + rect.Size * scale, glm::vec2(0.0f));
			glm::vec2 const pivot = anchorMin + span * rect.Pivot + rect.Position * scale;

			UIRectangle result;
			result.Min = pivot - size * rect.Pivot;
			result.Max = result.Min + size;
			if (!IsFinite(result.Min) || !IsFinite(result.Max))
			{
				return {parent.Min, parent.Min};
			}
			return result;
		}

		std::vector<UIElement> LayoutScene(Scene& scene, glm::vec2 viewportSize)
		{
			std::vector<UIElement> elements;
			entt::registry const& registry = scene.GetRegistry();
			// Scenes without a canvas (most of them) skip the walk over their hierarchy, which runs every frame.
			if (registry.view<UICanvasComponent>().empty())
			{
				return elements;
			}
			UIRectangle const screen{glm::vec2(0.0f), glm::max(viewportSize, glm::vec2(0.0f))};

			// Explicit stack: deep hierarchies must not overflow the call stack. Children are pushed in reverse, so they pop
			// in sibling order.
			std::vector<PendingEntity> stack;
			std::vector<UUID> const& roots = scene.GetRootEntityIDs();
			for (auto root = roots.rbegin(); root != roots.rend(); ++root)
			{
				if (Entity const entity = scene.GetEntityByUUID(*root))
				{
					stack.push_back({entity.GetHandle(), LayoutParent()});
				}
			}

			while (!stack.empty())
			{
				PendingEntity const pending = stack.back();
				stack.pop_back();
				entt::entity const handle = pending.Handle;

				LayoutParent current;
				if (UICanvasComponent const* canvas = registry.try_get<UICanvasComponent>(handle))
				{
					// A canvas covers the screen, wherever it is in the hierarchy.
					current.Rectangle = screen;
					current.Scale = GetCanvasScale(*canvas, screen.Max);
					current.Canvas = handle;
					current.InCanvas = true;
				}
				else if (pending.Parent.InCanvas)
				{
					current = pending.Parent;
					if (UIRectComponent const* rect = registry.try_get<UIRectComponent>(handle))
					{
						if (!rect->Visible)
						{
							continue;
						}
						current.Rectangle = ComputeRectangle(*rect, pending.Parent.Rectangle, pending.Parent.Scale);
						current.Opacity *= std::isfinite(rect->Opacity) ? std::clamp(rect->Opacity, 0.0f, 1.0f) : 1.0f;
					}
				}

				if (current.InCanvas)
				{
					if (registry.all_of<UIButtonComponent>(handle))
					{
						current.Button = handle;
					}
					UIElement element;
					element.Handle = handle;
					element.Canvas = current.Canvas;
					element.Rectangle = current.Rectangle;
					element.Scale = current.Scale;
					element.Opacity = current.Opacity;
					element.HitTarget =
						current.Opacity > 0.0f && (registry.all_of<UIImageComponent>(handle) || registry.all_of<UIButtonComponent>(handle));
					element.Button = current.Button;
					elements.push_back(element);
				}

				std::vector<UUID> const& children = registry.get<RelationshipComponent>(handle).Children;
				for (auto child = children.rbegin(); child != children.rend(); ++child)
				{
					if (Entity const entity = scene.GetEntityByUUID(*child))
					{
						stack.push_back({entity.GetHandle(), current});
					}
				}
			}
			return elements;
		}

		entt::entity FindButtonAt(std::span<UIElement const> elements, glm::vec2 point)
		{
			for (auto element = elements.rbegin(); element != elements.rend(); ++element)
			{
				if (element->HitTarget && element->Rectangle.Contains(point))
				{
					return element->Button;
				}
			}
			return entt::null;
		}
	}
}
