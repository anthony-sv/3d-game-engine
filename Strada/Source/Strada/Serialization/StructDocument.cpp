#include "stpch.h"
#include "Strada/Serialization/StructDocument.h"

#include <algorithm>
#include <limits>
#include <string>

namespace Strada
{
	Result<Json> ReadDocumentContent(Json const& document, StructDocumentFormat const& format)
	{
		uint32_t const newest = std::min<uint32_t>(format.Version, static_cast<uint32_t>(std::numeric_limits<int>::max()));
		Result<int> header = ReadFileHeader(document, format.Type, static_cast<int>(newest));
		if (!header)
		{
			return Error{header.GetError()};
		}
		std::string const type(format.Type);
		auto const content = document.find(type);
		if (content == document.end())
		{
			return MakeError("the document has no \"{}\" object", type);
		}
		if (!content->is_object())
		{
			return MakeError("\"{}\" must be a JSON object", type);
		}

		Json migrated = *content;
		for (uint32_t version = static_cast<uint32_t>(header.GetValue()); version < format.Version; version++)
		{
			if (version - 1 >= format.Migrations.size() || format.Migrations[version - 1] == nullptr)
			{
				return MakeError("no migration from version {} of {} documents", version, type);
			}
			Result<void> result;
			try
			{
				result = format.Migrations[version - 1](migrated);
			}
			catch (Json::exception const& exception)
			{
				result = MakeError("unexpected content ({})", exception.what());
			}
			if (!result)
			{
				return MakeError("{} version {} cannot be read: {}", type, version, result.GetError());
			}
			if (!migrated.is_object())
			{
				return MakeError("{} version {} cannot be read: its migration left no object", type, version);
			}
		}
		return migrated;
	}

	Json MakeStructDocument(StructDocumentFormat const& format, Json content)
	{
		uint32_t const version = std::min<uint32_t>(format.Version, static_cast<uint32_t>(std::numeric_limits<int>::max()));
		Json document = Json::object();
		document["Strada"] = MakeFileHeader(format.Type, static_cast<int>(version));
		document[std::string(format.Type)] = std::move(content);
		return document;
	}
}
