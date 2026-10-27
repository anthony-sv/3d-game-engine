#include "HeadlessImGui.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"

#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// The code points of a UTF-8 text.
	std::vector<ImWchar> GetCodePoints(std::string_view text)
	{
		std::vector<ImWchar> codePoints;
		char const* cursor = text.data();
		char const* const end = text.data() + text.size();
		while (cursor < end)
		{
			unsigned int codePoint = 0;
			int const length = ImTextCharFromUtf8(&codePoint, cursor, end);
			if (length <= 0)
			{
				break;
			}
			codePoints.push_back(static_cast<ImWchar>(codePoint));
			cursor += length;
		}
		return codePoints;
	}
}

TEST_CASE("VisualScriptEditor: pushing a ladder size before the global font scales gives exactly that size")
{
	Testing::HeadlessImGui imgui;
	imgui.UseEditorStyle();
	for (float const scale : {1.0f, 1.25f, 1.5f, 2.0f})
	{
		CAPTURE(scale);
		ImGui::GetStyle().FontScaleDpi = scale;
		imgui.Frame(
			[]
			{
				ImGui::Begin("Text");
				ImGuiStyle const& style = ImGui::GetStyle();
				for (float const pixels : VisualScriptTextLadder)
				{
					CAPTURE(pixels);
					ImGui::PushFont(nullptr, pixels / (style.FontScaleMain * style.FontScaleDpi));
					CHECK(ImGui::GetFontSize() == pixels);
					ImGui::PopFont();
				}
				ImGui::End();
			});
	}
	ImGui::GetStyle().FontScaleDpi = 1.0f;
}

TEST_CASE("VisualScriptEditor: zooming the canvas from 10 % to 200 % and back draws text only at ladder sizes")
{
	// 200 nodes of the five kinds the performance graphs use, with comments, at a UI scale of 2 with the editor's fonts.
	Testing::CanvasRenderFixture fixture(Testing::MakeGridGraph(20, 10, 2, 10), glm::vec2(1600.0f, 900.0f), true);
	fixture.SetUIScale(2.0f);
	glm::vec2 const center(20.0f * 320.0f * 0.5f, 10.0f * 180.0f * 0.5f);
	std::vector<float> zooms;
	for (float zoom = 0.10f; zoom < 2.0f; zoom *= 1.01f)
	{
		zooms.push_back(zoom);
	}
	zooms.push_back(2.0f);
	for (size_t i = zooms.size(); i-- > 0;)
	{
		zooms.push_back(zooms[i]);
	}

	std::vector<float> requested;
	double slowest = 0.0;
	for (float const zoom : zooms)
	{
		auto const start = std::chrono::steady_clock::now();
		fixture.SetView(center, zoom);
		slowest = std::max(slowest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		CHECK(fixture.Renderer.GetStats().TextSizes <= 16);
		for (float const pixels : fixture.Renderer.GetTextSizes())
		{
			if (std::find(requested.begin(), requested.end(), pixels) == requested.end())
			{
				requested.push_back(pixels);
			}
		}
	}
	std::sort(requested.begin(), requested.end());
	ImTextureData const* texture = ImGui::GetIO().Fonts->TexData;
	MESSAGE("zoom sweep: ", zooms.size(), " frames, ", requested.size(), " text sizes, atlas ", texture->Width, " x ", texture->Height,
	        ", slowest frame ", slowest, " ms");
	CHECK(!requested.empty());
	for (float const pixels : requested)
	{
		CAPTURE(pixels);
		// The pills keep the window's font size (FontSize × UI scale); everything else is on the ladder.
		bool const ladder = std::find(VisualScriptTextLadder.begin(), VisualScriptTextLadder.end(), pixels) != VisualScriptTextLadder.end();
		CHECK(ladder);
	}
	CHECK(texture->Width <= 4096);
	CHECK(texture->Height <= 4096);
	ImGui::GetStyle().FontScaleDpi = 1.0f;
}

TEST_CASE("VisualScriptEditor: the editor's UI font has every character the canvas draws")
{
	Testing::HeadlessImGui imgui;
	imgui.UseEditorStyle();
	Testing::FixtureVisualScriptSemantics semantics;
	VisualScriptLibrary const& library = semantics.GetFixtureLibrary();

	std::vector<std::string> texts(VisualScriptCanvasTexts::All.begin(), VisualScriptCanvasTexts::All.end());
	texts.emplace_back(VisualScriptEllipsis);
	// Operator symbols, badge counts and the chip texts of booleans.
	for (std::string_view const type : {"Operator.Add", "Operator.Subtract", "Operator.Multiply", "Operator.Divide", "Operator.Modulo",
	                                    "Operator.Equal", "Operator.NotEqual", "Operator.Less", "Operator.LessOrEqual", "Operator.Greater",
	                                    "Operator.GreaterOrEqual", "Operator.And", "Operator.Or", "Operator.Xor"})
	{
		CAPTURE(type);
		CHECK(!GetVisualScriptOperatorSymbol(type).empty());
		texts.emplace_back(GetVisualScriptOperatorSymbol(type));
	}
	texts.emplace_back("0123456789+-.e | true false ()[]{}\"':,");
	// The library's titles, as nodes, pins and pin tooltips show them.
	for (VisualScriptNodeTemplate const& node : library.GetNodes())
	{
		texts.push_back(node.Title);
		texts.push_back(node.Subtitle);
		for (VisualScriptPin const& pin : node.Pins)
		{
			texts.push_back(pin.Label.empty() ? pin.Name : pin.Label);
		}
	}
	for (VisualScriptTypeInfo const& type : library.GetTypes())
	{
		texts.push_back(type.Title);
		texts.push_back(type.ID);
	}

	ImFont* const font = ImGui::GetIO().Fonts->Fonts[0];
	REQUIRE(font != nullptr);
	for (std::string const& text : texts)
	{
		for (ImWchar const codePoint : GetCodePoints(text))
		{
			// The two arrows are drawn as shapes (UI::DrawArrowText); ImGui never receives them.
			if (codePoint == 0x2192 || codePoint == 0x2190 || codePoint == '\n')
			{
				continue;
			}
			CAPTURE(text);
			CAPTURE(static_cast<unsigned>(codePoint));
			CHECK(font->IsGlyphInFont(codePoint));
		}
	}
}
