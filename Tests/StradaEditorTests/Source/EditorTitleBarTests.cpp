#include "HeadlessImGui.h"

#include "Editor/UI/EditorTitleBar.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <optional>
#include <string>

using namespace Strada;

namespace
{
	// The editor's bar as EditorLayer draws it: two menus between Begin and End, in a context styled as the editor's.
	struct TitleBarFixture
	{
		TitleBarFixture() { Gui.UseEditorStyle(); }

		void Draw()
		{
			FrameHeight = ImGui::GetFrameHeight();
			TitleBarHeight = EditorTitleBar::GetHeight();
			if (!Bar.Begin(BarWindow))
			{
				return;
			}
			if (OpenFileMenu)
			{
				// As editor.entity-menu opens the Entity menu: by the ID the bar's menu has in the bar.
				ImGui::OpenPopup("File");
				OpenFileMenu = false;
			}
			FileMenuOpen = ImGui::BeginMenu("File");
			if (FileMenuOpen)
			{
				MenuFrameHeight = ImGui::GetFrameHeight();
				MenuTop = ImGui::GetWindowPos().y;
				ImGui::MenuItem("Open Project...");
				MenuItemHeight = ImGui::GetItemRectSize().y;
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				ImGui::MenuItem("Undo");
				ImGui::EndMenu();
			}
			Bar.End(Title, Status);
			if (FloatingWindow)
			{
				ImGui::SetNextWindowPos(FloatingMin);
				ImGui::SetNextWindowSize(ImVec2(240.0f, 160.0f));
				ImGui::Begin("Floating", nullptr, ImGuiWindowFlags_NoSavedSettings);
				ImGui::End();
			}
		}

		void Frames(int count = 3)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Gui.Frame(
					[this]
					{
						Draw();
					});
			}
		}

		TitleBarArea FindArea(float x, float y) const { return Bar.FindArea(glm::ivec2(static_cast<int>(x), static_cast<int>(y))); }

		// The middle of a part of the bar.
		static ImVec2 Center(EditorTitleBar::Rect const& rect)
		{
			return ImVec2((rect.Min.x + rect.Max.x) * 0.5f, (rect.Min.y + rect.Max.y) * 0.5f);
		}

		Testing::HeadlessImGui Gui;
		EditorTitleBar Bar;
		std::optional<EditorTitleBarWindow> BarWindow = EditorTitleBarWindow{};
		std::string Title = "Main* - Game - Strada Editor";
		std::string Status = "Automation: off";
		bool OpenFileMenu = false;
		bool FloatingWindow = false;
		ImVec2 FloatingMin = ImVec2(600.0f, 16.0f);

		// What the last frame saw.
		float FrameHeight = 0.0f;
		float TitleBarHeight = 0.0f;
		bool FileMenuOpen = false;
		float MenuFrameHeight = 0.0f;
		float MenuTop = 0.0f;
		float MenuItemHeight = 0.0f;
	};
}

TEST_CASE("EditorTitleBar: without a window to be the title bar of, it is the plain menu bar with the status at its right")
{
	TitleBarFixture fixture;
	fixture.BarWindow.reset();
	fixture.Frames();

	EditorTitleBar::Layout const& layout = fixture.Bar.GetLayout();
	ImVec2 const display = ImGui::GetIO().DisplaySize;
	CHECK(layout.Bar.Min.x == 0.0f);
	CHECK(layout.Bar.Min.y == 0.0f);
	CHECK(layout.Bar.Max.x == display.x);
	CHECK(layout.Bar.Max.y == doctest::Approx(fixture.FrameHeight));
	// ImGui's main menu bar, which the editor's keys know by name.
	ImGuiWindow const* const bar = ImGui::FindWindowByName("##MainMenuBar");
	REQUIRE(bar != nullptr);
	CHECK(bar->Size.y == doctest::Approx(fixture.FrameHeight));

	CHECK_FALSE(layout.Menus.IsEmpty());
	CHECK_FALSE(layout.Status.IsEmpty());
	CHECK(layout.Status.Max.x <= display.x);
	CHECK(layout.Status.Max.x > display.x - ImGui::GetStyle().ItemSpacing.x * 4.0f);
	CHECK(layout.Title.IsEmpty());
	CHECK(layout.Minimize.IsEmpty());
	CHECK(layout.Maximize.IsEmpty());
	CHECK(layout.Close.IsEmpty());

	// Nothing of it is the window's: the system has the title bar.
	ImVec2 const middle = TitleBarFixture::Center(layout.Bar);
	CHECK(fixture.FindArea(middle.x, middle.y) == TitleBarArea::Client);
	CHECK(fixture.FindArea(display.x - 4.0f, middle.y) == TitleBarArea::Client);
	CHECK(fixture.FindArea(4.0f, middle.y) == TitleBarArea::Client);
}

