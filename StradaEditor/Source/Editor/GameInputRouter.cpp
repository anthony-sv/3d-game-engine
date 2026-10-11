#include "Editor/GameInputRouter.h"

#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/Input.h"

#include <utility>

namespace Strada
{
	bool GameInputRouter::Route(Event const& event, ImGuiLayer::InputContext const& context)
	{
		switch (event.GetEventType())
		{
			case EventType::KeyPressed:
			{
				auto const& press = static_cast<KeyPressedEvent const&>(event);
				return RouteKeyPress(press.GetKeyCode(), press.IsRepeat(), context);
			}
			case EventType::KeyReleased:
			{
				KeyCode const key = static_cast<KeyReleasedEvent const&>(event).GetKeyCode();
				if (size_t const index = static_cast<size_t>(key); index < m_GameKeys.size())
				{
					m_GameKeys.reset(index);
				}
				Input::SetKeyState(key, false);
				return true;
			}
			case EventType::KeyTyped:
				// Games read keys, not text: typing reaches the editor while it has the input and nobody otherwise.
				return !m_HasInput;
			case EventType::MouseButtonPressed:
				return RouteButtonPress(static_cast<MouseButtonPressedEvent const&>(event).GetMouseButton(), context);
			case EventType::MouseButtonReleased:
				Input::SetMouseButtonState(static_cast<MouseButtonReleasedEvent const&>(event).GetMouseButton(), false);
				return true;
			case EventType::MouseMoved:
			{
				auto const& move = static_cast<MouseMovedEvent const&>(event);
				return RouteMove(glm::vec2(move.GetX(), move.GetY()));
			}
			case EventType::MouseScrolled:
			{
				auto const& scroll = static_cast<MouseScrolledEvent const&>(event);
				return RouteScroll(glm::vec2(scroll.GetXOffset(), scroll.GetYOffset()), context);
			}
			default:
				return true;
		}
	}

	void GameInputRouter::SetView(View const& view)
	{
		bool const moved = view.ImageMin != m_View.ImageMin || view.ImageSize != m_View.ImageSize || view.RenderSize != m_View.RenderSize;
		m_View = view;
		if (!m_HasInput)
		{
			return;
		}
		if (!view.Playing || !view.Focused)
		{
			Release();
			return;
		}
		// The cursor stays where it is when the view moves or resizes: the game sees it at its new pixels, without movement.
		if (moved && m_HasCursor && m_GameCursorMode != CursorMode::Locked)
		{
			Input::WarpMousePosition(ToGamePixels(m_Cursor));
		}
	}

	void GameInputRouter::Grant()
	{
		m_FocusRequested = true;
		if (m_HasInput)
		{
			return;
		}
		FlushRelease();
		m_HasInput = true;
		// The game's cursor was frozen while the editor had the input: it moves to the real one without reporting movement.
		if (m_HasCursor)
		{
			Input::WarpMousePosition(ToGamePixels(m_Cursor));
		}
	}

	void GameInputRouter::Release()
	{
		if (!m_HasInput)
		{
			return;
		}
		m_HasInput = false;
		m_ReleasePending = true;
		m_GameKeys.reset();
	}

	void GameInputRouter::BeginFrame()
	{
		FlushRelease();
	}

	bool GameInputRouter::TakeFocusRequest()
	{
		return std::exchange(m_FocusRequested, false);
	}

	bool GameInputRouter::RouteKeyPress(KeyCode key, bool repeat, ImGuiLayer::InputContext const& context)
	{
		size_t const index = static_cast<size_t>(key);
		bool const tracked = index < m_GameKeys.size();
		if (repeat)
		{
			// A held key repeats where it was pressed: the game's stay the game's and the editor's never become the game's.
			return !(tracked && m_GameKeys.test(index));
		}
		if (!m_HasInput)
		{
			return true;
		}
		if (key == KeyCode::Escape && context.Shift && !context.Control && !context.Alt && !context.Super)
		{
			Release();
			return false;
		}
		// The play shortcuts are named by the key the layout types (as ImGui's shortcuts read keys), not by its position.
		bool const shortcutModifier = m_ShortcutsUseSuper ? context.Super : context.Control;
		if (shortcutModifier && context.LayoutKey == KeyCode::P)
		{
			return true;
		}
		if (tracked)
		{
			m_GameKeys.set(index);
		}
		Input::SetKeyState(key, true);
		return false;
	}

	bool GameInputRouter::RouteButtonPress(MouseButton button, ImGuiLayer::InputContext const& context)
	{
		m_Cursor = context.Cursor;
		m_HasCursor = true;
		if (m_HasInput)
		{
			if (m_GameCursorMode == CursorMode::Locked || IsViewAt(context))
			{
				Input::SetMouseButtonState(button, true);
				return false;
			}
			// A click anywhere else is the editor's and gives it the input back.
			Release();
			return true;
		}
		if (!IsViewAt(context))
		{
			return true;
		}
		// The click that gives the game the input reaches it too; ImGui gets it as well and focuses the game view with it.
		Grant();
		Input::SetMouseButtonState(button, true);
		return true;
	}

	bool GameInputRouter::RouteMove(glm::vec2 cursor)
	{
		m_Cursor = cursor;
		m_HasCursor = true;
		if (!m_HasInput)
		{
			return true;
		}
		Input::SetMousePosition(ToGamePixels(cursor));
		// A locked cursor only turns the game's view: where it is means nothing to the editor.
		return m_GameCursorMode != CursorMode::Locked;
	}

	bool GameInputRouter::RouteScroll(glm::vec2 offset, ImGuiLayer::InputContext const& context)
	{
		if (m_HasInput && (m_GameCursorMode == CursorMode::Locked || IsViewAt(context)))
		{
			Input::AddScrollDelta(offset);
			return false;
		}
		return true;
	}

	bool GameInputRouter::IsViewAt(ImGuiLayer::InputContext const& context) const
	{
		glm::vec2 const position = context.Cursor;
		bool const inArea = position.x >= m_View.AreaMin.x && position.y >= m_View.AreaMin.y && position.x < m_View.AreaMax.x &&
		                    position.y < m_View.AreaMax.y;
		return m_View.Playing && context.Viewport == m_View.Viewport && inArea && !(m_CoverageTest && m_CoverageTest(position));
	}

	glm::vec2 GameInputRouter::ToGamePixels(glm::vec2 position) const
	{
		glm::vec2 const renderSize(static_cast<float>(m_View.RenderSize.x), static_cast<float>(m_View.RenderSize.y));
		return (position - m_View.ImageMin) * renderSize / glm::max(m_View.ImageSize, glm::vec2(1.0f));
	}

	void GameInputRouter::FlushRelease()
	{
		if (std::exchange(m_ReleasePending, false))
		{
			Input::ReleaseAll();
		}
	}
}
