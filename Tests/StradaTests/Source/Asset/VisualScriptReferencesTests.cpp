#include "Strada/Asset/VisualScriptReferences.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptNode MakeNode(Json settings)
	{
		VisualScriptNode node;
		node.ID = 7;
		node.Type = "Function.Call";
		node.Settings = std::move(settings);
		return node;
	}

	AssetHandle MakeHandle(uint64_t value)
	{
		return AssetHandle(UUID(value));
	}
}

TEST_CASE("VisualScript: a node's Graph setting is a string holding a decimal handle other than 0")
{
	struct Case
	{
		Json Graph;
		std::optional<AssetHandle> Expected;
	};
	std::vector<Case> const cases = {
		{"8146390251172604461", MakeHandle(8146390251172604461ull)},
		{"18446744073709551615", MakeHandle(18446744073709551615ull)},
		{"42", MakeHandle(42)},
		// Leading zeros are digits, as the runtime reads handles.
		{"007", MakeHandle(7)},
		// "0" is the literal of no graph.
		{"0", std::nullopt},
		{"00", std::nullopt},
		{"", std::nullopt},
		{"-5", std::nullopt},
		{"+5", std::nullopt},
		{" 5", std::nullopt},
		{"5 ", std::nullopt},
		{"0x10", std::nullopt},
		{"18446744073709551616", std::nullopt},
		{"asset://Scripts/Motion.sgraph", std::nullopt},
		{"Motion", std::nullopt},
		// Handles are strings: numbers are not, whatever their value.
		{42, std::nullopt},
		{8146390251172604461ull, std::nullopt},
		{1.5, std::nullopt},
		{nullptr, std::nullopt},
		{true, std::nullopt},
		{Json::array({"42"}), std::nullopt},
		{Json::object({{"Graph", "42"}}), std::nullopt},
	};
	for (Case const& tested : cases)
	{
		CAPTURE(tested.Graph.dump());
		VisualScriptNode const node = MakeNode(Json::object({{"Graph", tested.Graph}, {"Function", 3}}));
		CHECK(ReadVisualScriptGraphSetting(node) == tested.Expected);
		// A Graph member makes the node's other settings name another graph's elements, malformed or not.
		CHECK_FALSE(IsLocalVisualScriptReference(node));
	}
}

TEST_CASE("VisualScript: a node without a Graph setting refers to its own document")
{
	VisualScriptNode const call = MakeNode(Json::object({{"Function", 3}}));
	CHECK_FALSE(ReadVisualScriptGraphSetting(call).has_value());
	CHECK(IsLocalVisualScriptReference(call));

	VisualScriptNode const empty = MakeNode(Json::object());
	CHECK_FALSE(ReadVisualScriptGraphSetting(empty).has_value());
	CHECK(IsLocalVisualScriptReference(empty));

	// Settings of a node built in memory that are no object hold no Graph member.
	for (Json const& settings : {Json(), Json::array({"Graph", "42"}), Json("Graph")})
	{
		CAPTURE(settings.dump());
		VisualScriptNode const node = MakeNode(settings);
		CHECK_FALSE(ReadVisualScriptGraphSetting(node).has_value());
		CHECK(IsLocalVisualScriptReference(node));
	}

	// Names are case-sensitive, as every key of the format.
	VisualScriptNode const lowercase = MakeNode(Json::object({{"graph", "42"}}));
	CHECK_FALSE(ReadVisualScriptGraphSetting(lowercase).has_value());
	CHECK(IsLocalVisualScriptReference(lowercase));
}

TEST_CASE("VisualScript: a resolver answers the documents of other graphs, and none for the graphs it does not know")
{
	VisualScriptDocument library;
	library.Kind = VisualScriptDocumentKind::Library;
	AssetHandle const known = MakeHandle(5550000000000000001ull);
	VisualScriptGraphResolver const resolve = [&library, known](AssetHandle graph) -> VisualScriptDocument const*
	{
		return graph == known ? &library : nullptr;
	};
	VisualScriptNode const call = MakeNode(Json::object({{"Graph", "5550000000000000001"}, {"Function", 3}}));
	std::optional<AssetHandle> const graph = ReadVisualScriptGraphSetting(call);
	REQUIRE(graph.has_value());
	CHECK(resolve(*graph) == &library);
	CHECK(resolve(MakeHandle(42)) == nullptr);
}
