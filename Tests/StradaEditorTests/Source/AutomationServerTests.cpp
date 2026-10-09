#include "TcpTestClient.h"

#include "Editor/Automation/AutomationInstance.h"
#include "Editor/Automation/AutomationServer.h"
#include "Editor/Automation/EditorCommands.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#include <set>

using namespace Strada;
using Strada::Testing::TcpTestClient;

namespace
{
	struct ServerFixture
	{
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;
		AutomationServer Server{Registry};

		explicit ServerFixture(AutomationServerSpecification specification = {})
		{
			REQUIRE(RegisterEditorCommands(Registry, Operations, EditorCommandEnvironment{}).IsOk());
			REQUIRE(Server.Start(specification).IsOk());
		}

		Scope<TcpTestClient> Connect(bool authenticate = true)
		{
			auto client = CreateScope<TcpTestClient>(
				[this]
				{
					Server.ProcessRequests();
				});
			REQUIRE(client->Connect(Server.GetPort()));
			if (authenticate)
			{
				std::optional<Json> response = client->Call("authenticate", Json::object({{"token", Server.GetToken()}}));
				REQUIRE(response);
				REQUIRE((*response)["result"]["authenticated"] == true);
			}
			return client;
		}
	};

	int32_t ErrorCode(std::optional<Json> const& response)
	{
		REQUIRE(response);
		REQUIRE(response->contains("error"));
		return (*response)["error"]["code"].get<int32_t>();
	}

	constexpr int32_t Code(AutomationErrorCode code)
	{
		return static_cast<int32_t>(code);
	}
}

TEST_CASE("AutomationServer: tokens are random and compared in constant time")
{
	std::string const token = GenerateAutomationToken();
	CHECK(token.size() == 64);
	CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
	CHECK(GenerateAutomationToken() != token);

	CHECK(SecretsEqual("abc", "abc"));
	CHECK_FALSE(SecretsEqual("abc", "abd"));
	CHECK_FALSE(SecretsEqual("abc", "abcd"));
	CHECK_FALSE(SecretsEqual("", "a"));
}

TEST_CASE("AutomationServer: authenticated clients run commands over TCP")
{
	ServerFixture fixture;
	CHECK(fixture.Server.IsRunning());
	CHECK(fixture.Server.GetPort() != 0);
	Scope<TcpTestClient> client = fixture.Connect();

	std::optional<Json> created = client->Call("entity.create", Json::object({{"name", "Remote"}}));
	REQUIRE(created);
	REQUIRE(created->contains("result"));
	CHECK(fixture.Context.GetScene().FindEntityByName("Remote"));

	std::optional<Json> status = client->Call("editor.status");
	REQUIRE(status);
	CHECK((*status)["result"]["scene"]["entityCount"] == 1);
	CHECK(ErrorCode(client->Call("entity.get", Json::object({{"entity", "1"}}))) == Code(AutomationErrorCode::EntityNotFound));
	CHECK(ErrorCode(client->Call("no.such-command")) == Code(AutomationErrorCode::MethodNotFound));

	// Batches answer every request (notifications excepted) in one array.
	REQUIRE(client->Send(R"([{"jsonrpc":"2.0","id":"a","method":"editor.status"},)"
	                     R"({"jsonrpc":"2.0","method":"editor.status"},)"
	                     R"({"jsonrpc":"2.0","id":"b","method":"entity.find","params":{"name":"Remote"}}])"
	                     "\n"));
	std::optional<std::string> line = client->ReadLine();
	REQUIRE(line);
	Result<Json> batch = ParseJson(*line);
	REQUIRE(batch.IsOk());
	REQUIRE(batch.GetValue().is_array());
	REQUIRE(batch.GetValue().size() == 2);
	CHECK(batch.GetValue()[1]["result"]["entities"][0]["name"] == "Remote");

	// Several clients are served independently.
	Scope<TcpTestClient> second = fixture.Connect();
	CHECK(second->Call("editor.status"));
}

