#include "HeadlessImGui.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cfloat>
#include <string_view>
#include <vector>

using Strada::Testing::HeadlessImGui;

namespace
{
	// A window over the top-left of the display that neither moves nor shows a title bar.
	void BeginFixedWindow(char const* name, ImVec2 size)
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(size);
		ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	}

	constexpr ImGuiButtonFlags EveryMouseButton =
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle;
}

TEST_CASE("HeadlessImGui: the clipboard is the context's own, and text fields copy and paste through it")
{
	{
		HeadlessImGui imgui;
		CHECK(ImGui::GetPlatformIO().Platform_ClipboardUserData == &imgui);
		CHECK(std::string_view(ImGui::GetClipboardText()).empty());
		ImGui::SetClipboardText("Copied");
		CHECK(std::string_view(ImGui::GetClipboardText()) == "Copied");

		// Select all, copy, then paste over it, with the shortcut key of the platform.
		std::array<char, 64> text = {"Hello"};
		bool focus = true;
		auto const draw = [&]
		{
			BeginFixedWindow("Clipboard", ImVec2(400.0f, 200.0f));
			if (focus)
			{
				ImGui::SetKeyboardFocusHere();
				focus = false;
			}
			ImGui::InputText("##text", text.data(), text.size());
			ImGui::End();
		};
		for (int frame = 0; frame < 3; frame++)
		{
			imgui.Frame(draw);
		}
		imgui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, draw);
		imgui.Chord(ImGuiMod_Ctrl | ImGuiKey_C, draw);
		CHECK(std::string_view(ImGui::GetClipboardText()) == "Hello");
		ImGui::SetClipboardText("World");
		imgui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, draw);
		imgui.Chord(ImGuiMod_Ctrl | ImGuiKey_V, draw);
		CHECK(std::string_view(text.data()) == "World");
	}
	// Nothing carries over to the next context.
	HeadlessImGui imgui;
	CHECK(std::string_view(ImGui::GetClipboardText()).empty());
}

TEST_CASE("HeadlessImGui: chords reach shortcuts as the chord given, with and without macOS behaviors")
{
	HeadlessImGui imgui;
	for (bool const macOS : {false, true})
	{
		CAPTURE(macOS);
		ImGui::GetIO().ConfigMacOSXBehaviors = macOS;
		int saves = 0;
		int redos = 0;
		int supers = 0;
		bool controlHeld = false;
		auto const draw = [&]
		{
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
			{
				saves++;
				controlHeld = ImGui::GetIO().KeyCtrl;
			}
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
			{
				redos++;
			}
			if (ImGui::Shortcut(ImGuiMod_Super | ImGuiKey_K, ImGuiInputFlags_RouteGlobal))
			{
				supers++;
			}
		};
		imgui.Frame(draw);
		imgui.Chord(ImGuiMod_Ctrl | ImGuiKey_S, draw);
		imgui.Chord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, draw);
		imgui.Chord(ImGuiMod_Super | ImGuiKey_K, draw);
		CHECK(saves == 1);
		CHECK(controlHeld);
		CHECK(redos == 1);
		CHECK(supers == 1);
		// Every key is up again.
		ImGuiIO const& io = ImGui::GetIO();
		CHECK(!io.KeyCtrl);
		CHECK(!io.KeyShift);
		CHECK(!io.KeyAlt);
		CHECK(!io.KeySuper);
	}
}

