#pragma once

#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Strada::Testing
{
	// Minimal blocking-with-timeout TCP client for automation server tests. While waiting for data it calls the pump
	// (normally AutomationServer::ProcessRequests), because the server only runs requests when the main thread asks it to.
	class TcpTestClient
	{
	public:
		explicit TcpTestClient(std::function<void()> pump);
		~TcpTestClient();

		TcpTestClient(TcpTestClient const&) = delete;
		TcpTestClient& operator=(TcpTestClient const&) = delete;

		bool Connect(uint16_t port);
		bool Send(std::string_view data);
		// The next line without its terminator; empty optional on timeout or when the server closed the connection.
		std::optional<std::string> ReadLine(int timeoutMs = 5000);
		// True when the server closed the connection within the timeout.
		bool WaitForClose(int timeoutMs = 5000);

		// Sends one request and reads the response.
		std::optional<Json> Call(std::string_view method, Json params = Json::object(), int timeoutMs = 5000);

	private:
		std::function<void()> m_Pump;
		uintptr_t m_Socket;
		std::string m_Buffer;
		bool m_Closed = false;
		int m_NextID = 1;
	};
}
