#pragma once

#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

// Versioned documents of one reflected struct: the Strada file header, then the struct's fields as the content object named
// after the document's type. Data asset files are such documents (Asset/DataAsset.h), and so are files a game writes
// outside its assets (saves, the player's settings, binding profiles), which share the header, the migrations and the
// validation without being assets. Docs/Architecture.md §6.4 and §7.3.
namespace Strada
{
	// Upgrades the content object of a versioned document (the member named after its type, without the header) from one
	// format version to the next, in place. It may assume only that the content is a JSON object: what it reads it checks
	// first, and it refuses what it cannot convert. The reader also refuses the document when a migration throws (nlohmann
	// throws on a value of the wrong kind) or leaves something other than an object, so a missed check refuses instead of
	// crashing. Pure; any thread.
	using DocumentMigration = Result<void> (*)(Json& content);

	// The format of a versioned struct document:
	//   { "Strada": { "Version": <Version>, "Type": "<Type>" }, "<Type>": { <the struct's fields> } }
	struct StructDocumentFormat
	{
		// The header's Type and the key of the content object.
		std::string_view Type;
		// The version writers write and the newest readers accept, at least 1.
		uint32_t Version = 1;
		// One migration per older version (Version - 1 of them): Migrations[i] upgrades version i + 1 to version i + 2. The
		// storage must outlive the format's users (a static array).
		std::span<DocumentMigration const> Migrations;
	};

	// Checks the header (the format's type, a version from 1 to format.Version), takes the content object and runs the
	// migrations from the document's version to the current one. Fails on a header of another type or version, a missing
	// or non-object content, a missing migration, and a migration that fails, throws or leaves no object. Any thread.
	[[nodiscard]] Result<Json> ReadDocumentContent(Json const& document, StructDocumentFormat const& format);
	// The document of a content object in the current version of the format. Any thread.
	Json MakeStructDocument(StructDocumentFormat const& format, Json content);

	// Reads a document of the format into a value-initialized T: ReadDocumentContent, then the fields (missing fields keep
	// their defaults, unknown ones follow the context's policy, values outside their ranges or longer than their MaxCount
	// refuse the document), then validate (null when the field table says all). Any thread, when the context's resolver is.
	template<ReflectedStruct T>
	[[nodiscard]] Result<T> ReadStructDocument(Json const& document, StructDocumentFormat const& format, Result<void> (*validate)(T const&),
	                                           DeserializationContext const& context)
	{
		Result<Json> content = ReadDocumentContent(document, format);
		if (!content)
		{
			return Error{content.GetError()};
		}
		T data{};
		if (Result<void> read = DeserializeFields<StructTraits<T>>(content.GetValue(), data, context, "document"); !read)
		{
			return Error{read.GetError()};
		}
		if (validate != nullptr)
		{
			if (Result<void> valid = validate(data); !valid)
			{
				return MakeError("{}: {}", format.Type, valid.GetError());
			}
		}
		return data;
	}

	// The document of the data in the current version of the format: every field, in field table order. Any thread.
	template<ReflectedStruct T>
	Json WriteStructDocument(T const& data, StructDocumentFormat const& format)
	{
		return MakeStructDocument(format, SerializeFields<StructTraits<T>>(data));
	}
}
