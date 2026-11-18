#include "stpch.h"
#include "Strada/Serialization/VersionedDocument.h"

#include <cstddef>
#include <utility>

namespace Strada
{
	Result<void> RunDocumentMigration(DocumentMigration migration, Json& value, std::string_view type, int version)
	{
		Result<void> result;
		try
		{
			result = migration(value);
		}
		catch (Json::exception const& exception)
		{
			result = MakeError("unexpected content ({})", exception.what());
		}
		if (!result)
		{
			return MakeError("{} version {} cannot be read: {}", type, version, result.GetError());
		}
		if (!value.is_object())
		{
			return MakeError("{} version {} cannot be read: its migration left no object", type, version);
		}
		return {};
	}

	Result<int> MigrateDocument(Json& document, VersionedDocumentFormat const& format)
	{
		int const current = format.GetVersion();
		Result<int> header = ReadFileHeader(document, format.Type, current);
		if (!header)
		{
			return Error{header.GetError()};
		}
		int const fileVersion = header.GetValue();
		for (int version = fileVersion; version < current; version++)
		{
			DocumentMigration const migration = format.Versions[static_cast<size_t>(version - 1)].Migration;
			if (migration == nullptr)
			{
				continue;
			}
			if (Result<void> migrated = RunDocumentMigration(migration, document, format.Type, version); !migrated)
			{
				return Error{migrated.GetError()};
			}
		}
		if (fileVersion < current)
		{
			document["Strada"] = MakeFileHeader(format.Type, current);
		}
		return fileVersion;
	}

	MigratedDocument::MigratedDocument(Json const& document, std::optional<Json> migrated, int fileVersion)
		: m_Document(&document),
		  m_Migrated(std::move(migrated)),
		  m_FileVersion(fileVersion)
	{
	}

	Result<MigratedDocument> ReadMigratedDocument(Json const& document, VersionedDocumentFormat const& format)
	{
		Result<int> header = ReadFileHeader(document, format.Type, format.GetVersion());
		if (!header)
		{
			return Error{header.GetError()};
		}
		int const fileVersion = header.GetValue();
		bool needsMigration = false;
		for (size_t index = static_cast<size_t>(fileVersion - 1); index < format.Versions.size(); index++)
		{
			needsMigration = needsMigration || format.Versions[index].Migration != nullptr;
		}
		if (!needsMigration)
		{
			return MigratedDocument(document, std::nullopt, fileVersion);
		}

		Json copy = document;
		if (Result<int> migrated = MigrateDocument(copy, format); !migrated)
		{
			return Error{migrated.GetError()};
		}
		return MigratedDocument(document, std::move(copy), fileVersion);
	}
}
