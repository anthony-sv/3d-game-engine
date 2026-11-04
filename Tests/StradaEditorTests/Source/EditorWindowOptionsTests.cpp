#include "Editor/EditorWindowOptions.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string_view>

using namespace Strada;

TEST_CASE("EditorWindowOptions: --window-size reads a width and a height")
{
	std::optional<glm::uvec2> const qhd = ParseEditorWindowSize("2560x1440");
	REQUIRE(qhd.has_value());
	CHECK(*qhd == glm::uvec2(2560, 1440));
	std::optional<glm::uvec2> const fullHd = ParseEditorWindowSize("1920X1080");
	REQUIRE(fullHd.has_value());
	CHECK(*fullHd == glm::uvec2(1920, 1080));
	std::optional<glm::uvec2> const largest = ParseEditorWindowSize("16384x1");
	REQUIRE(largest.has_value());
	CHECK(*largest == glm::uvec2(16384, 1));
}

TEST_CASE("EditorWindowOptions: --window-size refuses zero, negative and oversized sizes, missing parts and other text")
{
	for (std::string_view const text : {"",
	                                    "x",
	                                    "1920",
	                                    "1920x",
	                                    "x1080",
	                                    "0x1080",
	                                    "1920x0",
	                                    "-1920x1080",
	                                    "1920x-1080",
	                                    "+1920x1080",
	                                    "16385x1080",
	                                    "1920x16385",
	                                    "99999999999999999999x1",
	                                    "1920x1080 ",
	                                    " 1920x1080",
	                                    "1920x1080x720",
	                                    "1920 x 1080",
	                                    "1920x1080px",
	                                    "1920*1080",
	                                    "0x0",
	                                    "1e3x1080",
	                                    "1920.5x1080",
	                                    "abcxdef"})
	{
		CAPTURE(text);
		CHECK_FALSE(ParseEditorWindowSize(text).has_value());
	}
}
