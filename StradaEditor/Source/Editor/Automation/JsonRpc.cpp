#include "Editor/Automation/JsonRpc.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Strada::JsonRpc
{
	namespace
	{
		constexpr std::array<std::string_view, 4> RequestMembers = {"jsonrpc", "id", "method", "params"};

		Request ParseRequest(Json const& value)
		{
			Request request;
			if (!value.is_object())
			{
				request.Error = MakeCommandError(AutomationErrorCode::InvalidRequest, "a request must be a JSON object");
				return request;
			}

			// The ID is read first so that errors about the rest of the request can still be correlated by the client.
			if (auto const id = value.find("id"); id != value.end())
			{
				if (!id->is_string() && !id->is_number() && !id->is_null())
				{
					request.Error = MakeCommandError(AutomationErrorCode::InvalidRequest, "\"id\" must be a string, a number or null");
					return request;
				}
				request.Id = *id;
			}

			for (auto const& item : value.items())
			{
				if (std::find(RequestMembers.begin(), RequestMembers.end(), item.key()) == RequestMembers.end())
				{
					request.Error = MakeCommandError(AutomationErrorCode::InvalidRequest,
					                                 "unknown request member '{}' (expected jsonrpc, id, method and params)", item.key());
					return request;
				}
			}

			auto const version = value.find("jsonrpc");
			if (version == value.end() || *version != "2.0")
			{
				request.Error = MakeCommandError(AutomationErrorCode::InvalidRequest, "\"jsonrpc\" must be \"2.0\"");
				return request;
			}

			auto const method = value.find("method");
			if (method == value.end() || !method->is_string())
			{
				request.Error = MakeCommandError(AutomationErrorCode::InvalidRequest, "\"method\" must be a string");
				return request;
			}
			request.Method = method->get<std::string>();

			if (auto const params = value.find("params"); params != value.end())
			{
				if (!params->is_object() && !params->is_array())
				{
					request.Error = MakeCommandError(AutomationErrorCode::InvalidRequest, "\"params\" must be an object when present");
					return request;
				}
				request.Params = *params;
			}
			return request;
		}
	}

	CommandValue<Message> Parse(std::string_view text)
	{
		if (MeasureNestingDepth(text) > MaxNestingDepth)
		{
			return MakeCommandError(AutomationErrorCode::ParseError, "the message nests deeper than {} levels", MaxNestingDepth);
		}

		Result<Json> parsed = ParseJson(text);
		if (!parsed)
		{
			return MakeCommandError(AutomationErrorCode::ParseError, "{}", parsed.GetError());
		}
		Json const& document = parsed.GetValue();

		Message message;
		if (!document.is_array())
		{
			message.Requests.push_back(ParseRequest(document));
			return message;
		}

		if (document.empty())
		{
			return MakeCommandError(AutomationErrorCode::InvalidRequest, "an empty batch is not a valid request");
		}
		if (document.size() > MaxBatchSize)
		{
			return MakeCommandError(AutomationErrorCode::InvalidRequest, "a batch may contain at most {} requests", MaxBatchSize);
		}
		message.IsBatch = true;
		message.Requests.reserve(document.size());
		for (Json const& element : document)
		{
			message.Requests.push_back(ParseRequest(element));
		}
		return message;
	}

	Json MakeResult(Json const& id, Json result)
	{
		Json response = Json::object();
		response["jsonrpc"] = "2.0";
		response["id"] = id;
		response["result"] = std::move(result);
		return response;
	}

	Json MakeError(Json const& id, CommandError const& error)
	{
		Json details = Json::object();
		details["code"] = static_cast<int32_t>(error.Code);
		details["message"] = error.Message;
		if (!error.Data.is_null())
		{
			details["data"] = error.Data;
		}

		Json response = Json::object();
		response["jsonrpc"] = "2.0";
		response["id"] = id;
		response["error"] = std::move(details);
		return response;
	}

	Json MakeResponse(Request const& request, CommandResult const& result)
	{
		Json const id = request.Id.value_or(Json(nullptr));
		return result.IsOk() ? MakeResult(id, result.GetValue()) : MakeError(id, result.GetError());
	}

	std::string Serialize(Json const& message)
	{
		// Compact output never contains a raw newline (newlines inside strings are escaped), so lines frame messages.
		return message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
	}

	size_t MeasureNestingDepth(std::string_view text)
	{
		size_t depth = 0;
		size_t maximum = 0;
		bool inString = false;
		bool escaped = false;
		for (char const character : text)
		{
			if (inString)
			{
				if (escaped)
				{
					escaped = false;
				}
				else if (character == '\\')
				{
					escaped = true;
				}
				else if (character == '"')
				{
					inString = false;
				}
				continue;
			}

			switch (character)
			{
				case '"':
					inString = true;
					break;
				case '[':
				case '{':
					maximum = std::max(maximum, ++depth);
					break;
				case ']':
				case '}':
					depth = depth > 0 ? depth - 1 : 0;
					break;
				default:
					break;
			}
		}
		return maximum;
	}
}
