#include "Editor/Automation/CommandRegistry.h"

#include "Editor/Automation/JsonSchema.h"

#include "Strada/Core/Log.h"

#include <exception>
#include <optional>
#include <utility>

namespace Strada
{
	namespace
	{
		CommandError MakeExceptionError(std::string_view command, char const* what)
		{
			// Handlers must not throw; reaching this is a bug in the handler, but it must not take the editor down.
			ST_ERROR("Automation command '{}' failed with an exception: {}", command, what);
			return MakeCommandError(AutomationErrorCode::InternalError, "command '{}' failed unexpectedly: {}", command, what);
		}

		bool IsNamePart(std::string_view part)
		{
			if (part.empty() || part.front() < 'a' || part.front() > 'z' || part.back() == '-')
			{
				return false;
			}
			for (char const character : part)
			{
				bool const valid = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
				if (!valid)
				{
					return false;
				}
			}
			return true;
		}
	}

	struct CommandCompletion::State
	{
		CommandCallback Callback;
		bool Completed = false;

		explicit State(CommandCallback callback)
			: Callback(std::move(callback))
		{
		}

		State(State const&) = delete;
		State& operator=(State const&) = delete;

		~State()
		{
			if (Completed || !Callback)
			{
				return;
			}
			try
			{
				Callback(MakeCommandError(AutomationErrorCode::Cancelled, "the command was abandoned before it completed"));
			}
			catch (std::exception const& exception)
			{
				ST_ERROR("Failed to report an abandoned automation command: {}", exception.what());
			}
		}
	};

	CommandCompletion::CommandCompletion(CommandCallback callback)
		: m_State(std::make_shared<State>(std::move(callback)))
	{
		ST_CORE_ASSERT(m_State->Callback, "A command completion needs a callback");
	}

	bool CommandCompletion::Complete(CommandResult result) const
	{
		if (m_State->Completed)
		{
			return false;
		}
		// Marked first so a callback that completes again (directly or indirectly) is ignored; the callback is moved out
		// so whatever it captured is released now rather than when the last copy of the completion goes away.
		m_State->Completed = true;
		CommandCallback const callback = std::move(m_State->Callback);
		callback(std::move(result));
		return true;
	}

	bool CommandCompletion::IsCompleted() const
	{
		return m_State->Completed;
	}

	bool CommandRegistry::IsValidCommandName(std::string_view name)
	{
		size_t const dot = name.find('.');
		if (dot == std::string_view::npos)
		{
			return false;
		}
		return IsNamePart(name.substr(0, dot)) && IsNamePart(name.substr(dot + 1));
	}

	Result<void> CommandRegistry::Register(CommandDefinition definition)
	{
		if (!IsValidCommandName(definition.Name))
		{
			return MakeError("invalid command name '{}' (expected <domain>.<action> in lowercase letters, digits and hyphens)",
			                 definition.Name);
		}
		if (m_Commands.contains(definition.Name))
		{
			return MakeError("command '{}' is already registered", definition.Name);
		}
		if (definition.Description.empty())
		{
			return MakeError("command '{}' has no description", definition.Name);
		}
		if (static_cast<bool>(definition.Handler) == static_cast<bool>(definition.AsyncHandler))
		{
			return MakeError("command '{}' needs exactly one handler (synchronous or asynchronous)", definition.Name);
		}

		if (definition.Parameters.is_null())
		{
			definition.Parameters = Json::object({{"type", "object"}, {"additionalProperties", false}});
		}
		if (Result<void> result = JsonSchema::CheckDefinition(definition.Parameters); !result)
		{
			return MakeError("command '{}': {}", definition.Name, result.GetError());
		}
		auto const type = definition.Parameters.find("type");
		if (type == definition.Parameters.end() || *type != "object")
		{
			return MakeError("command '{}': the parameter schema must have type \"object\"", definition.Name);
		}

		std::string name = definition.Name;
		m_Commands.emplace(std::move(name), std::move(definition));
		return {};
	}

	CommandDefinition const* CommandRegistry::Find(std::string_view name) const
	{
		auto const it = m_Commands.find(name);
		return it != m_Commands.end() ? &it->second : nullptr;
	}

	std::vector<CommandDefinition const*> CommandRegistry::GetCommands() const
	{
		std::vector<CommandDefinition const*> commands;
		commands.reserve(m_Commands.size());
		for (auto const& [name, command] : m_Commands)
		{
			commands.push_back(&command);
		}
		return commands;
	}

	Json CommandRegistry::Describe() const
	{
		Json commands = Json::array();
		for (auto const& [name, command] : m_Commands)
		{
			Json entry = Json::object();
			entry["name"] = name;
			entry["description"] = command.Description;
			entry["params"] = command.Parameters;
			entry["readOnly"] = command.ReadOnly;
			entry["async"] = command.IsAsync();
			commands.push_back(std::move(entry));
		}
		return commands;
	}

	void CommandRegistry::Execute(std::string_view name, Json const& params, CommandCallback callback) const
	{
		ST_CORE_ASSERT(callback, "CommandRegistry::Execute needs a callback");

		CommandDefinition const* command = Find(name);
		if (command == nullptr)
		{
			callback(
				MakeCommandError(AutomationErrorCode::MethodNotFound, "unknown command '{}' (editor.commands lists every command)", name));
			return;
		}

		Json const arguments = params.is_null() ? Json::object() : params;
		if (!arguments.is_object())
		{
			callback(
				MakeCommandError(AutomationErrorCode::InvalidParams, "the parameters of '{}' must be an object of named values", name));
			return;
		}
		if (std::optional<SchemaViolation> violation = JsonSchema::Validate(arguments, command->Parameters))
		{
			callback(
				CommandError{AutomationErrorCode::InvalidParams, std::move(violation->Message), Json::object({{"path", violation->Path}})});
			return;
		}

		if (command->IsAsync())
		{
			CommandCompletion const completion(std::move(callback));
			try
			{
				command->AsyncHandler(arguments, completion);
			}
			catch (std::exception const& exception)
			{
				completion.Complete(MakeExceptionError(name, exception.what()));
			}
			catch (...)
			{
				completion.Complete(MakeExceptionError(name, "unknown exception"));
			}
			return;
		}

		// The callback runs outside the try block: an exception thrown by the caller's callback is not the command's
		// failure and must not trigger a second callback.
		std::optional<CommandResult> result;
		try
		{
			result.emplace(command->Handler(arguments));
		}
		catch (std::exception const& exception)
		{
			result.emplace(MakeExceptionError(name, exception.what()));
		}
		catch (...)
		{
			result.emplace(MakeExceptionError(name, "unknown exception"));
		}
		callback(std::move(*result));
	}
}
