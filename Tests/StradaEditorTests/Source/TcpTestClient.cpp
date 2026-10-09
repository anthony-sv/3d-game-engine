#include "TcpTestClient.h"

#include <array>
#include <chrono>

#if defined(ST_PLATFORM_WINDOWS)
#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Strada::Testing
{
	namespace
	{
#if defined(ST_PLATFORM_WINDOWS)
		using SocketHandle = SOCKET;
		SocketHandle const InvalidSocket = INVALID_SOCKET;
		using PollDescriptor = WSAPOLLFD;
		int PollOne(PollDescriptor& descriptor, int timeoutMs)
		{
			return ::WSAPoll(&descriptor, 1, timeoutMs);
		}
		void CloseSocket(SocketHandle socket)
		{
			::closesocket(socket);
		}
#else
		using SocketHandle = int;
		SocketHandle const InvalidSocket = -1;
		using PollDescriptor = pollfd;
		int PollOne(PollDescriptor& descriptor, int timeoutMs)
		{
			return ::poll(&descriptor, 1, timeoutMs);
		}
		void CloseSocket(SocketHandle socket)
		{
			::close(socket);
		}
#endif

		SocketHandle ToSocket(uintptr_t value)
		{
			return static_cast<SocketHandle>(value);
		}
	}

	TcpTestClient::TcpTestClient(std::function<void()> pump)
		: m_Pump(std::move(pump)),
		  m_Socket(static_cast<uintptr_t>(InvalidSocket))
	{
#if defined(ST_PLATFORM_WINDOWS)
		WSADATA data;
		::WSAStartup(MAKEWORD(2, 2), &data);
#endif
	}

	TcpTestClient::~TcpTestClient()
	{
		if (ToSocket(m_Socket) != InvalidSocket)
		{
			CloseSocket(ToSocket(m_Socket));
		}
#if defined(ST_PLATFORM_WINDOWS)
		::WSACleanup();
#endif
	}

	bool TcpTestClient::Connect(uint16_t port)
	{
		SocketHandle const socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket == InvalidSocket)
		{
			return false;
		}
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		if (::connect(socket, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0)
		{
			CloseSocket(socket);
			return false;
		}
		m_Socket = static_cast<uintptr_t>(socket);
		return true;
	}

	bool TcpTestClient::Send(std::string_view data)
	{
		while (!data.empty())
		{
			int const sent = static_cast<int>(::send(ToSocket(m_Socket), data.data(), static_cast<int>(data.size()), 0));
			if (sent <= 0)
			{
				return false;
			}
			data.remove_prefix(static_cast<size_t>(sent));
		}
		return true;
	}

	std::optional<std::string> TcpTestClient::ReadLine(int timeoutMs)
	{
		auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (true)
		{
			if (size_t const newline = m_Buffer.find('\n'); newline != std::string::npos)
			{
				std::string line = m_Buffer.substr(0, newline);
				m_Buffer.erase(0, newline + 1);
				return line;
			}
			if (m_Closed || std::chrono::steady_clock::now() >= deadline)
			{
				return std::nullopt;
			}

			if (m_Pump)
			{
				m_Pump();
			}
			PollDescriptor descriptor{ToSocket(m_Socket), POLLIN, 0};
			if (PollOne(descriptor, 5) <= 0)
			{
				continue;
			}
			std::array<char, 65536> chunk;
			int const received = static_cast<int>(::recv(ToSocket(m_Socket), chunk.data(), static_cast<int>(chunk.size()), 0));
			if (received <= 0)
			{
				m_Closed = true;
				continue;
			}
			m_Buffer.append(chunk.data(), static_cast<size_t>(received));
		}
	}

	bool TcpTestClient::WaitForClose(int timeoutMs)
	{
		while (ReadLine(timeoutMs))
		{
		}
		return m_Closed;
	}

	std::optional<Json> TcpTestClient::Call(std::string_view method, Json params, int timeoutMs)
	{
		Json request =
			Json::object({{"jsonrpc", "2.0"}, {"id", m_NextID++}, {"method", std::string(method)}, {"params", std::move(params)}});
		if (!Send(request.dump() + "\n"))
		{
			return std::nullopt;
		}
		std::optional<std::string> const line = ReadLine(timeoutMs);
		if (!line)
		{
			return std::nullopt;
		}
		Result<Json> response = ParseJson(*line);
		return response ? std::optional<Json>(response.GetValue()) : std::nullopt;
	}
}
