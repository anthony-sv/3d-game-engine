#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/JsonSchema.h"

#include <doctest/doctest.h>

#include <optional>
#include <stdexcept>

using namespace Strada;

namespace
{
	CommandDefinition MakeEcho()
	{
		CommandDefinition definition;
		definition.Name = "test.echo";
		definition.Description = "Returns its text parameter.";
		definition.Parameters = SchemaBuilder::Object().Property("text", SchemaBuilder::String("Text"), true).Build();
		definition.ReadOnly = true;
		definition.Handler = [](Json const& params) -> CommandResult
		{
			return Json::object({{"text", params["text"]}});
		};
		return definition;
	}

	std::optional<CommandResult> Run(CommandRegistry const& registry, std::string_view name, Json const& params)
	{
		std::optional<CommandResult> result;
		registry.Execute(name, params,
		                 [&result](CommandResult value)
		                 {
							 result.emplace(std::move(value));
						 });
		return result;
	}
}

TEST_CASE("CommandRegistry: registration validates definitions")
{
	CommandRegistry registry;
	REQUIRE(registry.Register(MakeEcho()).IsOk());
	CHECK(registry.Register(MakeEcho()).IsError());
	CHECK(registry.Contains("test.echo"));
	CHECK(registry.GetCommandCount() == 1);

	CommandDefinition badName = MakeEcho();
	badName.Name = "Echo";
	CHECK(registry.Register(badName).IsError());
	CommandDefinition noDescription = MakeEcho();
	noDescription.Name = "test.other";
	noDescription.Description.clear();
	CHECK(registry.Register(noDescription).IsError());
	CommandDefinition bothHandlers = MakeEcho();
	bothHandlers.Name = "test.both";
	bothHandlers.AsyncHandler = [](Json const&, CommandCompletion) {};
	CHECK(registry.Register(bothHandlers).IsError());

	CHECK(CommandRegistry::IsValidCommandName("play.advance-frames"));
	CHECK_FALSE(CommandRegistry::IsValidCommandName("noDomain"));

	Json const description = registry.Describe();
	REQUIRE(description.size() == 1);
	CHECK(description[0]["name"] == "test.echo");
	CHECK(description[0]["readOnly"] == true);
}

TEST_CASE("CommandRegistry: parameters are validated before handlers run")
{
	CommandRegistry registry;
	REQUIRE(registry.Register(MakeEcho()).IsOk());

	std::optional<CommandResult> ok = Run(registry, "test.echo", Json::object({{"text", "hi"}}));
	REQUIRE(ok);
	REQUIRE(ok->IsOk());
	CHECK(ok->GetValue()["text"] == "hi");

	std::optional<CommandResult> missing = Run(registry, "test.echo", Json());
	REQUIRE(missing);
	REQUIRE(missing->IsError());
	CHECK(missing->GetError().Code == AutomationErrorCode::InvalidParams);
	CHECK(missing->GetError().Data["path"] == "text");

	CHECK(Run(registry, "test.echo", Json::array())->GetError().Code == AutomationErrorCode::InvalidParams);
	CHECK(Run(registry, "test.nope", Json())->GetError().Code == AutomationErrorCode::MethodNotFound);
}

TEST_CASE("CommandRegistry: escaping exceptions become internal errors")
{
	CommandRegistry registry;
	CommandDefinition throwing = MakeEcho();
	throwing.Handler = [](Json const&) -> CommandResult
	{
		throw std::runtime_error("boom");
	};
	REQUIRE(registry.Register(throwing).IsOk());
	std::optional<CommandResult> result = Run(registry, "test.echo", Json::object({{"text", "x"}}));
	REQUIRE(result);
	CHECK(result->GetError().Code == AutomationErrorCode::InternalError);
}

TEST_CASE("CommandRegistry: asynchronous commands complete later, once, or are cancelled")
{
	CommandRegistry registry;
	std::optional<CommandCompletion> pending;
	CommandDefinition later;
	later.Name = "test.later";
	later.Description = "Completes when the test says so.";
	later.AsyncHandler = [&pending](Json const&, CommandCompletion completion)
	{
		pending.emplace(std::move(completion));
	};
	REQUIRE(registry.Register(later).IsOk());

	std::optional<CommandResult> result;
	registry.Execute("test.later", Json(),
	                 [&result](CommandResult value)
	                 {
						 result.emplace(std::move(value));
					 });
	CHECK_FALSE(result);
	REQUIRE(pending);
	CHECK(pending->Complete(Json(1)));
	CHECK_FALSE(pending->Complete(Json(2)));
	REQUIRE(result);
	CHECK(result->GetValue() == Json(1));

	// Dropping the last completion without completing cancels the command.
	pending.reset();
	result.reset();
	registry.Execute("test.later", Json(),
	                 [&result](CommandResult value)
	                 {
						 result.emplace(std::move(value));
					 });
	CHECK_FALSE(result);
	pending.reset();
	REQUIRE(result);
	CHECK(result->GetError().Code == AutomationErrorCode::Cancelled);
}
