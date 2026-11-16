#include "Strada/Core/CommandLine.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	CommandLineParser CreateParser()
	{
		CommandLineParser parser("Test program");
		parser.AddFlag("headless", "Run without a window")
			.AddOption("project", "path", "Project file")
			.AddOption("frames", "count", "Frames to run")
			.AddOption("scale", "factor", "Scale factor");
		return parser;
	}

	Result<CommandLineArguments> Parse(std::vector<std::string> const& arguments)
	{
		return CreateParser().Parse(arguments);
	}
}

TEST_CASE("CommandLine: flags, separated and inline values")
{
	Result<CommandLineArguments> result = Parse({"--headless", "--project", "Game/Game.sproj", "--frames=120"});
	REQUIRE(result.IsOk());
	CommandLineArguments const& arguments = result.GetValue();
	CHECK(arguments.HasFlag("headless"));
	CHECK(arguments.GetValue("project") == std::optional<std::string>("Game/Game.sproj"));
	CHECK(arguments.GetInt("frames") == std::optional<int64_t>(120));
	CHECK_FALSE(arguments.GetValue("scale").has_value());
}

TEST_CASE("CommandLine: positionals and the -- separator")
{
	Result<CommandLineArguments> result = Parse({"first", "--headless", "--", "--not-an-option", "-x", "--"});
	REQUIRE(result.IsOk());
	std::vector<std::string> const& positionals = result.GetValue().GetPositionals();
	REQUIRE(positionals.size() == 4);
	CHECK(positionals[0] == "first");
	CHECK(positionals[1] == "--not-an-option");
	CHECK(positionals[2] == "-x");
	CHECK(positionals[3] == "--");
}

TEST_CASE("CommandLine: errors for unknown options and missing values")
{
	Result<CommandLineArguments> const unknown = Parse({"--bogus"});
	REQUIRE(unknown.IsError());
	CHECK(unknown.GetError().find("--bogus") != std::string::npos);

	Result<CommandLineArguments> const missing = Parse({"--project"});
	REQUIRE(missing.IsError());
	CHECK(missing.GetError().find("<path>") != std::string::npos);

	CHECK(Parse({"--headless=1"}).IsError());
}

TEST_CASE("CommandLine: numeric parsing is strict and locale-independent")
{
	Result<CommandLineArguments> result = Parse({"--frames", "12x", "--scale", "1.5"});
	REQUIRE(result.IsOk());
	CHECK_FALSE(result.GetValue().GetInt("frames").has_value());
	REQUIRE(result.GetValue().GetDouble("scale").has_value());
	CHECK(*result.GetValue().GetDouble("scale") == doctest::Approx(1.5));

	Result<CommandLineArguments> negative = Parse({"--frames", "-4"});
	REQUIRE(negative.IsOk());
	CHECK(negative.GetValue().GetInt("frames") == std::optional<int64_t>(-4));

	Result<CommandLineArguments> badDouble = Parse({"--scale", "1.5abc"});
	REQUIRE(badDouble.IsOk());
	CHECK_FALSE(badDouble.GetValue().GetDouble("scale").has_value());
}

TEST_CASE("CommandLine: argc/argv parsing skips the program name")
{
	char const* argv[] = {"program", "--headless", "level1"};
	Result<CommandLineArguments> result = CreateParser().Parse(3, argv);
	REQUIRE(result.IsOk());
	CHECK(result.GetValue().HasFlag("headless"));
	REQUIRE(result.GetValue().GetPositionals().size() == 1);
	CHECK(result.GetValue().GetPositionals()[0] == "level1");
}

TEST_CASE("CommandLine: help text lists every option")
{
	std::string const help = CreateParser().GetHelpText("strada");
	CHECK(help.find("Usage: strada") != std::string::npos);
	CHECK(help.find("--headless") != std::string::npos);
	CHECK(help.find("--project <path>") != std::string::npos);
	CHECK(help.find("Frames to run") != std::string::npos);
}

TEST_CASE("CommandLine: a window size reads a width and a height")
{
	std::optional<glm::uvec2> const qhd = ParseWindowSize("2560x1440");
	REQUIRE(qhd.has_value());
	CHECK(*qhd == glm::uvec2(2560, 1440));
	std::optional<glm::uvec2> const fullHd = ParseWindowSize("1920X1080");
	REQUIRE(fullHd.has_value());
	CHECK(*fullHd == glm::uvec2(1920, 1080));
	std::optional<glm::uvec2> const largest = ParseWindowSize("16384x1");
	REQUIRE(largest.has_value());
	CHECK(*largest == glm::uvec2(16384, 1));
}

TEST_CASE("CommandLine: a window size refuses zero, negative and oversized sizes, missing parts and other text")
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
		CHECK_FALSE(ParseWindowSize(text).has_value());
	}
}
