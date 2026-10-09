#include "Editor/Automation/JsonRpc.h"

#include <doctest/doctest.h>

#include <string>

using namespace Strada;

TEST_CASE("JsonRpc: requests and notifications parse")
{
	CommandValue<JsonRpc::Message> request = JsonRpc::Parse(R"({"jsonrpc":"2.0","id":7,"method":"entity.get","params":{"entity":"1"}})");
	REQUIRE(request.IsOk());
	REQUIRE(request.GetValue().Requests.size() == 1);
	JsonRpc::Request const& parsed = request.GetValue().Requests[0];
	CHECK_FALSE(request.GetValue().IsBatch);
	CHECK(parsed.Id == Json(7));
	CHECK(parsed.Method == "entity.get");
	CHECK(parsed.Params["entity"] == "1");
	CHECK_FALSE(parsed.Error.has_value());

	CommandValue<JsonRpc::Message> notification = JsonRpc::Parse(R"({"jsonrpc":"2.0","method":"editor.status"})");
	REQUIRE(notification.IsOk());
	CHECK(notification.GetValue().Requests[0].IsNotification());
}

TEST_CASE("JsonRpc: malformed messages produce protocol errors")
{
	CommandValue<JsonRpc::Message> notJson = JsonRpc::Parse("{nope");
	REQUIRE(notJson.IsError());
	CHECK(notJson.GetError().Code == AutomationErrorCode::ParseError);

	CHECK(JsonRpc::Parse("[]").GetError().Code == AutomationErrorCode::InvalidRequest);
	// A value that is not a request object is a request with an error (answered with a null ID), per JSON-RPC 2.0.
	CommandValue<JsonRpc::Message> scalar = JsonRpc::Parse("42");
	REQUIRE(scalar.IsOk());
	REQUIRE(scalar.GetValue().Requests[0].Error.has_value());
	CHECK(scalar.GetValue().Requests[0].Error->Code == AutomationErrorCode::InvalidRequest);

	std::string deep(JsonRpc::MaxNestingDepth + 1, '[');
	deep += std::string(JsonRpc::MaxNestingDepth + 1, ']');
	CHECK(JsonRpc::Parse(deep).IsError());

	// Invalid request objects are reported per request.
	CommandValue<JsonRpc::Message> wrongVersion = JsonRpc::Parse(R"({"jsonrpc":"1.0","id":1,"method":"x"})");
	REQUIRE(wrongVersion.IsOk());
	REQUIRE(wrongVersion.GetValue().Requests[0].Error.has_value());
	CHECK(wrongVersion.GetValue().Requests[0].Error->Code == AutomationErrorCode::InvalidRequest);
}

TEST_CASE("JsonRpc: batches keep request order")
{
	CommandValue<JsonRpc::Message> batch =
		JsonRpc::Parse(R"([{"jsonrpc":"2.0","id":1,"method":"a"},{"jsonrpc":"2.0","id":2,"method":"b"},{"bad":true}])");
	REQUIRE(batch.IsOk());
	CHECK(batch.GetValue().IsBatch);
	REQUIRE(batch.GetValue().Requests.size() == 3);
	CHECK(batch.GetValue().Requests[1].Method == "b");
	CHECK(batch.GetValue().Requests[2].Error.has_value());
}

TEST_CASE("JsonRpc: responses are single compact lines")
{
	JsonRpc::Request request;
	request.Id = Json("abc");
	request.Method = "x";
	Json const ok = JsonRpc::MakeResponse(request, CommandResult(Json::object({{"value", 1}})));
	CHECK(ok["jsonrpc"] == "2.0");
	CHECK(ok["id"] == "abc");
	CHECK(ok["result"]["value"] == 1);

	Json const failure = JsonRpc::MakeResponse(request, CommandResult(MakeCommandError(AutomationErrorCode::EntityNotFound, "gone")));
	CHECK(failure["error"]["code"] == static_cast<int32_t>(AutomationErrorCode::EntityNotFound));
	CHECK(failure["error"]["message"] == "gone");

	std::string const line = JsonRpc::Serialize(Json::object({{"text", "a\nb"}}));
	CHECK(line.back() == '\n');
	CHECK(line.find('\n') == line.size() - 1);
}
