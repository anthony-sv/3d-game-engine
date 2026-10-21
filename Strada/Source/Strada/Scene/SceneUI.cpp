#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Core/Input.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <algorithm>
#include <utility>

namespace Strada
{
	UIButtonState Scene::GetUIButtonState(Entity entity) const
	{
		if (!entity || entity.GetScene() != this)
		{
			return UIButtonState::Normal;
		}
		UIButtonComponent const* button = m_Registry.try_get<UIButtonComponent>(entity.GetHandle());
		if (button == nullptr)
		{
			return UIButtonState::Normal;
		}
		if (!button->Interactable)
		{
			return UIButtonState::Disabled;
		}
		UUID const id = m_Registry.get<IDComponent>(entity.GetHandle()).ID;
		if (m_UIHovered != id)
		{
			return UIButtonState::Normal;
		}
		if (m_UIPressed == id)
		{
			return UIButtonState::Pressed;
		}
		// While another button holds the mouse, the others do not light up.
		return m_UIPressed.IsValid() ? UIButtonState::Normal : UIButtonState::Hovered;
	}

	bool Scene::WasUIButtonClicked(Entity entity) const
	{
		if (!entity || entity.GetScene() != this)
		{
			return false;
		}
		UUID const id = m_Registry.get<IDComponent>(entity.GetHandle()).ID;
		return std::find(m_UIClicked.begin(), m_UIClicked.end(), id) != m_UIClicked.end();
	}

	void Scene::ClickUIButton(Entity entity)
	{
		if (m_IsRunning && entity && entity.GetScene() == this)
		{
			m_UIPendingClicks.push_back(entity.GetUUID());
		}
	}

	void Scene::ResetUI()
	{
		m_UIHovered = UUID::Invalid();
		m_UIPressed = UUID::Invalid();
		m_UIClicked.clear();
		m_UIPendingClicks.clear();
		m_UIMouseWasDown = false;
		m_UIPointerOver = false;
	}

	void Scene::UpdateUI()
	{
		glm::vec2 const viewport(static_cast<float>(m_ViewportWidth), static_cast<float>(m_ViewportHeight));
		// A locked cursor (first-person controls) points at nothing.
		bool const pointer = m_UIPointerEnabled && Input::GetCursorMode() != CursorMode::Locked && viewport.x > 0.0f && viewport.y > 0.0f;

		UUID hovered = UUID::Invalid();
		m_UIPointerOver = false;
		if (pointer)
		{
			std::vector<UIElement> const elements = UILayout::LayoutScene(*this, viewport);
			if (UIElement const* target = UILayout::FindHitTargetAt(elements, Input::GetMousePosition()))
			{
				m_UIPointerOver = true;
				if (target->Button != entt::null && m_Registry.get<UIButtonComponent>(target->Button).Interactable)
				{
					hovered = m_Registry.get<IDComponent>(target->Button).ID;
				}
			}
		}

		// Input's pressed and released flags last one application frame, and updates can be further apart (a paused scene
		// stepped, automation setting the buttons a frame before advancing the scene): changes of the button since the latest
		// update count too.
		bool const down = Input::IsMouseButtonDown(MouseButton::Left);
		bool const pressed = Input::IsMouseButtonPressed(MouseButton::Left) || (down && !m_UIMouseWasDown);
		bool const released = Input::IsMouseButtonReleased(MouseButton::Left) || (!down && m_UIMouseWasDown);
		// A release the device did not make (the window lost the focus, the editor took the game's input back) cancels the
		// press, as other interfaces cancel a mouse capture.
		bool const canceled = released && Input::WasMouseButtonReleasedByReset(MouseButton::Left);
		m_UIMouseWasDown = down;

		std::vector<UUID> clicked;
		// A press and its release can arrive in the same frame: the press is taken first.
		if (pointer && pressed)
		{
			m_UIPressed = hovered;
		}
		if (!pointer || released || !down)
		{
			if (pointer && released && !canceled && m_UIPressed.IsValid() && m_UIPressed == hovered)
			{
				clicked.push_back(m_UIPressed);
			}
			m_UIPressed = UUID::Invalid();
		}
		m_UIHovered = hovered;

		for (UUID const id : std::exchange(m_UIPendingClicks, {}))
		{
			entt::entity const handle = FindHandle(id);
			UIButtonComponent const* button = handle != entt::null ? m_Registry.try_get<UIButtonComponent>(handle) : nullptr;
			if (button != nullptr && button->Interactable && std::find(clicked.begin(), clicked.end(), id) == clicked.end())
			{
				clicked.push_back(id);
			}
		}
		m_UIClicked = clicked;

		if (!m_RunsScripts)
		{
			return;
		}
		for (UUID const button : clicked)
		{
			// The receivers are collected first: their scripts may change the hierarchy.
			std::vector<UUID> receivers;
			for (Entity entity = GetEntityByUUID(button); entity; entity = GetParent(entity))
			{
				receivers.push_back(entity.GetUUID());
			}
			for (UUID const receiver : receivers)
			{
				ScriptEngine::InvokeOnClick(receiver, button);
				VisualScriptEngine::InvokeOnClick(receiver, button);
			}
		}
	}
}
