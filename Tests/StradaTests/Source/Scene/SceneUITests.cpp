#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <string>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	// Input is global: tests that feed it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// An 800 x 600 menu in pixels: a play button in the middle with an icon in it, a quit button half covered by an image,
	// and a disabled button.
	struct Menu
	{
		Scene MenuScene{"Menu"};
		Entity Canvas;
		Entity Play;
		Entity Icon;
		Entity Quit;
		Entity Cover;
		Entity Locked;

		Menu()
		{
			MenuScene.OnViewportResize(800, 600);
			Canvas = MenuScene.CreateEntity("Canvas");
			Canvas.AddComponent<UICanvasComponent>().ScaleMode = UIScaleMode::ConstantPixelSize;
			// (300, 270) to (500, 330).
			Play = AddButton("Play", {0.0f, 0.0f}, {200.0f, 60.0f});
			Icon = MenuScene.CreateEntity("Icon", Play);
			UIRectComponent& icon = Icon.AddComponent<UIRectComponent>();
			icon.AnchorMin = icon.AnchorMax = {0.0f, 0.5f};
			icon.Pivot = {0.0f, 0.5f};
			icon.Position = {10.0f, 0.0f};
			icon.Size = {40.0f, 40.0f};
			Icon.AddComponent<UIImageComponent>();
			// (300, 370) to (500, 430), its right half covered.
			Quit = AddButton("Quit", {0.0f, 100.0f}, {200.0f, 60.0f});
			Cover = MenuScene.CreateEntity("Cover", Canvas);
			UIRectComponent& cover = Cover.AddComponent<UIRectComponent>();
			cover.Position = {50.0f, 100.0f};
			cover.Size = {100.0f, 60.0f};
			Cover.AddComponent<UIImageComponent>();
			// (300, 470) to (500, 530).
			Locked = AddButton("Locked", {0.0f, 200.0f}, {200.0f, 60.0f});
			Locked.GetComponent<UIButtonComponent>().Interactable = false;
		}

		Entity AddButton(char const* name, glm::vec2 position, glm::vec2 size)
		{
			Entity button = MenuScene.CreateEntity(name, Canvas);
			UIRectComponent& rect = button.AddComponent<UIRectComponent>();
			rect.Position = position;
			rect.Size = size;
			button.AddComponent<UIImageComponent>();
			button.AddComponent<UIButtonComponent>();
			return button;
		}

		// One runtime frame with the mouse at a point and its left button held or not.
		void Frame(glm::vec2 mouse, bool down)
		{
			Input::SetMousePosition(mouse);
			Input::SetMouseButtonState(MouseButton::Left, down);
			MenuScene.OnUpdateRuntime(Timestep(Step));
			Input::EndFrame();
		}
	};
}

