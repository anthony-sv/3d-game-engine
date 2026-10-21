#include "stpch.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <vector>

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
		// Compared at full width: narrowing to int first would turn huge versions into small ones (4294967297 into 1).
		if (version->is_number_unsigned() ? version->get<uint64_t>() == 0 : version->get<int64_t>() < 1)
		{
			return MakeError("invalid file version {}", version->get<int64_t>());
		}
		uint64_t const value = version->get<uint64_t>();
		if (value > static_cast<uint64_t>(maximumVersion))
		{
			return MakeError("file version {} is newer than the supported version {}", value, maximumVersion);
		}
		return static_cast<int>(value);
	}

	namespace
	{
		// Refuses documents that parse but are costly to use: nlohmann's copies, comparisons and dumps recurse once per
		// nesting level, and ordered_json inserts a member with a linear search for its key. A linear scan that only tracks
		// strings and brackets; syntax errors are left to the parser.
		Result<void> CheckJsonStructure(std::string_view text)
		{
			// Whether each open bracket is an object, and the members of each open object.
			std::vector<bool> isObject;
			std::vector<size_t> members;
			bool inString = false;
			for (size_t i = 0; i < text.size(); i++)
			{
				char const character = text[i];
				if (inString)
				{
					if (character == '\\')
					{
						i++;
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
					case '{':
					case '[':
						if (isObject.size() == MaxJsonDepth)
						{
							return MakeError("invalid JSON: values are nested deeper than {} levels", MaxJsonDepth);
						}
						isObject.push_back(character == '{');
						members.push_back(1);
						break;
					case '}':
					case ']':
						if (!isObject.empty())
						{
							isObject.pop_back();
							members.pop_back();
						}
						break;
					case ',':
						if (!isObject.empty() && isObject.back() && ++members.back() > MaxJsonObjectMembers)
						{
							return MakeError("invalid JSON: an object has more than {} members", MaxJsonObjectMembers);
						}
						break;
					default:
						break;
				}
			}
			return {};
		}
	}

	Result<Json> ParseJson(std::string_view text)
	{
		if (Result<void> structure = CheckJsonStructure(text); !structure)
		{
			return Error{structure.GetError()};
		}
		try
		{
			return Json::parse(text.begin(), text.end());
		}
		catch (Json::exception const& exception)
		{
			// Syntax errors, and numbers out of the range of double (out_of_range).
			return MakeError("invalid JSON: {}", exception.what());
		}
	}

	std::string DumpJson(Json const& json)
	{
		// Invalid UTF-8 in strings is replaced instead of throwing.
		return json.dump(1, '\t', false, Json::error_handler_t::replace) + "\n";
	}
}
