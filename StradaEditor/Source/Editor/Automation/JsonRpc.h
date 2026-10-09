#pragma once

#include "Editor/Automation/CommandResult.h"

#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// JSON-RPC 2.0 messages as used by the automation protocol: one compact JSON document per line, requests by name
// ("params" is an object), batches supported. See Docs/Automation.md.
namespace Strada::JsonRpc
{
	// Documents nesting deeper than this are rejected before parsing: JSON values are copied and serialized
	// recursively, so unbounded nesting from a client could exhaust the stack.
	inline constexpr size_t MaxNestingDepth = 64;
	// Requests per batch.
	inline constexpr size_t MaxBatchSize = 64;

	struct Request
	{
		// The request ID (a string, a number or null); empty for notifications, which get no response.
		std::optional<Json> Id;
		std::string Method;
		// As sent: an object, an array or null (absent). Commands take named parameters only.
		Json Params;
		// Set when this is not a valid request object; it is answered with this error (and Id, or null).
		std::optional<CommandError> Error;

		bool IsNotification() const { return !Id.has_value(); }
	};

	struct Message
	{
		bool IsBatch = false;
		// One entry for a single request; the batch elements in order otherwise.
		std::vector<Request> Requests;
	};

	// Parses one message (a line without its terminator). Fails, with the error to answer (with a null ID), when the
	// text is not JSON, nests too deeply, or is neither a request object nor a non-empty batch of at most MaxBatchSize.
	[[nodiscard]] CommandValue<Message> Parse(std::string_view text);

	Json MakeResult(Json const& id, Json result);
	// { "jsonrpc": "2.0", "id": id, "error": { "code", "message", "data"? } }.
	Json MakeError(Json const& id, CommandError const& error);
	// The response to a request: its ID (null when the request had none) with the result or the error.
	Json MakeResponse(Request const& request, CommandResult const& result);

	// One line on the wire: compact JSON plus "\n". Invalid UTF-8 in strings is replaced instead of failing.
	std::string Serialize(Json const& message);

	// The deepest array/object nesting of a JSON text, skipping string contents. Malformed text yields a best-effort
	// value (the parser reports the actual error).
	size_t MeasureNestingDepth(std::string_view text);
}