TEST_CASE("Scene: the mouse hovers, presses and clicks UI buttons")
{
	InputScope input;
	Menu menu;
	Scene& scene = menu.MenuScene;
	// Without running, buttons are at rest (or disabled).
	CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
	CHECK(scene.GetUIButtonState(menu.Locked) == UIButtonState::Disabled);
	CHECK(scene.GetUIButtonState(menu.Canvas) == UIButtonState::Normal);
	scene.OnRuntimeStart();

	SUBCASE("a press and a release over a button click it for one frame")
	{
		menu.Frame({400.0f, 300.0f}, false);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Hovered);
		CHECK(scene.GetUIButtonState(menu.Quit) == UIButtonState::Normal);
		menu.Frame({400.0f, 300.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Pressed);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
		menu.Frame({410.0f, 310.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Pressed);
		menu.Frame({410.0f, 310.0f}, false);
		CHECK(scene.WasUIButtonClicked(menu.Play));
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Quit));
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Hovered);
		menu.Frame({410.0f, 310.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
	}

	SUBCASE("a press and release within one frame click")
	{
		Input::SetMousePosition({400.0f, 300.0f});
		Input::SetMouseButtonState(MouseButton::Left, true);
		Input::SetMouseButtonState(MouseButton::Left, false);
		scene.OnUpdateRuntime(Timestep(Step));
		CHECK(scene.WasUIButtonClicked(menu.Play));
	}

	SUBCASE("presses and releases count between updates after Input's one-frame flags passed")
	{
		// As when a paused scene is stepped, or automation sets the button a frame before it advances the scene.
		Input::SetMousePosition({400.0f, 300.0f});
		Input::SetMouseButtonState(MouseButton::Left, true);
		Input::EndFrame();
		scene.OnUpdateRuntime(Timestep(Step));
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Pressed);
		// Held through more updates, the button is not pressed again.
		Input::EndFrame();
		scene.OnUpdateRuntime(Timestep(Step));
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Pressed);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
		Input::SetMouseButtonState(MouseButton::Left, false);
		Input::EndFrame();
		scene.OnUpdateRuntime(Timestep(Step));
		CHECK(scene.WasUIButtonClicked(menu.Play));
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Hovered);
	}

	SUBCASE("images inside a button pass on its clicks")
	{
		menu.Frame({320.0f, 300.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Pressed);
		menu.Frame({320.0f, 300.0f}, false);
		CHECK(scene.WasUIButtonClicked(menu.Play));
	}

	SUBCASE("a click needs the press and the release on the same button")
	{
		// Pressed on the button, released elsewhere.
		menu.Frame({400.0f, 300.0f}, true);
		menu.Frame({50.0f, 50.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
		menu.Frame({50.0f, 50.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
		// Pressed elsewhere, released on the button.
		menu.Frame({50.0f, 50.0f}, true);
		menu.Frame({400.0f, 300.0f}, true);
		menu.Frame({400.0f, 300.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
		// Pressed on one button, released on another; the other does not light up meanwhile.
		menu.Frame({400.0f, 300.0f}, true);
		menu.Frame({320.0f, 400.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Quit) == UIButtonState::Normal);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
		menu.Frame({320.0f, 400.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Quit));
	}

	SUBCASE("the topmost element takes the click")
	{
		menu.Frame({320.0f, 400.0f}, true);
		menu.Frame({320.0f, 400.0f}, false);
		CHECK(scene.WasUIButtonClicked(menu.Quit));
		// The image over the button's right half keeps the clicks there.
		menu.Frame({480.0f, 400.0f}, false);
		CHECK(scene.GetUIButtonState(menu.Quit) == UIButtonState::Normal);
		menu.Frame({480.0f, 400.0f}, true);
		menu.Frame({480.0f, 400.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Quit));
	}

	SUBCASE("disabled, hidden and transparent buttons take no clicks")
	{
		menu.Frame({400.0f, 500.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Locked) == UIButtonState::Disabled);
		menu.Frame({400.0f, 500.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Locked));

		menu.Play.GetComponent<UIRectComponent>().Visible = false;
		menu.Frame({400.0f, 300.0f}, true);
		menu.Frame({400.0f, 300.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));

		menu.Play.GetComponent<UIRectComponent>().Visible = true;
		menu.Play.GetComponent<UIRectComponent>().Opacity = 0.0f;
		menu.Frame({400.0f, 300.0f}, true);
		menu.Frame({400.0f, 300.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
	}

	SUBCASE("a locked cursor or a disabled pointer drives no buttons")
	{
		Input::SetCursorMode(CursorMode::Locked);
		menu.Frame({400.0f, 300.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
		menu.Frame({400.0f, 300.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
		Input::SetCursorMode(CursorMode::Normal);

		scene.SetUIPointerEnabled(false);
		CHECK_FALSE(scene.IsUIPointerEnabled());
		menu.Frame({400.0f, 300.0f}, false);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
		scene.SetUIPointerEnabled(true);
		CHECK(scene.IsUIPointerEnabled());
		// Taking the pointer away while a button is held cancels the press.
		menu.Frame({400.0f, 300.0f}, true);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Pressed);
		scene.SetUIPointerEnabled(false);
		menu.Frame({400.0f, 300.0f}, true);
		scene.SetUIPointerEnabled(true);
		menu.Frame({400.0f, 300.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Play));
	}

	SUBCASE("buttons clicked in code click at the next update unless disabled")
	{
		scene.ClickUIButton(menu.Quit);
		scene.ClickUIButton(menu.Locked);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Quit));
		menu.Frame({0.0f, 0.0f}, false);
		CHECK(scene.WasUIButtonClicked(menu.Quit));
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Locked));
		menu.Frame({0.0f, 0.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Quit));
		// Entities without a button are ignored.
		scene.ClickUIButton(menu.Cover);
		menu.Frame({0.0f, 0.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Cover));
	}

	SUBCASE("stopping forgets the pointer and pending clicks")
	{
		menu.Frame({400.0f, 300.0f}, true);
		scene.ClickUIButton(menu.Quit);
		scene.OnRuntimeStop();
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
		scene.OnRuntimeStart();
		menu.Frame({0.0f, 0.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Quit));
		// Clicks requested while stopped are ignored.
		scene.OnRuntimeStop();
		scene.ClickUIButton(menu.Quit);
		scene.OnRuntimeStart();
		menu.Frame({0.0f, 0.0f}, false);
		CHECK_FALSE(scene.WasUIButtonClicked(menu.Quit));
	}

	SUBCASE("the layout follows the viewport's size")
	{
		scene.OnViewportResize(1600, 1200);
		menu.Frame({400.0f, 300.0f}, false);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
		menu.Frame({800.0f, 600.0f}, false);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Hovered);
		// Without a viewport size there is nothing to point at.
		scene.OnViewportResize(0, 0);
		menu.Frame({800.0f, 600.0f}, false);
		CHECK(scene.GetUIButtonState(menu.Play) == UIButtonState::Normal);
	}
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: clicks reach the scripts of the button and its ancestors")
{
	InputScope input;
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Menu menu;
	Scene& scene = menu.MenuScene;
	auto const addRecorder = [](Entity entity, Entity watched, bool clickOnEnter)
	{
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = "Strada.Tests.ClickRecorder";
		script.Fields["Watched"] = ScriptFieldValue::FromEntity(watched.GetUUID());
		script.Fields["ClickOnEnter"] = ScriptFieldValue::FromBool(clickOnEnter);
	};
	addRecorder(menu.Canvas, menu.Quit, true);
	addRecorder(menu.Play, menu.Play, false);
	// The icon's script hears nothing: it is inside the button, not one of its ancestors.
	addRecorder(menu.Icon, menu.Play, false);
	std::string const play = std::to_string(menu.Play.GetUUID().GetValue());
	std::string const quit = std::to_string(menu.Quit.GetUUID().GetValue());

	scene.OnRuntimeStart();
	menu.Frame({400.0f, 300.0f}, false);
	CHECK(menu.Play.GetName() == "Clicks=0;Last=0;InClick=False;Polled=False;State=Hovered");
	menu.Frame({400.0f, 300.0f}, true);
	CHECK(menu.Play.GetName() == "Clicks=0;Last=0;InClick=False;Polled=False;State=Pressed");
	menu.Frame({400.0f, 300.0f}, false);
	// OnClick runs before the update, which sees WasClicked too.
	CHECK(menu.Play.GetName() == "Clicks=1;Last=" + play + ";InClick=True;Polled=True;State=Hovered");
	CHECK(menu.Canvas.GetName() == "Clicks=1;Last=" + play + ";InClick=True;Polled=False;State=Normal");
	CHECK(menu.Icon.GetName() == "Clicks=0;Last=0;InClick=False;Polled=True;State=Hovered");

	// The canvas's script clicks the quit button in code when Enter is pressed: the click comes at the next update.
	Input::SetKeyState(KeyCode::Enter, true);
	menu.Frame({0.0f, 0.0f}, false);
	Input::SetKeyState(KeyCode::Enter, false);
	CHECK(menu.Canvas.GetName() == "Clicks=1;Last=" + play + ";InClick=True;Polled=False;State=Normal");
	menu.Frame({0.0f, 0.0f}, false);
	CHECK(menu.Canvas.GetName() == "Clicks=2;Last=" + quit + ";InClick=True;Polled=True;State=Normal");
	CHECK(menu.Play.GetName() == "Clicks=1;Last=" + play + ";InClick=True;Polled=False;State=Normal");
	scene.OnRuntimeStop();
	CHECK_FALSE(Testing::WasLogged(logStart, "Exception"));
}

TEST_CASE("Scene: UI components round-trip through scene files and reject values out of range")
{
	Scene scene("UI");
	Entity canvas = scene.CreateEntity("Canvas");
	UICanvasComponent& canvasComponent = canvas.AddComponent<UICanvasComponent>();
	canvasComponent.ScaleMode = UIScaleMode::ConstantPixelSize;
	canvasComponent.ReferenceResolution = {1280.0f, 720.0f};
	Entity button = scene.CreateEntity("Button", canvas);
	UIRectComponent& rect = button.AddComponent<UIRectComponent>();
	rect.AnchorMin = {0.0f, 0.25f};
	rect.AnchorMax = {1.0f, 0.75f};
	rect.Position = {12.0f, -8.0f};
	rect.Size = {-40.0f, 64.0f};
	rect.Pivot = {0.0f, 1.0f};
	rect.Visible = false;
	rect.Opacity = 0.5f;
	UIImageComponent& image = button.AddComponent<UIImageComponent>();
	image.Color = {0.25f, 0.5f, 0.75f, 0.8f};
	image.PreserveAspect = true;
	image.CornerRadius = 14.0f;
	UITextComponent& text = button.AddComponent<UITextComponent>();
	text.Text = "Start\nGame";
	text.FontSize = 48.0f;
	text.Alignment = TextAlignment::Right;
	text.VerticalAlignment = TextVerticalAlignment::Bottom;
	text.Wrap = false;
	text.LineSpacing = 1.25f;
	UIButtonComponent& buttonComponent = button.AddComponent<UIButtonComponent>();
	buttonComponent.Interactable = false;
	buttonComponent.HoveredColor = {1.0f, 0.0f, 0.0f, 1.0f};

	Json const json = SceneSerializer::Serialize(scene);
	Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(json, DeserializationContext{});
	REQUIRE(loaded.IsOk());
	Entity const loadedButton = loaded.GetValue()->FindEntityByName("Button");
	REQUIRE(loadedButton);
	UIRectComponent const& loadedRect = loadedButton.GetComponent<UIRectComponent>();
	CHECK(loadedRect.AnchorMin == rect.AnchorMin);
	CHECK(loadedRect.AnchorMax == rect.AnchorMax);
	CHECK(loadedRect.Position == rect.Position);
	CHECK(loadedRect.Size == rect.Size);
	CHECK(loadedRect.Pivot == rect.Pivot);
	CHECK_FALSE(loadedRect.Visible);
	CHECK(loadedRect.Opacity == 0.5f);
	UIImageComponent const& loadedImage = loadedButton.GetComponent<UIImageComponent>();
	CHECK(loadedImage.Color == image.Color);
	CHECK(loadedImage.PreserveAspect);
	CHECK(loadedImage.CornerRadius == 14.0f);
	UITextComponent const& loadedText = loadedButton.GetComponent<UITextComponent>();
	CHECK(loadedText.Text == "Start\nGame");
	CHECK(loadedText.FontSize == 48.0f);
	CHECK(loadedText.Alignment == TextAlignment::Right);
	CHECK(loadedText.VerticalAlignment == TextVerticalAlignment::Bottom);
	CHECK_FALSE(loadedText.Wrap);
	CHECK(loadedText.LineSpacing == 1.25f);
	UIButtonComponent const& loadedButtonComponent = loadedButton.GetComponent<UIButtonComponent>();
	CHECK_FALSE(loadedButtonComponent.Interactable);
	CHECK(loadedButtonComponent.HoveredColor == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	CHECK(loadedButtonComponent.NormalColor == UIButtonComponent().NormalColor);
	UICanvasComponent const& loadedCanvas = loaded.GetValue()->FindEntityByName("Canvas").GetComponent<UICanvasComponent>();
	CHECK(loadedCanvas.ScaleMode == UIScaleMode::ConstantPixelSize);
	CHECK(loadedCanvas.ReferenceResolution == glm::vec2(1280.0f, 720.0f));

	// Values outside the fields' ranges, and unknown enumerators, are refused.
	entt::registry& registry = scene.GetRegistry();
	ComponentInfo const* rectInfo = ComponentRegistry::Find("UIRect");
	ComponentInfo const* canvasInfo = ComponentRegistry::Find("UICanvas");
	ComponentInfo const* textInfo = ComponentRegistry::Find("UIText");
	REQUIRE(rectInfo != nullptr);
	REQUIRE(canvasInfo != nullptr);
	REQUIRE(textInfo != nullptr);
	CHECK(rectInfo->Deserialize(registry, button.GetHandle(), Json::parse(R"({"Opacity": 1.5})"), DeserializationContext{}).IsError());
	CHECK(rectInfo->Deserialize(registry, button.GetHandle(), Json::parse(R"({"AnchorMin": [-0.5, 0]})"), DeserializationContext{})
	          .IsError());
	CHECK(canvasInfo->Deserialize(registry, canvas.GetHandle(), Json::parse(R"({"ReferenceResolution": [0, 0]})"), DeserializationContext{})
	          .IsError());
	CHECK(canvasInfo->Deserialize(registry, canvas.GetHandle(), Json::parse(R"({"ScaleMode": "Stretch"})"), DeserializationContext{})
	          .IsError());
	CHECK(textInfo->Deserialize(registry, button.GetHandle(), Json::parse(R"({"FontSize": 0})"), DeserializationContext{}).IsError());
	CHECK(textInfo->Deserialize(registry, button.GetHandle(), Json::parse(R"({"VerticalAlignment": "Center"})"), DeserializationContext{})
	          .IsError());
	CHECK(button.GetComponent<UIRectComponent>().Opacity == 0.5f);
	CHECK(textInfo
	          ->Deserialize(registry, button.GetHandle(), Json::parse(R"({"VerticalAlignment": "Middle", "FontSize": 20})"),
	                        DeserializationContext{})
	          .IsOk());
	CHECK(button.GetComponent<UITextComponent>().VerticalAlignment == TextVerticalAlignment::Middle);
}

TEST_CASE("Scene: scripts read and write every UI component field")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("UI fields");
	AssetHandle const font = GetBuiltInHandle(BuiltInAsset::DefaultFont);
	Entity writer = scene.CreateEntity("Writer");
	ScriptComponent& script = writer.AddComponent<ScriptComponent>();
	script.ClassName = "Strada.Tests.UIWriter";
	script.Fields["ExpectedFont"] = ScriptFieldValue::FromAsset(font);
	writer.AddComponent<UITextComponent>().Font = font;

	scene.OnRuntimeStart();
	CHECK_FALSE(Testing::WasLogged(logStart, "Mismatch"));
	CHECK(writer.GetName() == "Checked 31");

	UICanvasComponent const& canvas = writer.GetComponent<UICanvasComponent>();
	CHECK(canvas.ScaleMode == UIScaleMode::ConstantPixelSize);
	CHECK(canvas.ReferenceResolution == glm::vec2(1280.0f, 720.0f));
	UIRectComponent const& rect = writer.GetComponent<UIRectComponent>();
	CHECK(rect.AnchorMin == glm::vec2(0.0f, 0.25f));
	CHECK(rect.AnchorMax == glm::vec2(1.0f, 0.75f));
	CHECK(rect.Position == glm::vec2(12.0f, -8.0f));
	CHECK(rect.Size == glm::vec2(-40.0f, 64.0f));
	CHECK(rect.Pivot == glm::vec2(0.0f, 1.0f));
	CHECK_FALSE(rect.Visible);
	CHECK(rect.Opacity == 0.5f);
	UIImageComponent const& image = writer.GetComponent<UIImageComponent>();
	CHECK(image.Color == glm::vec4(0.25f, 0.5f, 0.75f, 0.8f));
	CHECK_FALSE(image.Texture.IsValid());
	CHECK(image.PreserveAspect);
	CHECK(image.CornerRadius == 14.0f);
	UITextComponent const& text = writer.GetComponent<UITextComponent>();
	CHECK(text.Text == "Start \xE2\x9C\x93");
	CHECK(text.Font == font);
	CHECK(text.Color == glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
	CHECK(text.FontSize == 48.0f);
	CHECK(text.Alignment == TextAlignment::Right);
	CHECK(text.VerticalAlignment == TextVerticalAlignment::Bottom);
	CHECK_FALSE(text.Wrap);
	CHECK(text.LineSpacing == 1.25f);
	UIButtonComponent const& button = writer.GetComponent<UIButtonComponent>();
	CHECK_FALSE(button.Interactable);
	CHECK(button.NormalColor == glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
	CHECK(button.HoveredColor == glm::vec4(0.2f, 0.3f, 0.4f, 1.0f));
	CHECK(button.PressedColor == glm::vec4(0.3f, 0.4f, 0.5f, 1.0f));
	CHECK(button.DisabledColor == glm::vec4(0.4f, 0.5f, 0.6f, 0.5f));
	scene.OnRuntimeStop();
}
