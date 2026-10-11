using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>The screen-space UI: canvas, rectangles, images, text and buttons. It runs on the canvas, so it hears the clicks
/// of the buttons in it; test runs have no mouse, so the buttons are clicked in code.</summary>
public sealed class UITests : FeatureTestScript
{
	private int m_Clicks;
	private string? m_LastClicked;
	private bool m_ClickedInCallback;

	protected override void Start()
	{
		Check("canvases", CanvasChecks);
		Check("rectangles", RectChecks);
		Check("images", ImageChecks);
		Check("text", TextChecks);
		Check("buttons", ButtonChecks);
		Check("buttons clicked in code click at the next update", () =>
		{
			UIButtonComponent play = Find<UIButtonComponent>("Play Button");
			play.Click();
			Assert.IsFalse(play.WasClicked);
		});
	}

	protected override void OnClick(Entity button)
	{
		m_Clicks++;
		m_LastClicked = button.Name;
		m_ClickedInCallback = button.GetComponent<UIButtonComponent>()!.WasClicked;
	}

	protected override bool Step(int frame, float deltaTime)
	{
		UIButtonComponent play = Find<UIButtonComponent>("Play Button");
		if (frame == 1)
		{
			Check("OnClick reaches the canvas's script, WasClicked lasts the frame", () =>
			{
				Assert.AreEqual(1, m_Clicks);
				Assert.AreEqual("Play Button", m_LastClicked);
				Assert.IsTrue(m_ClickedInCallback);
				Assert.IsTrue(play.WasClicked);
			});
			play.Interactable = false;
			play.Click();
			return false;
		}
		Check("clicks last one frame, and disabled buttons take none", () =>
		{
			Assert.IsFalse(play.WasClicked);
			Assert.AreEqual(1, m_Clicks);
			Assert.AreEqual(UIButtonState.Disabled, play.State);
		});
		play.Interactable = true;
		return true;
	}

	private static T Find<T>(string entityName)
		where T : Component
	{
		T? component = Entity.FindByName(entityName)?.GetComponent<T>();
		Assert.IsNotNull(component, $"{entityName} has a {typeof(T).Name}");
		return component!;
	}

	private static void CanvasChecks()
	{
		UICanvasComponent canvas = Find<UICanvasComponent>("UI Canvas");
		Assert.AreEqual(UIScaleMode.ScaleWithScreenHeight, canvas.ScaleMode);
		Assert.AreEqual(new Vector2(1920.0f, 1080.0f), canvas.ReferenceResolution);
		canvas.ScaleMode = UIScaleMode.ConstantPixelSize;
		Assert.AreEqual(UIScaleMode.ConstantPixelSize, canvas.ScaleMode);
		canvas.ScaleMode = UIScaleMode.ScaleWithScreenHeight;
		canvas.ReferenceResolution = new Vector2(1280.0f, 720.0f);
		Assert.AreEqual(new Vector2(1280.0f, 720.0f), canvas.ReferenceResolution);
		canvas.ReferenceResolution = new Vector2(1920.0f, 1080.0f);
	}

	private static void RectChecks()
	{
		UIRectComponent rect = Find<UIRectComponent>("UI Title");
		Assert.AreEqual(new Vector2(0.0f, 0.0f), rect.AnchorMin);
		Assert.AreEqual(new Vector2(1.0f, 0.0f), rect.AnchorMax);
		Assert.AreEqual(new Vector2(0.5f, 0.0f), rect.Pivot);
		Assert.AreEqual(new Vector2(0.0f, 40.0f), rect.Position);
		Assert.AreEqual(new Vector2(-80.0f, 80.0f), rect.Size);
		Assert.IsTrue(rect.Visible);
		Assert.AreEqual(1.0f, rect.Opacity);

		rect.AnchorMin = new Vector2(0.25f, 0.5f);
		Assert.AreEqual(new Vector2(0.25f, 0.5f), rect.AnchorMin);
		rect.AnchorMax = new Vector2(0.75f, 0.5f);
		Assert.AreEqual(new Vector2(0.75f, 0.5f), rect.AnchorMax);
		rect.Pivot = new Vector2(1.0f, 1.0f);
		Assert.AreEqual(new Vector2(1.0f, 1.0f), rect.Pivot);
		rect.Position = new Vector2(-12.0f, 34.0f);
		Assert.AreEqual(new Vector2(-12.0f, 34.0f), rect.Position);
		rect.Size = new Vector2(320.0f, 64.0f);
		Assert.AreEqual(new Vector2(320.0f, 64.0f), rect.Size);
		rect.Visible = false;
		Assert.IsFalse(rect.Visible);
		rect.Opacity = 0.25f;
		Assert.AreEqual(0.25f, rect.Opacity);

		rect.AnchorMin = new Vector2(0.0f, 0.0f);
		rect.AnchorMax = new Vector2(1.0f, 0.0f);
		rect.Pivot = new Vector2(0.5f, 0.0f);
		rect.Position = new Vector2(0.0f, 40.0f);
		rect.Size = new Vector2(-80.0f, 80.0f);
		rect.Visible = true;
		rect.Opacity = 1.0f;
	}

