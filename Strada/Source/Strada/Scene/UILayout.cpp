#include "stpch.h"
#include "Strada/Scene/UILayout.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Strada
{
	namespace
	{
		bool IsFinite(glm::vec2 const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		// A reference length a canvas fits to the screen: scripts can set any value, and tiny or infinite ones would blow the
		// UI up beyond any screen or shrink it to nothing.
		float GetReferenceLength(float length)
		{
			return std::isfinite(length) ? std::max(length, 1.0f) : 1.0f;
		}

		// What an element passes to its descendants while a canvas is laid out.
		struct LayoutParent
		{
			UIRectangle Rectangle;
			float Scale = 1.0f;
			float Opacity = 1.0f;
			entt::entity Canvas = entt::null;
			entt::entity Button = entt::null;
		};

		struct PendingEntity
		{
			entt::entity Handle = entt::null;
			LayoutParent Parent;
		};

		// The position of an entity in the hierarchy, from its root down: its index among the roots, then among its parent's
		// children at every level. Positions sort in hierarchy order. Empty when a canvas is among the entity's ancestors.
		std::vector<size_t> GetTopLevelPosition(Scene& scene, entt::entity handle)
		{
			entt::registry const& registry = scene.GetRegistry();
			std::vector<size_t> position;
			for (Entity entity(handle, &scene); entity;)
			{
				UUID const id = entity.GetUUID();
				Entity const parent = scene.GetParent(entity);
				if (parent && registry.all_of<UICanvasComponent>(parent.GetHandle()))
				{
					return {};
				}
				std::vector<UUID> const& siblings =
					parent ? registry.get<RelationshipComponent>(parent.GetHandle()).Children : scene.GetRootEntityIDs();
				position.push_back(static_cast<size_t>(std::find(siblings.begin(), siblings.end(), id) - siblings.begin()));
				entity = parent;
			}
			std::reverse(position.begin(), position.end());
			return position;
		}
	}

	namespace UILayout
	{
		float GetCanvasScale(UICanvasComponent const& canvas, glm::vec2 viewportSize)
		{
			if (!(viewportSize.x > 0.0f) || !(viewportSize.y > 0.0f) || !IsFinite(viewportSize))
			{
				return 1.0f;
			}
			switch (canvas.ScaleMode)
			{
				case UIScaleMode::ConstantPixelSize:
					break;
				case UIScaleMode::ScaleWithScreenHeight:
					return viewportSize.y / GetReferenceLength(canvas.ReferenceResolution.y);
				case UIScaleMode::ScaleWithScreenSize:
					// The whole reference resolution fits: the screen's narrower side (relative to the reference) sets the scale.
					return std::min(viewportSize.x / GetReferenceLength(canvas.ReferenceResolution.x),
					                viewportSize.y / GetReferenceLength(canvas.ReferenceResolution.y));
			}
			return 1.0f;
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

		std::vector<entt::entity> GetCanvases(Scene& scene)
		{
			std::vector<std::pair<std::vector<size_t>, entt::entity>> canvases;
			for (entt::entity const handle : scene.GetRegistry().view<UICanvasComponent>())
			{
				std::vector<size_t> position = GetTopLevelPosition(scene, handle);
				if (!position.empty())
				{
					canvases.emplace_back(std::move(position), handle);
				}
			}
			std::sort(canvases.begin(), canvases.end(),
			          [](auto const& a, auto const& b)
			          {
						  return a.first < b.first;
					  });
			std::vector<entt::entity> result;
			result.reserve(canvases.size());
			for (auto const& [position, handle] : canvases)
			{
				result.push_back(handle);
			}
			return result;
		}

		void LayoutCanvas(Scene& scene, entt::entity canvas, glm::vec2 viewportSize, std::vector<UIElement>& elements)
		{
			entt::registry const& registry = scene.GetRegistry();
			if (!registry.valid(canvas) || !registry.all_of<UICanvasComponent>(canvas))
			{
				return;
			}
			glm::vec2 const size(std::isfinite(viewportSize.x) ? std::max(viewportSize.x, 0.0f) : 0.0f,
			                     std::isfinite(viewportSize.y) ? std::max(viewportSize.y, 0.0f) : 0.0f);
			UIRectangle const screen{glm::vec2(0.0f), size};

			// Explicit stack: deep hierarchies must not overflow the call stack. Children are pushed in reverse, so they pop
			// in sibling order.
			std::vector<PendingEntity> stack = {{canvas, LayoutParent()}};
			while (!stack.empty())
			{
				PendingEntity const pending = stack.back();
				stack.pop_back();
				entt::entity const handle = pending.Handle;

				LayoutParent current = pending.Parent;
				if (UICanvasComponent const* canvasComponent = registry.try_get<UICanvasComponent>(handle))
				{
					// A canvas covers the screen, wherever it is in the hierarchy.
					current = LayoutParent();
					current.Rectangle = screen;
					current.Scale = GetCanvasScale(*canvasComponent, size);
					current.Canvas = handle;
				}
				else if (UIRectComponent const* rect = registry.try_get<UIRectComponent>(handle))
				{
					if (!rect->Visible)
					{
						continue;
					}
					current.Rectangle = ComputeRectangle(*rect, pending.Parent.Rectangle, pending.Parent.Scale);
					current.Opacity *= std::isfinite(rect->Opacity) ? std::clamp(rect->Opacity, 0.0f, 1.0f) : 1.0f;
				}

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

				std::vector<UUID> const& children = registry.get<RelationshipComponent>(handle).Children;
				for (auto child = children.rbegin(); child != children.rend(); ++child)
				{
					if (Entity const entity = scene.GetEntityByUUID(*child))
					{
						stack.push_back({entity.GetHandle(), current});
					}
				}
			}
		}

		std::vector<UIElement> LayoutScene(Scene& scene, glm::vec2 viewportSize)
		{
			std::vector<UIElement> elements;
			for (entt::entity const canvas : GetCanvases(scene))
			{
				LayoutCanvas(scene, canvas, viewportSize, elements);
			}
			return elements;
		}

		UIElement const* FindHitTargetAt(std::span<UIElement const> elements, glm::vec2 point)
		{
			for (auto element = elements.rbegin(); element != elements.rend(); ++element)
			{
				if (element->HitTarget && element->Rectangle.Contains(point))
				{
					return &*element;
				}
			}
			return nullptr;
		}

		entt::entity FindButtonAt(std::span<UIElement const> elements, glm::vec2 point)
		{
			UIElement const* const target = FindHitTargetAt(elements, point);
			return target != nullptr ? target->Button : entt::null;
		}
	}
}
