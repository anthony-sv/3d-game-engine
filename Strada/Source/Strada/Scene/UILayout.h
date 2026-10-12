#pragma once

#include "Strada/Scene/Components.h"

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Strada
{
	class Scene;

	// An axis-aligned rectangle in pixels of the viewport, from its top-left corner with y downwards.
	struct UIRectangle
	{
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);

		glm::vec2 GetSize() const { return Max - Min; }
		glm::vec2 GetCenter() const { return 0.5f * (Min + Max); }
		// Includes the top and left edges and excludes the bottom and right ones, so neighbors never share a point.
		bool Contains(glm::vec2 point) const { return point.x >= Min.x && point.y >= Min.y && point.x < Max.x && point.y < Max.y; }

		bool operator==(UIRectangle const& other) const = default;
	};

	// The state a UIButton is drawn in (its color).
	enum class UIButtonState : uint8_t
	{
		Normal = 0,
		Hovered,
		Pressed,
		Disabled
	};

	// An element of a laid-out screen-space UI: a canvas or one of its descendants.
	struct UIElement
	{
		entt::entity Handle = entt::null;
		// The canvas the element belongs to (itself for canvases).
		entt::entity Canvas = entt::null;
		UIRectangle Rectangle;
		// Pixels per canvas unit of the element's canvas.
		float Scale = 1.0f;
		// The product of the UIRect opacities of the element and its ancestors.
		float Opacity = 1.0f;
		// Takes the clicks over its rectangle: the element has a UIImage or a UIButton and is not fully transparent.
		bool HitTarget = false;
		// The nearest UIButton among the element and its ancestors up to its canvas; null when there is none.
		entt::entity Button = entt::null;
	};

	// Screen-space UI layout: pure functions of the components, the hierarchy and the viewport's size.
	namespace UILayout
	{
		// Pixels per canvas unit on a viewport of the given size (in pixels).
		float GetCanvasScale(UICanvasComponent const& canvas, glm::vec2 viewportSize);
		// The rectangle of a UIRect in its parent's rectangle, with scale pixels per canvas unit. A negative size makes an empty
		// rectangle at the pivot's place; values that overflow make an empty one at the parent's corner.
		UIRectangle ComputeRectangle(UIRectComponent const& rect, UIRectangle const& parent, float scale);
		// The scene's top-level canvases (those without a canvas among their ancestors) in hierarchy order, the order their
		// UIs are drawn in. Only the canvases' ancestors are visited, so large scenes with a small UI stay cheap.
		std::vector<entt::entity> GetCanvases(Scene& scene);
		// Lays out a top-level canvas's UI on a viewport of the given size and appends it to elements: the canvas, then its
		// descendants in hierarchy order (parents before their children, siblings in order), the order they are drawn in.
		// Canvases nested in it start again from the screen. Hidden elements and their descendants are left out.
		void LayoutCanvas(Scene& scene, entt::entity canvas, glm::vec2 viewportSize, std::vector<UIElement>& elements);
		// Lays out every canvas of the scene (GetCanvases, then LayoutCanvas) on a viewport of the given size.
		std::vector<UIElement> LayoutScene(Scene& scene, glm::vec2 viewportSize);
		// The topmost element that takes clicks at a point (the last drawn hit target whose rectangle contains it), or null.
		UIElement const* FindHitTargetAt(std::span<UIElement const> elements, glm::vec2 point);
		// The button the mouse is over at a point: the nearest button of the topmost hit target there (images inside a button
		// pass on its clicks, other images covering it keep them). Null when the topmost hit target has no button, or there is
		// none at the point. Disabled buttons are returned too (they keep the clicks from what they cover).
		entt::entity FindButtonAt(std::span<UIElement const> elements, glm::vec2 point);
	}
}