TEST_CASE("AutomationServer: unauthenticated and malformed traffic is rejected")
{
	ServerFixture fixture;

	SUBCASE("commands before authenticating")
	{
		Scope<TcpTestClient> client = fixture.Connect(false);
		CHECK(ErrorCode(client->Call("editor.status")) == Code(AutomationErrorCode::Unauthenticated));
		CHECK(client->WaitForClose());
	}
	SUBCASE("wrong token")
	{
		Scope<TcpTestClient> client = fixture.Connect(false);
		CHECK(ErrorCode(client->Call("authenticate", Json::object({{"token", "guess"}}))) == Code(AutomationErrorCode::Unauthenticated));
		CHECK(client->WaitForClose());
	}
	SUBCASE("malformed JSON after authenticating keeps the connection")
	{
		Scope<TcpTestClient> client = fixture.Connect();
		REQUIRE(client->Send("{oops\n"));
		std::optional<std::string> line = client->ReadLine();
		REQUIRE(line);
		CHECK(ParseJson(*line).GetValue()["error"]["code"] == Code(AutomationErrorCode::ParseError));
		CHECK(client->Call("editor.status"));
	}
}

TEST_CASE("AutomationServer: oversized messages and excess connections are refused")
{
	AutomationServerSpecification specification;
	specification.MaxMessageSize = 1024;
	specification.MaxConnections = 1;
	ServerFixture fixture(specification);

	Scope<TcpTestClient> client = fixture.Connect();
	REQUIRE(client->Send(std::string(4096, 'x')));
	std::optional<std::string> line = client->ReadLine();
	REQUIRE(line);
	CHECK(ParseJson(*line).GetValue()["error"]["code"] == Code(AutomationErrorCode::MessageTooLarge));
	CHECK(client->WaitForClose());

	Scope<TcpTestClient> first = fixture.Connect();
	TcpTestClient extra(
		[&fixture]
		{
			fixture.Server.ProcessRequests();
		});
	REQUIRE(extra.Connect(fixture.Server.GetPort()));
	std::optional<std::string> busy = extra.ReadLine();
	REQUIRE(busy);
	CHECK(ParseJson(*busy).GetValue()["error"]["code"] == Code(AutomationErrorCode::ServerBusy));
}

TEST_CASE("AutomationServer: stopping disconnects clients and can restart")
{
	ServerFixture fixture;
	Scope<TcpTestClient> client = fixture.Connect();
	fixture.Server.Stop();
	CHECK_FALSE(fixture.Server.IsRunning());
	CHECK(client->WaitForClose());
	CHECK(fixture.Server.Start().IsOk());
	CHECK(fixture.Connect()->Call("editor.status"));
}

TEST_CASE("AutomationInstance: files round trip and stale instances are cleaned up")
{
	AutomationInstanceInfo info;
	info.ProcessID = Platform::GetProcessID();
	info.Port = 4242;
	info.Token = GenerateAutomationToken();
	info.Project = "C:/Games/Tetris/Tetris.sproj";
	info.Version = "0.1.0";

	Result<std::filesystem::path> written = AutomationInstance::Write(info);
	REQUIRE(written.IsOk());
	CHECK(written.GetValue() == AutomationInstance::GetPath(info.ProcessID));
#if !defined(ST_PLATFORM_WINDOWS)
	std::filesystem::perms const permissions = std::filesystem::status(written.GetValue()).permissions();
	CHECK((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif

	Result<AutomationInstanceInfo> read = AutomationInstance::Read(written.GetValue());
	REQUIRE(read.IsOk());
	CHECK(read.GetValue().Port == 4242);
	CHECK(read.GetValue().Token == info.Token);
	CHECK(read.GetValue().Project == info.Project);

	// A file left by a process that no longer exists is removed by the scan.
	AutomationInstanceInfo stale = info;
	stale.ProcessID = 0x7FFFFFF0u;
	REQUIRE_FALSE(Platform::IsProcessRunning(stale.ProcessID));
	REQUIRE(AutomationInstance::Write(stale).IsOk());

	std::set<uint32_t> running;
	for (AutomationInstanceInfo const& instance : AutomationInstance::FindRunningInstances())
	{
		running.insert(instance.ProcessID);
	}
	CHECK(running.contains(info.ProcessID));
	CHECK_FALSE(running.contains(stale.ProcessID));
	CHECK_FALSE(FileSystem::Exists(AutomationInstance::GetPath(stale.ProcessID)));

	AutomationInstance::Remove(info.ProcessID);
	CHECK_FALSE(FileSystem::Exists(written.GetValue()));
	CHECK(Platform::IsProcessRunning(Platform::GetProcessID()));
	CHECK_FALSE(Platform::IsProcessRunning(0));
}
