#pragma once

#include "Strada/Core/Assert.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Strada
{
	// Error codes of the automation protocol (documented in Docs/Automation.md; never renumber them). Negative codes are
	// JSON-RPC 2.0's own and its implementation-defined server-error range (protocol and transport problems); positive
	// codes are failures of a command that was understood.
	enum class AutomationErrorCode : int32_t
	{
		ParseError = -32700,
		InvalidRequest = -32600,
		MethodNotFound = -32601,
		InvalidParams = -32602,
		InternalError = -32603,
		// The connection has not authenticated, or presented a wrong token.
		Unauthenticated = -32001,
		// The server does not accept more connections.
		ServerBusy = -32002,
		// A message exceeded the size limit; the connection is closed because the framing is lost.
		MessageTooLarge = -32003,
		// A parameter names an entity that does not exist.
		EntityNotFound = 1001,
		// A parameter names an unknown component type, or a component the entity does not have.
		ComponentNotFound = 1002,
		// The request is well-formed but not possible in the current state (nothing to undo, hierarchy cycle, core
		// component, ...).
		InvalidOperation = 1003,
		// Reading or writing a file failed, or a file has invalid contents.
		FileError = 1004,
		// The feature is not available in this editor configuration (for example screenshots without a window).
		Unavailable = 1005,
		// An asynchronous command was abandoned before completing (for example because the editor closed).
		Cancelled = 1006,
		// The edited scene has unsaved changes that the command would discard.
		UnsavedChanges = 1007
	};

	inline constexpr std::array<AutomationErrorCode, 15> AllAutomationErrorCodes = {
		AutomationErrorCode::ParseError,        AutomationErrorCode::InvalidRequest,   AutomationErrorCode::MethodNotFound,
		AutomationErrorCode::InvalidParams,     AutomationErrorCode::InternalError,    AutomationErrorCode::Unauthenticated,
		AutomationErrorCode::ServerBusy,        AutomationErrorCode::MessageTooLarge,  AutomationErrorCode::EntityNotFound,
		AutomationErrorCode::ComponentNotFound, AutomationErrorCode::InvalidOperation, AutomationErrorCode::FileError,
		AutomationErrorCode::Unavailable,       AutomationErrorCode::Cancelled,        AutomationErrorCode::UnsavedChanges};

	// The enumerator name, as used in the documentation.
	std::string_view GetAutomationErrorCodeName(AutomationErrorCode code);

	struct CommandError
	{
		AutomationErrorCode Code = AutomationErrorCode::InternalError;
		std::string Message;
		// Optional structured details (an object); null when there are none.
		Json Data;
	};

	template<typename... Args>
	[[nodiscard]] CommandError MakeCommandError(AutomationErrorCode code, fmt::format_string<Args...> format, Args&&... args)
	{
		return CommandError{code, fmt::format(format, std::forward<Args>(args)...), Json()};
	}

	// A value or a structured command error; CommandValue<Json> is the outcome of every automation command.
	template<typename T>
	class [[nodiscard]] CommandValue
	{
	public:
		CommandValue(T value)
			: m_Storage(std::in_place_index<0>, std::move(value))
		{
		}

		CommandValue(CommandError error)
			: m_Storage(std::in_place_index<1>, std::move(error))
		{
		}

		bool IsOk() const { return m_Storage.index() == 0; }
		bool IsError() const { return m_Storage.index() == 1; }
		explicit operator bool() const { return IsOk(); }

		T& GetValue() &
		{
			ST_CORE_ASSERT(IsOk(), "CommandValue::GetValue called on an error");
			return std::get<0>(m_Storage);
		}

		T const& GetValue() const&
		{
			ST_CORE_ASSERT(IsOk(), "CommandValue::GetValue called on an error");
			return std::get<0>(m_Storage);
		}

		// Moves the value out.
		T TakeValue()
		{
			ST_CORE_ASSERT(IsOk(), "CommandValue::TakeValue called on an error");
			return std::move(std::get<0>(m_Storage));
		}

		CommandError const& GetError() const
		{
			ST_CORE_ASSERT(IsError(), "CommandValue::GetError called on a value");
			return std::get<1>(m_Storage);
		}

		// Moves the error out (to return it from a handler).
		CommandError TakeError()
		{
			ST_CORE_ASSERT(IsError(), "CommandValue::TakeError called on a value");
			return std::move(std::get<1>(m_Storage));
		}

	private:
		std::variant<T, CommandError> m_Storage;
	};

	using CommandResult = CommandValue<Json>;
}
