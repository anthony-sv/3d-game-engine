#include "Editor/Automation/AutomationServer.h"

#include "Strada/Core/Log.h"
#include "Strada/Core/Version.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#if defined(ST_PLATFORM_WINDOWS)
#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace Strada
{
	namespace
	{
#if defined(ST_PLATFORM_WINDOWS)
		using SocketHandle = SOCKET;
		constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
		using PollDescriptor = WSAPOLLFD;

		void CloseSocket(SocketHandle socket)
		{
			::closesocket(socket);
		}

		int PollSockets(PollDescriptor* descriptors, size_t count, int timeoutMs)
		{
			return ::WSAPoll(descriptors, static_cast<ULONG>(count), timeoutMs);
		}

		bool SetNonBlocking(SocketHandle socket)
		{
			u_long enabled = 1;
			return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
		}

		bool WouldBlock()
		{
			return ::WSAGetLastError() == WSAEWOULDBLOCK;
		}

		// Winsock is reference counted per process: every server start/stop pairs WSAStartup/WSACleanup.
		bool InitializeSockets()
		{
			WSADATA data;
			return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
		}

		void ShutdownSockets()
		{
			::WSACleanup();
		}

		constexpr int SendFlags = 0;
#else
		using SocketHandle = int;
		constexpr SocketHandle InvalidSocket = -1;
		using PollDescriptor = pollfd;

		void CloseSocket(SocketHandle socket)
		{
			::close(socket);
		}

		int PollSockets(PollDescriptor* descriptors, size_t count, int timeoutMs)
		{
			return ::poll(descriptors, static_cast<nfds_t>(count), timeoutMs);
		}

		bool SetNonBlocking(SocketHandle socket)
		{
			int const flags = ::fcntl(socket, F_GETFL, 0);
			return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
		}

		bool WouldBlock()
		{
#if EAGAIN == EWOULDBLOCK
			return errno == EAGAIN || errno == EINTR;
#else
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
		}

		bool InitializeSockets()
		{
			return true;
		}

		void ShutdownSockets()
		{
		}

#if defined(MSG_NOSIGNAL)
		// A client that disconnects mid-write must not kill the editor with SIGPIPE.
		constexpr int SendFlags = MSG_NOSIGNAL;
#else
		constexpr int SendFlags = 0;
#endif
#endif

		SocketHandle ToSocket(uintptr_t value)
		{
			return static_cast<SocketHandle>(value);
		}

		constexpr int PollTimeoutMs = 20;
		constexpr size_t ReceiveChunkSize = 64 * 1024;

		struct Connection
		{
			uint64_t ID = 0;
			SocketHandle Socket = InvalidSocket;
			std::string ReadBuffer;
			std::string WriteBuffer;
			bool Authenticated = false;
			// Close once WriteBuffer is flushed (after a protocol violation or a failed authentication).
			bool CloseAfterFlush = false;
			bool Closed = false;
		};

		std::string ErrorLine(Json const& id, AutomationErrorCode code, std::string message)
		{
			return JsonRpc::Serialize(JsonRpc::MakeError(id, CommandError{code, std::move(message), Json()}));
		}
	}

	// State shared between the server, the network thread and command callbacks that may outlive the server.
	struct AutomationServer::Shared
	{
		std::mutex Mutex;
		std::vector<std::pair<uint64_t, std::string>> Outbox;
	};

	bool SecretsEqual(std::string_view a, std::string_view b)
	{
		size_t const length = std::max(a.size(), b.size());
		unsigned difference = a.size() == b.size() ? 0u : 1u;
		for (size_t i = 0; i < length; i++)
		{
			unsigned char const left = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
			unsigned char const right = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
			difference |= static_cast<unsigned>(left ^ right);
		}
		return difference == 0;
	}

	std::string GenerateAutomationToken()
	{
		// std::random_device draws from the OS cryptographic source on every supported standard library (RtlGenRandom,
		// getrandom/urandom, arc4random).
		std::random_device device;
		std::string token;
		token.reserve(64);
		constexpr char Digits[] = "0123456789abcdef";
		for (int i = 0; i < 8; i++)
		{
			uint32_t const value = device();
			for (int nibble = 7; nibble >= 0; nibble--)
			{
				token.push_back(Digits[(value >> (nibble * 4)) & 0xF]);
			}
		}
		return token;
	}

	AutomationServer::AutomationServer(CommandRegistry const& registry)
		: m_Registry(registry)
	{
	}

	AutomationServer::~AutomationServer()
	{
		Stop();
	}

	Result<void> AutomationServer::Start(AutomationServerSpecification specification)
	{
		if (IsRunning())
		{
			return Error{"the automation server is already running"};
		}
		if (specification.MaxMessageSize == 0 || specification.MaxConnections == 0)
		{
			return Error{"the automation server needs a positive message size and connection limit"};
		}
		if (!InitializeSockets())
		{
			return Error{"failed to initialize the socket library"};
		}

		SocketHandle const listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == InvalidSocket)
		{
			ShutdownSockets();
			return Error{"failed to create the automation socket"};
		}
#if defined(ST_PLATFORM_WINDOWS)
		// Prevent other processes from binding the same port and intercepting requests.
		BOOL exclusive = TRUE;
		::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&exclusive), sizeof(exclusive));
