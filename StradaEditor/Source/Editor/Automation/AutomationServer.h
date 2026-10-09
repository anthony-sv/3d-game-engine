#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/JsonRpc.h"

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace Strada
{
	struct AutomationServerSpecification
	{
		// 0 picks a free port (read it back with GetPort).
		uint16_t Port = 0;
		// Lines longer than this close the connection (the framing would be lost).
		size_t MaxMessageSize = 16 * 1024 * 1024;
		// Further clients are told the server is busy and disconnected.
		uint32_t MaxConnections = 8;
		// Empty generates a random 256-bit token (64 hex characters).
		std::string Token;
	};

	// JSON-RPC 2.0 automation endpoint: newline-delimited messages over TCP on 127.0.0.1 only. Each connection must first
	// call "authenticate" with { "token": "<token>" }; anything else before that is answered with Unauthenticated and the
	// connection is closed. A background thread owns the sockets and never touches editor state: requests run on the main
	// thread in ProcessRequests, through the command registry. Protocol details are in Docs/Automation.md.
	class AutomationServer
	{
	public:
		explicit AutomationServer(CommandRegistry const& registry);
		~AutomationServer();

		AutomationServer(AutomationServer const&) = delete;
		AutomationServer& operator=(AutomationServer const&) = delete;

		[[nodiscard]] Result<void> Start(AutomationServerSpecification specification = {});
		// Closes every connection and joins the network thread. Pending asynchronous commands complete into nothing.
		void Stop();
		bool IsRunning() const { return m_Thread.joinable(); }

		uint16_t GetPort() const { return m_Port; }
		std::string const& GetToken() const { return m_Token; }
		// Thread-safe.
		size_t GetConnectionCount() const { return m_ConnectionCount.load(); }

		// Runs the requests received since the last call. Main thread only; the editor calls it once per frame.
		void ProcessRequests();

	private:
		struct Shared;
		struct Incoming
		{
			uint64_t ConnectionID = 0;
			JsonRpc::Message Message;
		};

		void RunNetworkThread(uintptr_t listenSocket);

		CommandRegistry const& m_Registry;
		AutomationServerSpecification m_Specification;
		std::string m_Token;
		uint16_t m_Port = 0;

		std::thread m_Thread;
		std::atomic<bool> m_StopRequested = false;
		std::atomic<size_t> m_ConnectionCount = 0;

		// Network thread -> main thread.
		std::mutex m_InboxMutex;
		std::deque<Incoming> m_Inbox;
		// Main thread -> network thread; held weakly by command callbacks so late completions after Stop are dropped.
		Ref<Shared> m_Shared;
	};

	// Constant-time comparison for secrets (independent of where the first difference is).
	bool SecretsEqual(std::string_view a, std::string_view b);
	// 32 random bytes from the OS random source as 64 lowercase hex characters.
	std::string GenerateAutomationToken();
}
