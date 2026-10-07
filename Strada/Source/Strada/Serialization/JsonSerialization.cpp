#include "stpch.h"
#include "Strada/Serialization/JsonSerialization.h"

namespace Strada
{
	Json MakeFileHeader(std::string_view type, int version)
	{
		Json header = Json::object();
		header["Version"] = version;
		header["Type"] = std::string(type);
		return header;
	}

	Result<int> ReadFileHeader(Json const& document, std::string_view expectedType, int maximumVersion)
	{
		if (!document.is_object())
		{
			return Error{"document must be a JSON object"};
		}
		auto const header = document.find("Strada");
		if (header == document.end() || !header->is_object())
		{
			return Error{"missing \"Strada\" file header"};
		}

		auto const type = header->find("Type");
		if (type == header->end() || !type->is_string())
		{
			return Error{"file header has no \"Type\""};
		}
		if (type->get_ref<std::string const&>() != expectedType)
		{
			return MakeError("expected a {} file but found '{}'", expectedType, type->get_ref<std::string const&>());
		}

		auto const version = header->find("Version");
		if (version == header->end() || !version->is_number_integer())
		{
			return Error{"file header has no integer \"Version\""};
		}
		int const value = version->get<int>();
		if (value < 1)
		{
			return MakeError("invalid file version {}", value);
		}
		if (value > maximumVersion)
		{
			return MakeError("file version {} is newer than the supported version {}", value, maximumVersion);
		}
		return value;
	}

	Result<Json> ParseJson(std::string_view text)
	{
		try
		{
			return Json::parse(text.begin(), text.end());
		}
		catch (Json::parse_error const& exception)
		{
			return MakeError("invalid JSON: {}", exception.what());
		}
	}

	std::string DumpJson(Json const& json)
	{
		// Invalid UTF-8 in strings is replaced instead of throwing.
		return json.dump(1, '\t', false, Json::error_handler_t::replace) + "\n";
	}
}
