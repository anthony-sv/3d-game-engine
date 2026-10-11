using System;
using System.Collections.Generic;
using System.Globalization;

namespace Strada.Tests;

// Counts the clicks it hears (OnClick, as the button or one of its ancestors) and, each update, names its entity after them,
// the state of the Watched button and whether the mouse is over the UI; clicks the watched button in code when Enter is
// pressed and ClickOnEnter is set.
public sealed class ClickRecorder : Script
{
	public Entity? Watched;
	public bool ClickOnEnter;
	private int m_Clicks;
	private ulong m_LastButton;
	private bool m_LastButtonWasClicked;

	protected override void OnClick(Entity button)
	{
		m_Clicks++;
		m_LastButton = button.ID;
		m_LastButtonWasClicked = button.GetComponent<UIButtonComponent>()!.WasClicked;
	}

	protected override void OnUpdate(float deltaTime)
	{
		UIButtonComponent watched = Watched!.GetComponent<UIButtonComponent>()!;
		if (ClickOnEnter && Input.IsKeyPressed(KeyCode.Enter))
		{
			watched.Click();
		}
		string clicks = string.Create(CultureInfo.InvariantCulture,
			$"Clicks={m_Clicks};Last={m_LastButton};InClick={m_LastButtonWasClicked};Polled={watched.WasClicked}");
		Name = string.Create(CultureInfo.InvariantCulture, $"{clicks};State={watched.State};Over={Input.IsPointerOverUI}");
	}
}

// Sets every UI component field through the API, reads each back (logging "Mismatch" when it differs) and names its entity
// "Checked <count>"; the engine tests then read the fields natively. The entity has a UIText whose font arrives in
// ExpectedFont.
public sealed class UIWriter : Script
{
	public AssetHandle ExpectedFont;
	private int m_Checked;

	protected override void OnCreate()
	{
		UICanvasComponent canvas = AddComponent<UICanvasComponent>();
		Check(() => canvas.ScaleMode, value => canvas.ScaleMode = value, UIScaleMode.ConstantPixelSize, "UICanvas.ScaleMode");
		Check(() => canvas.ReferenceResolution, value => canvas.ReferenceResolution = value, new Vector2(1280.0f, 720.0f),
			"UICanvas.ReferenceResolution");

		UIRectComponent rect = AddComponent<UIRectComponent>();
		Check(() => rect.AnchorMin, value => rect.AnchorMin = value, new Vector2(0.0f, 0.25f), "UIRect.AnchorMin");
		Check(() => rect.AnchorMax, value => rect.AnchorMax = value, new Vector2(1.0f, 0.75f), "UIRect.AnchorMax");
		Check(() => rect.Position, value => rect.Position = value, new Vector2(12.0f, -8.0f), "UIRect.Position");
		Check(() => rect.Size, value => rect.Size = value, new Vector2(-40.0f, 64.0f), "UIRect.Size");
		Check(() => rect.Pivot, value => rect.Pivot = value, new Vector2(0.0f, 1.0f), "UIRect.Pivot");
		Check(() => rect.Visible, value => rect.Visible = value, false, "UIRect.Visible");
		Check(() => rect.Opacity, value => rect.Opacity = value, 0.5f, "UIRect.Opacity");

		UIImageComponent image = AddComponent<UIImageComponent>();
		Expect(image.Texture == null, true, "UIImage.Texture");
		Check(() => image.Color, value => image.Color = value, new Color(0.25f, 0.5f, 0.75f, 0.8f), "UIImage.Color");
		Check(() => image.PreserveAspect, value => image.PreserveAspect = value, true, "UIImage.PreserveAspect");
		Check(() => image.CornerRadius, value => image.CornerRadius = value, 14.0f, "UIImage.CornerRadius");

		// The engine test gives the entity its UIText with a font.
		UITextComponent text = GetComponent<UITextComponent>()!;
		Font? font = text.Font;
		Expect(font?.Handle ?? AssetHandle.Invalid, ExpectedFont, "UIText.Font");
		text.Font = null;
		Expect(text.Font == null, true, "UIText.Font cleared");
		text.Font = font;
		Expect(text.Font?.Handle ?? AssetHandle.Invalid, ExpectedFont, "UIText.Font set");
		Check(() => text.Text, value => text.Text = value, "Start ✓", "UIText.Text");
		Check(() => text.Color, value => text.Color = value, new Color(1.0f, 0.5f, 0.0f, 1.0f), "UIText.Color");
		Check(() => text.FontSize, value => text.FontSize = value, 48.0f, "UIText.FontSize");
		Check(() => text.Alignment, value => text.Alignment = value, TextAlignment.Right, "UIText.Alignment");
		Check(() => text.VerticalAlignment, value => text.VerticalAlignment = value, TextVerticalAlignment.Bottom, "UIText.VerticalAlignment");
		Check(() => text.Wrap, value => text.Wrap = value, false, "UIText.Wrap");
		Check(() => text.LineSpacing, value => text.LineSpacing = value, 1.25f, "UIText.LineSpacing");

		UIButtonComponent button = AddComponent<UIButtonComponent>();
		Expect(button.State, UIButtonState.Normal, "UIButton.State");
		Expect(button.WasClicked, false, "UIButton.WasClicked");
		Check(() => button.NormalColor, value => button.NormalColor = value, new Color(0.1f, 0.2f, 0.3f), "UIButton.NormalColor");
		Check(() => button.HoveredColor, value => button.HoveredColor = value, new Color(0.2f, 0.3f, 0.4f), "UIButton.HoveredColor");
		Check(() => button.PressedColor, value => button.PressedColor = value, new Color(0.3f, 0.4f, 0.5f), "UIButton.PressedColor");
		Check(() => button.DisabledColor, value => button.DisabledColor = value, new Color(0.4f, 0.5f, 0.6f, 0.5f), "UIButton.DisabledColor");
		Check(() => button.Interactable, value => button.Interactable = value, false, "UIButton.Interactable");
		Expect(button.State, UIButtonState.Disabled, "UIButton.State disabled");

		Name = string.Create(CultureInfo.InvariantCulture, $"Checked {m_Checked}");
	}

	private void Check<T>(Func<T> get, Action<T> set, T value, string field)
	{
		set(value);
		Expect(get(), value, field);
	}

	private void Expect<T>(T actual, T expected, string field)
	{
		m_Checked++;
		if (!EqualityComparer<T>.Default.Equals(actual, expected))
		{
			Log.Error($"Mismatch: {field} is {actual}, expected {expected}");
		}
	}
}
