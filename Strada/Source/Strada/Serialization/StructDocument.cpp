#include "stpch.h"
#include "Strada/Serialization/StructDocument.h"

#include <algorithm>
#include <limits>
#include <string>

namespace Strada
{
	Result<Json const*> FindDocumentContent(Json const& document, StructDocumentFormat const& format, Json& migrated)
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
		if (static_cast<uint32_t>(header.GetValue()) == format.Version)
		{
			return &*content;
		}

		migrated = *content;
		for (uint32_t version = static_cast<uint32_t>(header.GetValue()); version < format.Version; version++)
		{
			if (version - 1 >= format.Migrations.size() || format.Migrations[version - 1] == nullptr)
			{
				return MakeError("no migration from version {} of {} documents", version, type);
			}
			if (Result<void> result = RunDocumentMigration(format.Migrations[version - 1], migrated, type, static_cast<int>(version));
			    !result)
			{
				return Error{result.GetError()};
			}
		}
		return &migrated;
	}

	Result<Json> ReadDocumentContent(Json const& document, StructDocumentFormat const& format)
	{
		Json migrated;
		Result<Json const*> const content = FindDocumentContent(document, format, migrated);
		if (!content)
		{
			return Error{content.GetError()};
		}
		if (content.GetValue() == &migrated)
		{
			return migrated;
		}
		return *content.GetValue();
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
