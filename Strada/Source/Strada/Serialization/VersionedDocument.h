#pragma once

#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <optional>
#include <span>
#include <string_view>

// Versioned JSON documents and their migrations: how a reader of a format's current version reads the files of the versions
// before it. Two kinds of document share the mechanism. Struct documents (data assets and the files a game writes,
// Serialization/StructDocument.h) migrate the content object named after their type. Whole documents (scenes and prefabs,
// VersionedDocumentFormat) migrate the document, header included. Docs/Architecture.md §7.3, "Versions and migrations".
namespace Strada
{
	// Upgrades a document of a format from one version to the next, in place: a struct document's content object, or a whole
	// document (whose header the reader rewrites afterwards). It may assume only that it is given a JSON object: what it reads
	// it checks first, and it refuses what it cannot convert. Readers also refuse the document when a migration throws
	// (nlohmann throws on a value of the wrong kind) or leaves something other than an object, so a missed check refuses
	// instead of crashing. Pure; any thread.
	using DocumentMigration = Result<void> (*)(Json& value);

	// Runs the migration of a document of version `version` of the format named `type` as every reader does: a migration that
	// fails, throws or leaves no object refuses the document ("<type> version <version> cannot be read: <reason>"). Any
	// thread.
	[[nodiscard]] Result<void> RunDocumentMigration(DocumentMigration migration, Json& value, std::string_view type, int version);

	// A version of a whole-document format after its first.
	struct DocumentVersion
	{
		// What the version changed, in a few words: the format's history, kept beside its readers.
		std::string_view Change;
		// Upgrades a document of the version before to this one. Null when the version only added what readers of the version
		// before refuse, so that every document of that version is also one of this version: such documents are read as
		// they are, never copied.
		DocumentMigration Migration = nullptr;
	};

	// A format whose documents are its file header ({ "Strada": { "Version", "Type" } }) beside members of its own: scenes
	// and prefabs.
	struct VersionedDocumentFormat
	{
		// The header's Type.
		std::string_view Type;
		// Versions[i] is version i + 2, so the current version, which writers write and the newest readers accept, is
		// Versions.size() + 1. The storage must outlive the format's users (a static array).
		std::span<DocumentVersion const> Versions;

		[[nodiscard]] constexpr int GetVersion() const { return static_cast<int>(Versions.size()) + 1; }
	};

	// Checks the header (the format's type, a version from 1 to the current one) and brings a document the caller owns to the
	// current version in place: the migrations of the versions after the document's own run in order, and the header then
	// names the current version. Returns the version the document had. Fails as ReadFileHeader does and when a migration
	// refuses (RunDocumentMigration); the document is then left part-way, for the caller to discard. Any thread.
	[[nodiscard]] Result<int> MigrateDocument(Json& document, VersionedDocumentFormat const& format);

	// A document of a format at its current version, read without changing the caller's document (ReadMigratedDocument).
	class MigratedDocument
	{
	public:
		// The current version's document: the caller's document itself when no version after its own has a migration (its
		// header still names its own version), else the migrated copy, which this object holds. The reference is valid
		// while this object and the caller's document live.
		Json const& Get() const { return m_Migrated ? *m_Migrated : *m_Document; }
		// The version the caller's document has.
		int GetFileVersion() const { return m_FileVersion; }
		// Whether Get() is a migrated copy.
		bool IsCopy() const { return m_Migrated.has_value(); }

	private:
		MigratedDocument(Json const& document, std::optional<Json> migrated, int fileVersion);

		friend Result<MigratedDocument> ReadMigratedDocument(Json const& document, VersionedDocumentFormat const& format);

		Json const* m_Document = nullptr;
		std::optional<Json> m_Migrated;
		int m_FileVersion = 0;
	};

	// MigrateDocument for a document the caller keeps as it is: the document is copied, and the copy migrated, only when a
	// version after its own has a migration, so files of versions that only added what older readers refuse are read in
	// place, whatever their size. Any thread.
	[[nodiscard]] Result<MigratedDocument> ReadMigratedDocument(Json const& document, VersionedDocumentFormat const& format);
}
