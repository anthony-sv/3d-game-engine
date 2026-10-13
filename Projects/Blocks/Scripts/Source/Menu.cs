using System.Collections.Generic;
using Strada;

namespace Blocks;

/// <summary>Buttons the mouse and the keyboard share. The arrow keys (and W, A, S, D) move the focus and Enter (or Space,
/// unless the menu leaves it to the game) clicks the focused button; the mouse takes the focus when it moves onto a button,
/// so a cursor left resting on one does not take it back from the keys. The focused button lights up and grows a little
/// over its glow (an image named after it with " Glow", behind it), which breathes; a button held down shrinks. Clicks come
/// back from <see cref="Update"/> whether the mouse or a key made them: a key clicks the button as the mouse does, through
/// <see cref="UIButtonComponent.Click"/>. A menu can wait a moment after it opens before it takes keys, so keys pressed for
/// the game as the menu opens do not move its focus or click it. Keys act when they go down: one held from before does
/// nothing until it is pressed again.</summary>
public sealed class Menu
{
	// The focused button's colors: a bright fill with dark letters.
	private static readonly Color s_FocusColor = new(0.06f, 0.62f, 1.0f);
	private static readonly Color s_PressedColor = new(0.02f, 0.3f, 0.62f);
	private static readonly Color s_FocusLabel = new(0.004f, 0.008f, 0.035f);
	private static readonly KeyCode[] s_PreviousKeys = [KeyCode.Up, KeyCode.W, KeyCode.Left, KeyCode.A];
	private static readonly KeyCode[] s_NextKeys = [KeyCode.Down, KeyCode.S, KeyCode.Right, KeyCode.D];
	private static readonly KeyCode[] s_ClickKeys = [KeyCode.Enter, KeyCode.KPEnter, KeyCode.Space];
	private static readonly KeyCode[] s_EnterKeys = [KeyCode.Enter, KeyCode.KPEnter];
	private const float FocusScale = 1.06f;
	private const float PressScale = 0.95f;
	// How fast buttons grow, shrink and light up: the part of the way left they go per second, exponentially.
	private const float Response = 18.0f;
	// How long a key's click shows its button held down (seconds).
	private const float KeyPressSeconds = 0.12f;
	// The focused button's glow breathes: it dims by this part of its light and back, this fast (radians per second).
	private const float GlowBreath = 0.25f;
	private const float GlowBreathSpeed = 3.2f;

	private readonly List<MenuButton> m_Buttons = [];
	private readonly System.Action m_FocusMoved;
	private readonly float m_KeyDelay;
	private readonly KeyCode[] m_ClickKeys;
	private Vector2 m_Mouse;
	private bool m_Active;
	// Seconds the open menu still waits before it takes keys.
	private float m_KeyWait;

	/// <summary>A closed menu of the buttons, in the order the keys move through them; focusMoved is called whenever the
	/// focus moves to another button. Once open, it takes keys after keyDelay seconds (the mouse at once). Without
	/// spaceClicks only Enter clicks: Space stays the game's (a menu that opens as the game ends, while the player may still be
	/// pressing it).</summary>
	public Menu(IEnumerable<Entity> buttons, System.Action focusMoved, float keyDelay = 0.0f, bool spaceClicks = true)
	{
		foreach (Entity button in buttons)
		{
			MenuButton menuButton = new(button);
			menuButton.Button.Interactable = false;
			m_Buttons.Add(menuButton);
		}
		m_FocusMoved = focusMoved;
		m_KeyDelay = keyDelay;
		m_ClickKeys = spaceClicks ? s_ClickKeys : s_EnterKeys;
	}

	/// <summary>The focused button's index.</summary>
	public int Focus { get; private set; }

	/// <summary>The focused button.</summary>
	public Entity FocusedButton => m_Buttons[Focus].Entity;

	/// <summary>Whether the menu takes clicks and keys.</summary>
	public bool IsOpen => m_Active;

	/// <summary>Opens the menu with the focus on its first button, or closes it: a closed menu takes neither clicks nor keys
	/// (while its screen fades out) and its buttons keep their look.</summary>
	public void Open(bool open)
	{
		if (open == m_Active)
		{
			return;
		}
		if (open)
		{
			Focus = 0;
			m_Mouse = Input.MousePosition;
			m_KeyWait = m_KeyDelay;
		}
		m_Active = open;
		foreach (MenuButton button in m_Buttons)
		{
			button.Button.Interactable = open;
		}
	}

	/// <summary>Follows the mouse and the keys for a frame, moves the buttons' looks on by the time step and returns the
	/// button clicked this frame, if any.</summary>
	public Entity? Update(float deltaTime)
	{
		Entity? clicked = null;
		if (m_Active)
		{
			Vector2 mouse = Input.MousePosition;
			bool mouseActs = mouse != m_Mouse || Input.IsMouseButtonPressed(MouseButton.Left);
			m_Mouse = mouse;
			for (int i = 0; i < m_Buttons.Count; i++)
			{
				UIButtonComponent button = m_Buttons[i].Button;
				if (button.WasClicked)
				{
					clicked = m_Buttons[i].Entity;
				}
				if (mouseActs && button.State is UIButtonState.Hovered or UIButtonState.Pressed)
				{
					MoveFocus(i);
				}
			}
			bool keys = m_KeyWait <= 0.0f;
			m_KeyWait -= deltaTime;
			int step = keys ? (IsPressed(s_NextKeys) ? 1 : 0) - (IsPressed(s_PreviousKeys) ? 1 : 0) : 0;
			if (step != 0)
			{
				MoveFocus((Focus + step + m_Buttons.Count) % m_Buttons.Count);
			}
			if (keys && IsPressed(m_ClickKeys))
			{
				MenuButton focused = m_Buttons[Focus];
				focused.Button.Click();
				focused.KeyPress = KeyPressSeconds;
			}
		}

		float blend = 1.0f - Mathf.Exp(-Response * deltaTime);
		float breath = 1.0f - (GlowBreath * 0.5f * (1.0f - Mathf.Cos((float)Time.Elapsed * GlowBreathSpeed)));
		for (int i = 0; i < m_Buttons.Count; i++)
		{
			m_Buttons[i].Animate(m_Active && i == Focus, m_Active, blend, breath, deltaTime);
		}
		return clicked;
	}