TEST_CASE("HeadlessImGui: DragPath drags along a straight line with any mouse button, and short paths only click")
{
	HeadlessImGui imgui;
	ImGuiMouseButton button = ImGuiMouseButton_Left;
	std::vector<ImVec2> positions;
	ImVec2 delta(0.0f, 0.0f);
	int clicks = 0;
	auto const draw = [&]
	{
		BeginFixedWindow("Surface", ImVec2(800.0f, 600.0f));
		if (ImGui::InvisibleButton("##surface", ImGui::GetContentRegionAvail(), EveryMouseButton))
		{
			clicks++;
		}
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(button))
		{
			positions.push_back(ImGui::GetIO().MousePos);
			delta = ImGui::GetMouseDragDelta(button);
		}
		ImGui::End();
	};
	imgui.Frame(draw);

	ImVec2 const from(100.0f, 100.0f);
	ImVec2 const to(300.0f, 200.0f);
	for (ImGuiMouseButton const dragged : {ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle})
	{
		CAPTURE(dragged);
		button = dragged;
		positions.clear();
		clicks = 0;
		imgui.DragPath(from, to, draw, dragged);
		// Four steps along the line, then a frame resting on the target.
		REQUIRE(positions.size() == 5);
		for (size_t step = 0; step < 4; step++)
		{
			float const amount = static_cast<float>(step + 1) / 4.0f;
			CHECK(positions[step].x == doctest::Approx(from.x + (to.x - from.x) * amount));
			CHECK(positions[step].y == doctest::Approx(from.y + (to.y - from.y) * amount));
		}
		CHECK(positions[4].x == to.x);
		CHECK(positions[4].y == to.y);
		CHECK(delta.x == to.x - from.x);
		CHECK(delta.y == to.y - from.y);
		// Released over the surface.
		CHECK(clicks == 1);
	}

	// One step goes straight to the target.
	button = ImGuiMouseButton_Left;
	positions.clear();
	imgui.DragPath(from, to, draw, ImGuiMouseButton_Left, 1);
	REQUIRE(positions.size() == 2);
	CHECK(positions[0].x == to.x);
	CHECK(positions[0].y == to.y);

	// Within the drag threshold the press and release are a click.
	positions.clear();
	clicks = 0;
	imgui.DragPath(from, ImVec2(from.x + 3.0f, from.y), draw);
	CHECK(positions.empty());
	CHECK(clicks == 1);
}