#endif

		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(specification.Port);
		socklen_t addressLength = sizeof(address);
		if (::bind(listener, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0 || ::listen(listener, 16) != 0 ||
		    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0 || !SetNonBlocking(listener))
		{
			CloseSocket(listener);
			ShutdownSockets();
			return MakeError("failed to listen on 127.0.0.1:{} (is the port in use?)", specification.Port);
		}

		m_Specification = specification;
		m_Token = specification.Token.empty() ? GenerateAutomationToken() : specification.Token;
		m_Port = ntohs(address.sin_port);
		m_Shared = CreateRef<Shared>();
		m_StopRequested = false;
		m_Thread = std::thread(&AutomationServer::RunNetworkThread, this, static_cast<uintptr_t>(listener));
		ST_CORE_INFO("Automation server listening on 127.0.0.1:{}", m_Port);
		return {};
	}

	void AutomationServer::Stop()
	{
		if (!IsRunning())
		{
			return;
		}
		m_StopRequested = true;
		m_Thread.join();
		ShutdownSockets();
		{
			std::lock_guard lock(m_InboxMutex);
			m_Inbox.clear();
		}
		m_Shared.reset();
		m_ConnectionCount = 0;
		ST_CORE_INFO("Automation server stopped");
	}

	void AutomationServer::ProcessRequests()
	{
		std::deque<Incoming> incoming;
		{
			std::lock_guard lock(m_InboxMutex);
			incoming.swap(m_Inbox);
		}

		for (Incoming& message : incoming)
		{
			struct Batch
			{
				uint64_t ConnectionID = 0;
				bool IsBatch = false;
				std::vector<Json> Responses;
				size_t Remaining = 0;
			};
			auto batch = CreateRef<Batch>();
			batch->ConnectionID = message.ConnectionID;
			batch->IsBatch = message.Message.IsBatch;
			batch->Responses.resize(message.Message.Requests.size());
			batch->Remaining = message.Message.Requests.size();

			std::weak_ptr<Shared> const shared = m_Shared;
			auto const deliver = [shared, batch](size_t index, Json response)
			{
				batch->Responses[index] = std::move(response);
				if (--batch->Remaining > 0)
				{
					return;
				}
				Json answer = Json::array();
				for (Json& entry : batch->Responses)
				{
					if (!entry.is_null())
					{
						answer.push_back(std::move(entry));
					}
				}
				Ref<Shared> const state = shared.lock();
				if (answer.empty() || !state)
				{
					return;
				}
				std::string line = JsonRpc::Serialize(batch->IsBatch ? answer : answer[0]);
				std::lock_guard lock(state->Mutex);
				state->Outbox.emplace_back(batch->ConnectionID, std::move(line));
			};

			for (size_t i = 0; i < message.Message.Requests.size(); i++)
			{
				JsonRpc::Request const& request = message.Message.Requests[i];
				if (request.Error)
				{
					deliver(i, JsonRpc::MakeError(request.Id.value_or(Json()), *request.Error));
					continue;
				}
				m_Registry.Execute(request.Method, request.Params,
				                   [deliver, i, request](CommandResult result)
				                   {
									   deliver(i, request.IsNotification() ? Json() : JsonRpc::MakeResponse(request, result));
								   });
			}
		}
	}

	void AutomationServer::RunNetworkThread(uintptr_t listenSocket)
	{
		SocketHandle const listener = ToSocket(listenSocket);
		std::map<uint64_t, Connection> connections;
		uint64_t nextConnectionID = 1;
		std::vector<PollDescriptor> descriptors;
		std::vector<uint64_t> descriptorOwners;

		auto const handleLine = [this](Connection& connection, std::string_view line)
		{
			CommandValue<JsonRpc::Message> parsed = JsonRpc::Parse(line);
			if (!parsed)
			{
				connection.WriteBuffer += JsonRpc::Serialize(JsonRpc::MakeError(Json(), parsed.GetError()));
				if (!connection.Authenticated)
				{
					connection.CloseAfterFlush = true;
				}
				return;
			}

			JsonRpc::Message& message = parsed.GetValue();
			bool const isAuthentication = !message.IsBatch && message.Requests.size() == 1 && !message.Requests[0].Error &&
			                              message.Requests[0].Method == "authenticate";
			if (isAuthentication)
			{
				JsonRpc::Request const& request = message.Requests[0];
				Json const id = request.Id.value_or(Json());
				auto const token = request.Params.is_object() ? request.Params.find("token") : request.Params.end();
				bool const valid = request.Params.is_object() && token != request.Params.end() && token->is_string() &&
				                   SecretsEqual(token->get_ref<std::string const&>(), m_Token);
				if (!valid)
				{
					connection.WriteBuffer += ErrorLine(id, AutomationErrorCode::Unauthenticated, "invalid automation token");
					connection.CloseAfterFlush = true;
					return;
				}
				connection.Authenticated = true;
				if (!request.IsNotification())
				{
					connection.WriteBuffer += JsonRpc::Serialize(
						JsonRpc::MakeResult(id, Json::object({{"authenticated", true}, {"version", EngineVersion::String}})));
				}
				return;
			}
			if (!connection.Authenticated)
			{
				connection.WriteBuffer +=
					ErrorLine(Json(), AutomationErrorCode::Unauthenticated, "authenticate with the instance token before sending commands");
				connection.CloseAfterFlush = true;
				return;
			}

			std::lock_guard lock(m_InboxMutex);
			m_Inbox.push_back(Incoming{connection.ID, std::move(message)});
		};

		while (!m_StopRequested)
		{
			if (Ref<Shared> const shared = m_Shared)
			{
				std::lock_guard lock(shared->Mutex);
				for (auto& [connectionID, line] : shared->Outbox)
				{
					if (auto const it = connections.find(connectionID); it != connections.end())
					{
						it->second.WriteBuffer += line;
					}
				}
				shared->Outbox.clear();
			}

			descriptors.clear();
			descriptorOwners.clear();
			descriptors.push_back(PollDescriptor{listener, POLLIN, 0});
			descriptorOwners.push_back(0);
			for (auto const& [connectionID, connection] : connections)
			{
				short events = connection.CloseAfterFlush ? 0 : POLLIN;
				if (!connection.WriteBuffer.empty())
				{
					events |= POLLOUT;
				}
				descriptors.push_back(PollDescriptor{connection.Socket, events, 0});
				descriptorOwners.push_back(connectionID);
			}

			if (PollSockets(descriptors.data(), descriptors.size(), PollTimeoutMs) < 0)
			{
				continue;
			}

			if ((descriptors[0].revents & POLLIN) != 0)
			{
				SocketHandle const accepted = ::accept(listener, nullptr, nullptr);
				if (accepted != InvalidSocket)
				{
					if (!SetNonBlocking(accepted))
					{
						CloseSocket(accepted);
					}
					else
					{
						Connection connection;
						connection.ID = nextConnectionID++;
						connection.Socket = accepted;
						if (connections.size() >= m_Specification.MaxConnections)
						{
							connection.WriteBuffer = ErrorLine(Json(), AutomationErrorCode::ServerBusy, "too many automation connections");
							connection.CloseAfterFlush = true;
						}
						connections.emplace(connection.ID, std::move(connection));
					}
				}
			}

			for (size_t d = 1; d < descriptors.size(); d++)
			{
				Connection& connection = connections.at(descriptorOwners[d]);
				short const revents = descriptors[d].revents;

				if ((revents & POLLIN) != 0 && !connection.CloseAfterFlush)
				{
					std::array<char, ReceiveChunkSize> chunk;
					int const received = static_cast<int>(::recv(connection.Socket, chunk.data(), static_cast<int>(chunk.size()), 0));
					if (received > 0)
					{
						connection.ReadBuffer.append(chunk.data(), static_cast<size_t>(received));
						size_t start = 0;
						for (size_t newline = connection.ReadBuffer.find('\n'); newline != std::string::npos && !connection.CloseAfterFlush;
						     newline = connection.ReadBuffer.find('\n', start))
						{
							std::string_view line(connection.ReadBuffer.data() + start, newline - start);
							start = newline + 1;
							if (!line.empty() && line.back() == '\r')
							{
								line.remove_suffix(1);
							}
							if (line.size() > m_Specification.MaxMessageSize)
							{
								connection.WriteBuffer +=
									ErrorLine(Json(), AutomationErrorCode::MessageTooLarge, "message exceeds the size limit");
								connection.CloseAfterFlush = true;
							}
							else if (line.find_first_not_of(" \t") != std::string_view::npos)
							{
								handleLine(connection, line);
							}
						}
						connection.ReadBuffer.erase(0, start);
						if (connection.ReadBuffer.size() > m_Specification.MaxMessageSize && !connection.CloseAfterFlush)
						{
							connection.WriteBuffer +=
								ErrorLine(Json(), AutomationErrorCode::MessageTooLarge, "message exceeds the size limit");
							connection.CloseAfterFlush = true;
							connection.ReadBuffer.clear();
						}
					}
					else if (received == 0 || !WouldBlock())
					{
						connection.Closed = true;
					}
				}

				if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (revents & POLLIN) == 0)
				{
					connection.Closed = true;
				}

				if (!connection.Closed && !connection.WriteBuffer.empty() && (revents & POLLOUT) != 0)
				{
					int const sent =
						static_cast<int>(::send(connection.Socket, connection.WriteBuffer.data(),
					                            static_cast<int>(std::min<size_t>(connection.WriteBuffer.size(), 1 << 20)), SendFlags));
					if (sent > 0)
					{
						connection.WriteBuffer.erase(0, static_cast<size_t>(sent));
					}
					else if (!WouldBlock())
					{
						connection.Closed = true;
					}
				}
				if (connection.CloseAfterFlush && connection.WriteBuffer.empty())
				{
					connection.Closed = true;
				}
			}

			for (auto it = connections.begin(); it != connections.end();)
			{
				if (it->second.Closed)
				{
					CloseSocket(it->second.Socket);
					it = connections.erase(it);
				}
				else
				{
					++it;
				}
			}
			m_ConnectionCount = connections.size();
		}

		// Deliver responses queued before Stop (such as the reply to editor.quit), giving slow clients a short deadline.
		if (Ref<Shared> const shared = m_Shared)
		{
			std::lock_guard lock(shared->Mutex);
			for (auto& [connectionID, line] : shared->Outbox)
			{
				if (auto const it = connections.find(connectionID); it != connections.end())
				{
					it->second.WriteBuffer += line;
				}
			}
			shared->Outbox.clear();
		}
		constexpr int FlushDeadlineMs = 1000;
		for (int waitedMs = 0; waitedMs < FlushDeadlineMs; waitedMs += PollTimeoutMs)
		{
			descriptors.clear();
			descriptorOwners.clear();
			for (auto const& [connectionID, connection] : connections)
			{
				if (!connection.Closed && !connection.WriteBuffer.empty())
				{
					descriptors.push_back(PollDescriptor{connection.Socket, POLLOUT, 0});
					descriptorOwners.push_back(connectionID);
				}
			}
			if (descriptors.empty() || PollSockets(descriptors.data(), descriptors.size(), PollTimeoutMs) < 0)
			{
				break;
			}
			for (size_t d = 0; d < descriptors.size(); d++)
			{
				Connection& connection = connections.at(descriptorOwners[d]);
				if ((descriptors[d].revents & POLLOUT) == 0)
				{
					if ((descriptors[d].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
					{
						connection.Closed = true;
					}
					continue;
				}
				int const sent =
					static_cast<int>(::send(connection.Socket, connection.WriteBuffer.data(),
				                            static_cast<int>(std::min<size_t>(connection.WriteBuffer.size(), 1 << 20)), SendFlags));
				if (sent > 0)
				{
					connection.WriteBuffer.erase(0, static_cast<size_t>(sent));
				}
				else if (!WouldBlock())
				{
					connection.Closed = true;
				}
			}
		}

		for (auto& [connectionID, connection] : connections)
		{
			CloseSocket(connection.Socket);
		}
		CloseSocket(listener);
	}
}