	private static void ImageChecks()
	{
		UIImageComponent image = Find<UIImageComponent>("UI Panel");
		Assert.AreEqual(24.0f, image.CornerRadius);
		Assert.IsNull(image.Texture);
		Assert.IsFalse(image.PreserveAspect);
		Color color = image.Color;
		Assert.IsTrue(color.A > 0.5f && color.A < 1.0f, "the panel is translucent");

		image.Texture = Assets.Load<Texture>("builtin://White");
		Assert.AreEqual(Assets.Load<Texture>("builtin://White"), image.Texture);
		image.Texture = null;
		Assert.IsNull(image.Texture);
		image.PreserveAspect = true;
		Assert.IsTrue(image.PreserveAspect);
		image.PreserveAspect = false;
		image.CornerRadius = 8.0f;
		Assert.AreEqual(8.0f, image.CornerRadius);
		image.CornerRadius = 24.0f;
		image.Color = new Color(0.5f, 0.25f, 1.0f, 0.75f);
		Assert.AreEqual(new Color(0.5f, 0.25f, 1.0f, 0.75f), image.Color);
		image.Color = color;
	}

	private static void TextChecks()
	{
		UITextComponent title = Find<UITextComponent>("UI Title");
		Assert.AreEqual("Strada UI", title.Text);
		Assert.AreEqual(48.0f, title.FontSize);
		Assert.IsNull(title.Font);
		Assert.AreEqual(TextAlignment.Center, title.Alignment);
		Assert.AreEqual(TextVerticalAlignment.Middle, title.VerticalAlignment);
		Assert.IsTrue(title.Wrap);
		Assert.AreEqual(1.0f, title.LineSpacing);

		title.Text = "Héllo ✓";
		Assert.AreEqual("Héllo ✓", title.Text);
		title.Font = Assets.Load<Font>("builtin://DefaultFont");
		Assert.AreEqual(Assets.Load<Font>("builtin://DefaultFont"), title.Font);
		title.Font = null;
		title.Color = new Color(1.0f, 0.8f, 0.2f);
		Assert.AreEqual(new Color(1.0f, 0.8f, 0.2f), title.Color);
		title.FontSize = 64.0f;
		Assert.AreEqual(64.0f, title.FontSize);
		title.Alignment = TextAlignment.Right;
		Assert.AreEqual(TextAlignment.Right, title.Alignment);
		title.VerticalAlignment = TextVerticalAlignment.Bottom;
		Assert.AreEqual(TextVerticalAlignment.Bottom, title.VerticalAlignment);
		title.VerticalAlignment = TextVerticalAlignment.Top;
		Assert.AreEqual(TextVerticalAlignment.Top, title.VerticalAlignment);
		title.Wrap = false;
		Assert.IsFalse(title.Wrap);
		title.LineSpacing = 1.5f;
		Assert.AreEqual(1.5f, title.LineSpacing);

		title.Text = "Strada UI";
		title.Color = new Color(1.0f, 1.0f, 1.0f);
		title.FontSize = 48.0f;
		title.Alignment = TextAlignment.Center;
		title.VerticalAlignment = TextVerticalAlignment.Middle;
		title.Wrap = true;
		title.LineSpacing = 1.0f;
	}

	private static void ButtonChecks()
	{
		UIButtonComponent play = Find<UIButtonComponent>("Play Button");
		Assert.IsTrue(play.Interactable);
		Assert.IsFalse(play.WasClicked);
		// Test runs have no mouse over the button.
		Assert.AreEqual(UIButtonState.Normal, play.State);
		Assert.AreEqual("Play", Entity.FindByName("Play Button")!.GetComponent<UITextComponent>()!.Text);

		Color normal = play.NormalColor;
		play.NormalColor = new Color(0.1f, 0.2f, 0.3f);
		Assert.AreEqual(new Color(0.1f, 0.2f, 0.3f), play.NormalColor);
		play.NormalColor = normal;
		play.HoveredColor = new Color(0.2f, 0.3f, 0.4f);
		Assert.AreEqual(new Color(0.2f, 0.3f, 0.4f), play.HoveredColor);
		play.PressedColor = new Color(0.3f, 0.4f, 0.5f);
		Assert.AreEqual(new Color(0.3f, 0.4f, 0.5f), play.PressedColor);
		play.DisabledColor = new Color(0.4f, 0.5f, 0.6f, 0.5f);
		Assert.AreEqual(new Color(0.4f, 0.5f, 0.6f, 0.5f), play.DisabledColor);
		play.Interactable = false;
		Assert.IsFalse(play.Interactable);
		Assert.AreEqual(UIButtonState.Disabled, play.State);
		play.Interactable = true;
		Assert.AreEqual(UIButtonState.Normal, play.State);
		Assert.AreNotEqual(UIButtonState.Hovered, play.State);
		Assert.AreNotEqual(UIButtonState.Pressed, play.State);
	}
}