TEST_CASE("EditorTitleBar: as a title bar it is higher, with the title in its middle and the window's buttons in its corner")
{
	TitleBarFixture fixture;
	fixture.Frames();

	EditorTitleBar::Layout const& layout = fixture.Bar.GetLayout();
	ImVec2 const display = ImGui::GetIO().DisplaySize;
	CHECK(fixture.TitleBarHeight > fixture.FrameHeight * 1.25f);
	// Whole pixels, the same number above and below the text.
	CHECK(fixture.TitleBarHeight == std::round(fixture.TitleBarHeight));
	CHECK(std::fmod(fixture.TitleBarHeight - ImGui::GetStyle().FontSizeBase, 2.0f) == 0.0f);
	CHECK(layout.Bar.Min.x == 0.0f);
	CHECK(layout.Bar.Min.y == 0.0f);
	CHECK(layout.Bar.Max.x == display.x);
	CHECK(layout.Bar.Max.y == fixture.TitleBarHeight);
	ImGuiWindow const* const bar = ImGui::FindWindowByName("##MainMenuBar");
	REQUIRE(bar != nullptr);
	CHECK(bar->Size.y == fixture.TitleBarHeight);
	// The rest of the window starts under it.
	CHECK(ImGui::GetMainViewport()->WorkPos.y == fixture.TitleBarHeight);

	// The buttons fill the corner, side by side, wider than high as the system's are.
	CHECK(layout.Close.Max.x == display.x);
	CHECK(layout.Close.Min.y == 0.0f);
	CHECK(layout.Close.Max.y == fixture.TitleBarHeight);
	CHECK(layout.Maximize.Max.x == layout.Close.Min.x);
	CHECK(layout.Minimize.Max.x == layout.Maximize.Min.x);
	float const width = layout.Close.Max.x - layout.Close.Min.x;
	CHECK(width > fixture.TitleBarHeight);
	CHECK(layout.Maximize.Max.x - layout.Maximize.Min.x == width);
	CHECK(layout.Minimize.Max.x - layout.Minimize.Min.x == width);

	// Menus, title, status and buttons from left to right, none over another; the title in the bar's middle.
	CHECK(layout.Menus.Min.x >= 0.0f);
	CHECK(layout.Menus.Max.x < layout.Title.Min.x);
	CHECK(layout.Title.Max.x < layout.Status.Min.x);
	CHECK(layout.Status.Max.x < layout.Minimize.Min.x);
	CHECK(TitleBarFixture::Center(layout.Title).x == doctest::Approx(display.x * 0.5f).epsilon(0.002));
	CHECK(TitleBarFixture::Center(layout.Title).y == doctest::Approx(fixture.TitleBarHeight * 0.5f).epsilon(0.05));
	CHECK(TitleBarFixture::Center(layout.Status).y == doctest::Approx(fixture.TitleBarHeight * 0.5f).epsilon(0.05));

	SUBCASE("it draws every state of the window and its buttons")
	{
		for (TitleBarArea const button : {TitleBarArea::Minimize, TitleBarArea::Maximize, TitleBarArea::Close})
		{
			for (bool const maximized : {false, true})
			{
				fixture.BarWindow =
					EditorTitleBarWindow{.Maximized = maximized, .Focused = !maximized, .HoveredButton = button, .PressedButton = button};
				fixture.Frames(1);
				fixture.BarWindow->PressedButton = TitleBarArea::Client;
				fixture.Frames(1);
			}
		}
		// The states change what is drawn, never where.
		CHECK(fixture.Bar.GetLayout().Close.Max.x == display.x);
		CHECK(fixture.Bar.GetLayout().Bar.Max.y == fixture.TitleBarHeight);
	}

	SUBCASE("what does not fit a narrow window gives way: the title, then the status, never the buttons")
	{
		float const buttons = width * 3.0f;
		float const menus = layout.Menus.Max.x;
		float const title = layout.Title.Max.x - layout.Title.Min.x;
		float const status = layout.Status.Max.x - layout.Status.Min.x;
		float const spacing = ImGui::GetStyle().ItemSpacing.x;
		// Room for the menus, the status, the buttons and a little more than half of the title: the title is shortened,
		// between the menus and the status, off the window's middle.
		float const shortened = std::round(menus + title * 0.6f + status + buttons + spacing * 6.0f);
		fixture.Gui.SetDisplaySize(ImVec2(shortened, 600.0f));
		fixture.Frames();
		EditorTitleBar::Layout narrow = fixture.Bar.GetLayout();
		CHECK_FALSE(narrow.Status.IsEmpty());
		CHECK_FALSE(narrow.Title.IsEmpty());
		CHECK(narrow.Title.Max.x - narrow.Title.Min.x < title);
		CHECK(narrow.Title.Min.x > narrow.Menus.Max.x);
		CHECK(narrow.Title.Max.x < narrow.Status.Min.x);
		CHECK(narrow.Status.Max.x < narrow.Minimize.Min.x);
		CHECK(narrow.Close.Max.x == shortened);

		// No room for a title worth reading: the status stays.
		float const untitled = std::round(menus + status + buttons + spacing * 4.0f);
		fixture.Gui.SetDisplaySize(ImVec2(untitled, 600.0f));
		fixture.Frames();
		narrow = fixture.Bar.GetLayout();
		CHECK(narrow.Title.IsEmpty());
		CHECK_FALSE(narrow.Status.IsEmpty());
		CHECK(narrow.Status.Min.x > narrow.Menus.Max.x);
		CHECK(narrow.Status.Max.x < narrow.Minimize.Min.x);

		fixture.Gui.SetDisplaySize(ImVec2(menus + buttons + 20.0f, 600.0f));
		fixture.Frames();
		narrow = fixture.Bar.GetLayout();
		CHECK(narrow.Status.IsEmpty());
		CHECK(narrow.Title.IsEmpty());
		CHECK(narrow.Minimize.Min.x >= narrow.Menus.Max.x);
		CHECK(narrow.Close.Max.x == menus + buttons + 20.0f);
		// What is left between the menus and the buttons still drags the window.
		CHECK(fixture.FindArea(narrow.Menus.Max.x + 10.0f, fixture.TitleBarHeight * 0.5f) == TitleBarArea::Caption);
	}
}