TEST_CASE("HeadlessImGui: Wheel turns the mouse wheel over a point, scrolling the window under it")
{
	HeadlessImGui imgui;
	float wheel = 0.0f;
	float scroll = 0.0f;
	auto const draw = [&]
	{
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
		ImGui::Begin("Rows", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		for (int row = 0; row < 100; row++)
		{
			ImGui::Text("Row %d", row);
		}
		if (ImGui::IsWindowHovered())
		{
			wheel += ImGui::GetIO().MouseWheel;
		}
		scroll = ImGui::GetScrollY();
		ImGui::End();
	};
	imgui.Frame(draw);
	imgui.Frame(draw);
	CHECK(scroll == 0.0f);
	imgui.Wheel(ImVec2(200.0f, 150.0f), ImVec2(0.0f, -2.0f), draw);
	imgui.Frame(draw);
	CHECK(wheel == -2.0f);
	CHECK(scroll > 0.0f);

	// Outside the window the wheel scrolls nothing.
	float const scrolled = scroll;
	imgui.Wheel(ImVec2(1000.0f, 800.0f), ImVec2(0.0f, -2.0f), draw);
	imgui.Frame(draw);
	CHECK(wheel == -2.0f);
	CHECK(scroll == scrolled);
}

TEST_CASE("HeadlessImGui: MouseClick clicks any mouse button, and two clicks are a double click")
{
	HeadlessImGui imgui;
	std::array<int, 3> clicks = {};
	std::array<int, 3> doubleClicks = {};
	bool menu = false;
	auto const draw = [&]
	{
		BeginFixedWindow("Target", ImVec2(400.0f, 300.0f));
		ImGui::InvisibleButton("##target", ImVec2(200.0f, 100.0f), EveryMouseButton);
		for (ImGuiMouseButton button = 0; button < 3; button++)
		{
			if (ImGui::IsItemClicked(button))
			{
				clicks[static_cast<size_t>(button)]++;
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(button))
			{
				doubleClicks[static_cast<size_t>(button)]++;
			}
		}
		if (ImGui::BeginPopupContextItem("##menu"))
		{
			menu = true;
			ImGui::EndPopup();
		}
		ImGui::End();
	};
	imgui.Frame(draw);

	imgui.MouseClick(ImVec2(50.0f, 50.0f), ImGuiMouseButton_Middle, draw);
	CHECK(clicks == std::array<int, 3>{0, 0, 1});
	imgui.MouseClick(ImVec2(50.0f, 50.0f), ImGuiMouseButton_Left, draw, 2);
	CHECK(clicks == std::array<int, 3>{2, 0, 1});
	CHECK(doubleClicks == std::array<int, 3>{1, 0, 0});
	CHECK(!menu);

	// A right click opens the context menu.
	imgui.MouseClick(ImVec2(50.0f, 50.0f), ImGuiMouseButton_Right, draw);
	imgui.Frame(draw);
	CHECK(clicks == std::array<int, 3>{2, 1, 1});
	CHECK(menu);
	CHECK(GImGui->OpenPopupStack.Size == 1);
}

TEST_CASE("HeadlessImGui: UseEditorStyle sets the context up as the editor does: keyboard navigation, the theme and the fonts")
{
	SUBCASE("before the first frame")
	{
		HeadlessImGui imgui;
		ImFont* const monospace = imgui.UseEditorStyle();
		REQUIRE(monospace != nullptr);
		ImGuiIO const& io = ImGui::GetIO();
		CHECK((io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
		CHECK((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) == 0);
		// Roboto, the default font, and Cousine, the fixed-width one: the editor's two fonts and no other.
		REQUIRE(io.Fonts->Fonts.Size == 2);
		ImFont* const roboto = io.FontDefault;
		CHECK(roboto == io.Fonts->Fonts[0]);
		CHECK(monospace == io.Fonts->Fonts[1]);
		// The theme.
		ImGuiStyle const& style = ImGui::GetStyle();
		CHECK(style.FontSizeBase == 15.0f);
		CHECK(style.FramePadding.x == 8.0f);
		CHECK(ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_WindowBg]) == IM_COL32(0x26, 0x2A, 0x31, 0xFF));
		imgui.Frame(
			[&]
			{
				CHECK(ImGui::GetFont() == roboto);
				CHECK(ImGui::GetFontSize() == 15.0f);
				// Roboto (it has a minus sign, which ImGui's own font lacks) is proportional; Cousine is fixed-width.
				CHECK(roboto->IsGlyphInFont(0x2212));
				CHECK(roboto->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, "iiii").x < roboto->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, "MMMM").x);
				CHECK(monospace->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, "iiii").x == monospace->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, "MMMM").x);
			});
	}
	SUBCASE("with docking")
	{
		HeadlessImGui imgui;
		REQUIRE(imgui.UseEditorStyle(true) != nullptr);
		CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0);
		imgui.Frame(
			[]
			{
				ImGui::DockSpaceOverViewport(ImGui::GetID("Dockspace"), ImGui::GetMainViewport());
			});
	}
	SUBCASE("after frames drawn with ImGui's own font")
	{
		HeadlessImGui imgui;
		imgui.Frame([] {});
		int const imguiFonts = ImGui::GetIO().Fonts->Fonts.Size;
		REQUIRE(imguiFonts > 0);
		ImFont* const monospace = imgui.UseEditorStyle();
		REQUIRE(monospace != nullptr);
		ImGuiIO const& io = ImGui::GetIO();
		REQUIRE(io.Fonts->Fonts.Size == imguiFonts + 2);
		// Roboto is the default font all the same.
		CHECK(io.FontDefault == io.Fonts->Fonts[imguiFonts]);
		CHECK(monospace == io.Fonts->Fonts[imguiFonts + 1]);
		imgui.Frame(
			[&]
			{
				CHECK(ImGui::GetFont() == io.FontDefault);
				CHECK(ImGui::GetFontSize() == 15.0f);
			});
	}
}