	private void MoveFocus(int index)
	{
		if (index != Focus)
		{
			Focus = index;
			m_FocusMoved();
		}
	}

	private static bool IsPressed(KeyCode[] keys)
	{
		foreach (KeyCode key in keys)
		{
			if (Input.IsKeyPressed(key))
			{
				return true;
			}
		}
		return false;
	}

	// A button with what it looks like at rest, and how far it has grown and lit up.
	private sealed class MenuButton
	{
		public readonly Entity Entity;
		public readonly UIButtonComponent Button;
		// Seconds a key's click still shows the button held down.
		public float KeyPress;

		private readonly UIRectComponent m_Rect;
		private readonly Vector2 m_Size;
		private readonly Color m_RestColor;
		private readonly UITextComponent? m_Label;
		private readonly float m_LabelSize;
		private readonly Color m_RestLabel;
		private readonly UIRectComponent? m_Glow;
		private readonly Vector2 m_GlowSize;
		private float m_Scale = 1.0f;
		private float m_Light;
		// What the button shows (written when it changes).
		private float m_ShownScale = 1.0f;
		private float m_ShownGlow;
		private Color m_ShownNormal;
		private bool m_ShownFocus;

		public MenuButton(Entity entity)
		{
			Entity = entity;
			Button = entity.GetComponent<UIButtonComponent>()!;
			m_Rect = entity.GetComponent<UIRectComponent>()!;
			m_Size = m_Rect.Size;
			m_RestColor = Button.NormalColor;
			// Under the mouse, a button looks as at rest until it has the focus; held down, it darkens; a closed menu's buttons
			// keep their look while their screen fades out.
			Button.HoveredColor = m_RestColor;
			Button.PressedColor = s_PressedColor;
			Button.DisabledColor = m_RestColor;
			m_Label = FindChild(entity, $"{entity.Name} Label")?.GetComponent<UITextComponent>();
			if (m_Label is not null)
			{
				m_LabelSize = m_Label.FontSize;
				m_RestLabel = m_Label.Color;
			}
			m_Glow = entity.Parent is { } parent ? FindChild(parent, $"{entity.Name} Glow")?.GetComponent<UIRectComponent>() : null;
			m_GlowSize = m_Glow?.Size ?? Vector2.Zero;
			m_ShownNormal = m_RestColor;
			m_ShownGlow = m_Glow?.Opacity ?? 0.0f;
		}

		// The focused button lights up and grows; one held down (by the mouse or a key) shrinks. A button under the mouse
		// without the focus looks as at rest. The glow shows the light times the breath. Only what changes is written to the
		// button.
		public void Animate(bool focused, bool open, float blend, float breath, float deltaTime)
		{
			KeyPress = Mathf.Max(KeyPress - deltaTime, 0.0f);
			bool held = KeyPress > 0.0f || (open && Button.State == UIButtonState.Pressed);
			m_Scale = Approach(m_Scale, held ? PressScale : focused ? FocusScale : 1.0f, blend);
			m_Light = Approach(m_Light, focused ? 1.0f : 0.0f, blend);

			Color fill = focused ? s_FocusColor : m_RestColor;
			Color normal = KeyPress > 0.0f ? s_PressedColor : fill;
			if (normal != m_ShownNormal || focused != m_ShownFocus)
			{
				m_ShownNormal = normal;
				Button.NormalColor = normal;
				Button.HoveredColor = fill;
			}
			if (focused != m_ShownFocus)
			{
				m_ShownFocus = focused;
				if (m_Label is not null)
				{
					m_Label.Color = focused ? s_FocusLabel : m_RestLabel;
				}
			}
			if (m_Scale != m_ShownScale)
			{
				m_ShownScale = m_Scale;
				m_Rect.Size = m_Size * m_Scale;
				if (m_Label is not null)
				{
					m_Label.FontSize = m_LabelSize * m_Scale;
				}
				if (m_Glow is not null)
				{
					m_Glow.Size = m_GlowSize + (m_Size * (m_Scale - 1.0f));
				}
			}
			float glow = m_Light * breath;
			if (glow != m_ShownGlow && m_Glow is not null)
			{
				m_ShownGlow = glow;
				m_Glow.Opacity = glow;
			}
		}

		// Moves a value toward its target by the blend, arriving once it is close enough not to show.
		private static float Approach(float value, float target, float blend)
		{
			float next = Mathf.Lerp(value, target, blend);
			return Mathf.Abs(next - target) < 0.001f ? target : next;
		}

		private static Entity? FindChild(Entity parent, string name)
		{
			foreach (Entity child in parent.Children)
			{
				if (child.Name == name)
				{
					return child;
				}
			}
			return null;
		}
	}
}