TEST_CASE("EditorTitleBar: it tells the window which of its parts are the caption and the buttons")
{
	TitleBarFixture fixture;
	fixture.Frames();
	EditorTitleBar::Layout const& layout = fixture.Bar.GetLayout();
	float const middle = fixture.TitleBarHeight * 0.5f;

	// The menus are the editor's; the rest of the bar drags the window, its title and status included.
	CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Menus).x, middle) == TitleBarArea::Client);
	CHECK(fixture.FindArea(layout.Menus.Min.x + 2.0f, 0.0f) == TitleBarArea::Client);
	CHECK(fixture.FindArea(layout.Menus.Max.x + 12.0f, middle) == TitleBarArea::Caption);
	CHECK(fixture.FindArea(1.0f, middle) == TitleBarArea::Caption);
	CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Title).x, middle) == TitleBarArea::Caption);
	CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Status).x, middle) == TitleBarArea::Caption);
	CHECK(fixture.FindArea(layout.Minimize.Min.x - 1.0f, 0.0f) == TitleBarArea::Caption);
	// The buttons, from edge to edge and from the top.
	CHECK(fixture.FindArea(layout.Minimize.Min.x, 0.0f) == TitleBarArea::Minimize);
	CHECK(fixture.FindArea(layout.Maximize.Min.x - 1.0f, fixture.TitleBarHeight - 1.0f) == TitleBarArea::Minimize);
	CHECK(fixture.FindArea(layout.Maximize.Min.x, middle) == TitleBarArea::Maximize);
	CHECK(fixture.FindArea(layout.Close.Min.x, middle) == TitleBarArea::Close);
	CHECK(fixture.FindArea(layout.Close.Max.x - 1.0f, 0.0f) == TitleBarArea::Close);
	// Under the bar, and outside the window.
	CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Title).x, fixture.TitleBarHeight) == TitleBarArea::Client);
	CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Close).x, fixture.TitleBarHeight + 1.0f) == TitleBarArea::Client);
	CHECK(fixture.FindArea(-5.0f, middle) == TitleBarArea::Client);
	CHECK(fixture.FindArea(layout.Close.Max.x, middle) == TitleBarArea::Client);
	CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Title).x, -1.0f) == TitleBarArea::Client);

	SUBCASE("another window over the bar has what it covers")
	{
		fixture.FloatingWindow = true;
		fixture.Frames();
		float const inside = fixture.FloatingMin.x + 40.0f;
		CHECK(fixture.FindArea(inside, fixture.FloatingMin.y + 6.0f) == TitleBarArea::Client);
		// The bar above and beside it is still the window's (ImGui gives a window that can be resized a few pixels around
		// it, to grab its borders by).
		CHECK(fixture.FindArea(inside, 2.0f) == TitleBarArea::Caption);
		CHECK(fixture.FindArea(fixture.FloatingMin.x - 20.0f, fixture.FloatingMin.y + 6.0f) == TitleBarArea::Caption);
		// Over a button too.
		fixture.FloatingMin.x = layout.Close.Min.x - 100.0f;
		fixture.Frames();
		CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Close).x, fixture.FloatingMin.y + 6.0f) == TitleBarArea::Client);
		CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Close).x, 2.0f) == TitleBarArea::Close);
	}

	SUBCASE("a bar that is not drawn any more is nobody's title bar")
	{
		ImVec2 const title = TitleBarFixture::Center(layout.Title);
		// Asked between frames, and while the next frame is being drawn.
		CHECK(fixture.FindArea(title.x, middle) == TitleBarArea::Caption);
		fixture.Gui.Frame(
			[&]
			{
				CHECK(fixture.FindArea(title.x, middle) == TitleBarArea::Caption);
			});
		// That frame ended without the bar.
		CHECK(fixture.FindArea(title.x, middle) == TitleBarArea::Client);
		CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Close).x, middle) == TitleBarArea::Client);
		// Drawn again, it is the title bar once ImGui finds the bar's window under a position: a frame after the window
		// came back.
		fixture.Frames(2);
		CHECK(fixture.FindArea(title.x, middle) == TitleBarArea::Caption);
	}

	SUBCASE("in another ImGui context it has no layout")
	{
		ImVec2 const title = TitleBarFixture::Center(layout.Title);
		ImGuiContext* const own = ImGui::GetCurrentContext();
		ImGuiContext* const other = ImGui::CreateContext();
		ImGui::SetCurrentContext(other);
		CHECK(fixture.FindArea(title.x, middle) == TitleBarArea::Client);
		ImGui::SetCurrentContext(nullptr);
		CHECK(fixture.FindArea(title.x, middle) == TitleBarArea::Client);
		ImGui::SetCurrentContext(own);
		ImGui::DestroyContext(other);
		CHECK(ImGui::GetCurrentContext() == own);
		CHECK(fixture.FindArea(title.x, middle) == TitleBarArea::Caption);
	}
}

