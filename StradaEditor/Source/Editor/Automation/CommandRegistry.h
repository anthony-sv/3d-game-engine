#pragma once

#include "Editor/Automation/CommandResult.h"

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// Receives the outcome of an automation command, exactly once, on the main thread.
	using CommandCallback = std::function<void(CommandResult result)>;

	// Completes an asynchronous command. Copies share one result slot: the first Complete wins and later calls are
	// ignored. When the last copy is destroyed without completing, the command fails with AutomationErrorCode::Cancelled,
	// so a client never waits forever for a command that was abandoned (for example because the editor closed).
	// Main thread only: copies are made, completed and destroyed on the main thread.
	class CommandCompletion
	{
	public:
		explicit CommandCompletion(CommandCallback callback);

		// Delivers the result to the caller of CommandRegistry::Execute. Returns false (ignoring the result) when the
		// command was already completed.
		bool Complete(CommandResult result) const;
		bool IsCompleted() const;

	private:
		struct State;
		std::shared_ptr<State> m_State;
	};

	// Synchronous handler. The parameters satisfy the command's schema (an object, never null).
	using CommandHandler = std::function<CommandResult(Json const& params)>;
	// Asynchronous handler: completes later on the main thread through the completion (for example after N frames).
	using AsyncCommandHandler = std::function<void(Json const& params, CommandCompletion completion)>;

	struct CommandDefinition
	{
		// "<domain>.<action>" made of lowercase letters, digits and hyphens: "entity.create", "play.advance-frames".
		std::string Name;
		// What the command does and returns, for humans and AI agents (MCP bridges turn it into the tool description).
		std::string Description;
		// JSON schema of the parameters object: root type "object", using the subset JsonSchema supports. Null means
		// "no parameters" ({"type": "object", "additionalProperties": false}).
		Json Parameters;
		// The command does not change the scene, the selection, files or any other editor state.
		bool ReadOnly = false;
		// Exactly one of the two handlers is set.
		CommandHandler Handler;
		AsyncCommandHandler AsyncHandler;

		bool IsAsync() const { return static_cast<bool>(AsyncHandler); }
	};

	// The named automation commands of the editor. Parameters are validated against the command's schema before its
	// handler runs, and handler failures (including exceptions escaping a handler) become structured errors, so no
	// request can crash the editor. Main thread only.
	class CommandRegistry
	{
	public:
		// Fails for an invalid name or schema, an empty description, zero or two handlers, and duplicate names.
		[[nodiscard]] Result<void> Register(CommandDefinition definition);

		bool Contains(std::string_view name) const { return Find(name) != nullptr; }
		// Null when no command has the name.
		CommandDefinition const* Find(std::string_view name) const;
		size_t GetCommandCount() const { return m_Commands.size(); }
		// Sorted by name.
		std::vector<CommandDefinition const*> GetCommands() const;
		// [{ "name", "description", "params", "readOnly", "async" }, ...] sorted by name (the editor.commands result).
		Json Describe() const;

		// Runs a command. The callback receives the result exactly once: before Execute returns for synchronous
		// commands, possibly later for asynchronous ones. Unknown names fail with MethodNotFound; parameters that are not
		// an object (null counts as {}) or violate the schema fail with InvalidParams, with the offending location in
		// the error data ({ "path": "..." }).
		void Execute(std::string_view name, Json const& params, CommandCallback callback) const;

		static bool IsValidCommandName(std::string_view name);

	private:
		std::map<std::string, CommandDefinition, std::less<>> m_Commands;
	};
}