TEST_CASE("EditorTitleBar: its menus open at its lower edge and hold items as high as the plain menu bar's")
{
	// What the plain bar's menu is like.
	float plainItemHeight = 0.0f;
	{
		TitleBarFixture plain;
		plain.BarWindow.reset();
		plain.Frames();
		ImVec2 const file(plain.Bar.GetLayout().Menus.Min.x + 12.0f, plain.FrameHeight * 0.5f);
		plain.Gui.Click(file,
		                [&plain]
		                {
							plain.Draw();
						});
		plain.Frames();
		REQUIRE(plain.FileMenuOpen);
		CHECK(plain.MenuTop == doctest::Approx(plain.FrameHeight));
		CHECK(plain.MenuFrameHeight == plain.FrameHeight);
		plainItemHeight = plain.MenuItemHeight;
		REQUIRE(plainItemHeight > 0.0f);
	}

	TitleBarFixture fixture;
	fixture.Frames();
	EditorTitleBar::Layout const& layout = fixture.Bar.GetLayout();
	float const middle = fixture.TitleBarHeight * 0.5f;
	ImVec2 const caption(layout.Menus.Max.x + 12.0f, middle);
	REQUIRE(fixture.FindArea(caption.x, caption.y) == TitleBarArea::Caption);
	CHECK_FALSE(fixture.FileMenuOpen);

	SUBCASE("clicked")
	{
		fixture.Gui.Click(ImVec2(layout.Menus.Min.x + 12.0f, middle),
		                  [&fixture]
		                  {
							  fixture.Draw();
						  });
	}
	SUBCASE("opened by its ID in the bar, as agents open menus")
	{
		fixture.OpenFileMenu = true;
	}
	fixture.Frames();

	REQUIRE(fixture.FileMenuOpen);
	// The popup hangs from the bar's lower edge, and its contents have the style's own padding.
	CHECK(fixture.MenuTop == fixture.TitleBarHeight);
	CHECK(fixture.MenuFrameHeight == fixture.FrameHeight);
	CHECK(fixture.MenuItemHeight == plainItemHeight);

	// While a menu is open the bar drags nothing: the click closes the menu, as on a title bar of the system's. The
	// buttons stay the window's.
	CHECK(fixture.FindArea(caption.x, caption.y) == TitleBarArea::Client);
	CHECK(fixture.FindArea(TitleBarFixture::Center(layout.Close).x, middle) == TitleBarArea::Close);
	fixture.Gui.Click(caption,
	                  [&fixture]
	                  {
						  fixture.Draw();
					  });
	fixture.Frames();
	CHECK_FALSE(fixture.FileMenuOpen);
	CHECK(fixture.FindArea(caption.x, caption.y) == TitleBarArea::Caption);
}
