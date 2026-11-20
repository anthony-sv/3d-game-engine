#pragma once

#include "Strada/Core/Result.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// A game's save slots (Docs/Architecture.md §13.1, "Saves", and §7.3): named files of named values that the game writes
// while it runs, kept below the folder the GameData service names (<game data>/Saves/<name>.ssave), one per slot. A slot
// holds values of the script field types (Script/ScriptField.h: numbers, text, vectors, quaternions, colors, entities,
// prefabs, assets, and lists of any of them of up to MaxScriptFieldListElements elements), by name. It is a versioned
// struct document (Serialization/StructDocument.h), not an asset: no handle, no editor, no commands.
//
// A reader sees a slot in one of five states (SaveSlotStatus). A slot of a newer version is never replaced or deleted by
// this engine: it reads as empty and writes to it are refused, so that a player who runs an older build of a game keeps
// the newer build's save. A damaged slot reads as empty and stays on disk until the game explicitly writes or deletes
// it. Writes are durable (FileSystem::WriteTextFileDurably): after one returns, the slot on disk is the new one, whole,
// whatever happens next; one that fails, or is interrupted by a power loss, leaves the old one whole.
//
// The scripting boundary (Strada-ScriptCore's SaveSlot.cs; bindings in ScriptBindingsSaves.cpp and InternalCalls.cs):
//   public sealed class SaveSlot
//     static SaveSlot Open(string name)          reads the slot (impure: it reads the disk); never null; an invalid name
//                                                throws ArgumentException; without a game data folder it logs and answers
//                                                an empty slot of status Unavailable
//     static string[] GetNames()                 the slots on disk, sorted (impure: it reads the disk)
//     string Name { get; }                       the slot's name as SaveSlotStore::MakeFileStem gives it (lower case)
//     SaveSlotStatus Status { get; }             as read, or as the last Save or Delete left it
//     bool Has(string name)                      [PureNode]
//     T Get<T>(string name, T fallback)          [PureNode] the value, when the slot holds one of T's field type
//                                                (ScriptFieldCodec.GetMemberFieldType: T[] and List<T> are lists, enums
//                                                their integers, typed assets handles); else the fallback, warned about
//                                                once per slot object and name when the value is of another type
//     void Set<T>(string name, T value)          kept in the object until Save; refuses (ArgumentException) a name
//                                                ValidateSavedValueName refuses, a T without a field type, a list longer
//                                                than MaxScriptFieldListElements, a number that is not finite, a
//                                                quaternion ValidateSavedValue refuses, and more than MaxSaveSlotValues
//                                                values (it measures no size: a slot past MaxSaveSlotBytes is refused
//                                                whole by Save)
//     bool Remove(string name)
//     bool Save()                                writes every value durably and returns once the slot is on disk (its
//                                                answer is the write's, so the frame that saves waits for the device: a
//                                                game saves at a results screen or a menu, never while the player
//                                                rides); false (logged) when the slot is Newer or Unavailable, or the
//                                                store refuses the write; Status is Present after one
//     bool Delete()                              removes the file and the values; false (logged) when Newer or Unavailable
//   public enum SaveSlotStatus { Missing, Present, Damaged, Newer, Unavailable } (the values of the C++ enum)
// Get<T> and Set<T> are unconstrained generic methods, so graphs get one node each, typed by their links or a TypeArguments
// setting (members of this shape, on an object held in a graph's variable and typed by links alone, are run by
// ScriptCoreTests' GenericMethodTests.InstanceMethodsOfPlainClassesInferTheirTypeFromLinks). The object holds the values in the managed
// runtime (stored forms, ScriptFieldCodec), so Get and Set cross no boundary; Open and Save cross it once with the values' JSON (the script
// field map form, §7.3). Two objects opened with one name are independent copies, and the last Save wins. The bindings, all main thread:
//   char const* SaveSlot_Read(char const* name, int32_t length, int32_t* status, int32_t* textLength)
//     the slot's document (WriteReadSaveSlotText) in engine-owned storage valid until the next binding of
//     ScriptBindingsSaves.cpp (the caller copies it at once, then releases it); *status a SaveSlotStatus, or -1 for a name
//     MakeFileStem refuses, the text then saying why (the managed side throws with it)
//   void SaveSlot_ReleaseText()                           frees that storage (a slot's text may be MiBs)
//   uint8_t SaveSlot_Write(char const* name, int32_t length, char const* values, int32_t valuesLength)
//   uint8_t SaveSlot_Delete(char const* name, int32_t length)
//   char const* SaveSlot_GetNames(int32_t* textLength)     the names joined by '\n'
// What the store refuses or finds (an unreadable slot, a refused write, no game data folder) is logged through the script
// logger as "SaveSlot.<Member>: ...", never thrown. No member exposes a path.
namespace Strada
{
	// What a reader finds for a slot.
	enum class SaveSlotStatus : uint8_t
	{
		// No file: an empty slot, which a write creates.
		Missing = 0,
		// Read: its values.
		Present,
		// A file that cannot be read (not JSON, another type, a value of the wrong kind, larger than MaxSaveSlotBytes
		// without a later version's header): read as empty; only an explicit write or delete replaces it.
		Damaged,
		// A file of a version newer than this engine's: read as empty; never written over or deleted.
		Newer,
		// No game runs (the GameData service is empty): nothing is read or written.
		Unavailable
	};

	template<>
	struct EnumTraits<SaveSlotStatus>
	{
		static constexpr std::array<std::pair<SaveSlotStatus, std::string_view>, 5> Values = {{
			{SaveSlotStatus::Missing, "Missing"},
			{SaveSlotStatus::Present, "Present"},
			{SaveSlotStatus::Damaged, "Damaged"},
			{SaveSlotStatus::Newer, "Newer"},
			{SaveSlotStatus::Unavailable, "Unavailable"},
		}};
	};

	// The longest slot name, in bytes.
	inline constexpr size_t MaxSaveSlotNameLength = 64;
	// The longest name of a value (of a slot, or of the player's settings), in bytes of UTF-8.
	inline constexpr size_t MaxSavedValueNameLength = 128;
	// The most values a slot holds: what ParseJson reads in one object.
	inline constexpr size_t MaxSaveSlotValues = MaxJsonObjectMembers;
	// The largest slot file, in bytes: written larger is refused, and of a larger file only the first MaxSaveSlotHeaderBytes
	// are read, for its header (a later version's, or Damaged). It is 16 times the budget's 1 MB slot (a lap's ghost at 10
	// samples a second is far below either).
	inline constexpr uint64_t MaxSaveSlotBytes = 16ull * 1024 * 1024;
	// How much of a file larger than MaxSaveSlotBytes is read to find its header.
	inline constexpr uint64_t MaxSaveSlotHeaderBytes = 64ull * 1024;
	// The shortest quaternion a saved value holds: what JsonTraits<glm::quat> reads.
	inline constexpr float MinSavedQuaternionLength = 1e-6f;
	// The folder below the game data folder, and the file's extension.
	inline constexpr std::string_view SaveSlotDirectoryName = "Saves";
	inline constexpr std::string_view SaveSlotExtension = ".ssave";

	// A slot's content: its values by name, written in name order.
	struct SaveSlotData
	{
		ScriptFieldMap Values;

		bool operator==(SaveSlotData const& other) const = default;
	};

	template<>
	struct StructTraits<SaveSlotData>
	{
		static constexpr std::string_view Name = "SaveSlot";
		static constexpr auto Fields =
			std::make_tuple(Field("Values", &SaveSlotData::Values)
		                        .Doc("The slot's values by name, each with its script field type ({ \"Type\", \"Value\" })."));
	};

	inline constexpr std::array<DocumentMigration, 0> SaveSlotMigrations = {};

	// { "Strada": { "Version": 1, "Type": "SaveSlot" }, "SaveSlot": { "Values": { "<Name>": { "Type", "Value" } } } }
	inline constexpr StructDocumentFormat SaveSlotFormat = {
		.Type = "SaveSlot",
		.Version = 1,
		.Migrations = SaveSlotMigrations,
	};

	// A value's name (in a slot or in the player's settings): 1 to MaxSavedValueNameLength bytes of valid UTF-8 without
	// control characters. Names are compared exactly (letter case included).
	[[nodiscard]] Result<void> ValidateSavedValueName(std::string_view name);
	// A value a slot or the player's settings hold: a script field type other than None, a list of at most
	// MaxScriptFieldListElements elements, every number finite (JSON holds no other) and every quaternion of a length of
	// at least MinSavedQuaternionLength (the JSON reader refuses shorter ones; a quaternion reads back normalized).
	[[nodiscard]] Result<void> ValidateSavedValue(ScriptFieldValue const& value);
	// A slot's content: at most MaxSaveSlotValues values, each name and value as the two checks above say.
	[[nodiscard]] Result<void> ValidateSaveSlot(SaveSlotData const& slot);

	// What reading a slot found.
	struct SaveSlotRead
	{
		SaveSlotStatus Status = SaveSlotStatus::Missing;
		// The values: empty unless Present.
		SaveSlotData Data;
		// The file's version, when its header names the type (Present: 1; Newer: above it); 0 otherwise.
		int Version = 0;
		// Why a Damaged or Newer slot reads as empty; empty otherwise.
		std::string Problem;
		// Unknown fields skipped while reading (a compatible field of a later engine), for the log.
		std::vector<std::string> Warnings;
	};

	// The version a versioned document's header names when its Type is `type`, read from the header alone, so that a
	// document of a later version is recognised whatever its content holds: the text's first top-level member when it is
	// "Strada" (writers write it first; it is delimited by walking strings and brackets, so the text may end anywhere after
	// it, as the first MaxSaveSlotHeaderBytes of a larger file do), else the last "Strada" member that ScanJsonTopLevel
	// (Serialization/JsonScanner.h) delimits in the whole text (the parser keeps the last of repeated keys); only that
	// member is parsed. A version above INT_MAX reads as INT_MAX, one below 1 as 0. Nothing when neither finds a header of
	// that type with an integer version. Save slots and the player's settings (Script/PlayerSettings.h) read theirs so. Any
	// thread.
	[[nodiscard]] std::optional<int> ReadDocumentHeaderVersion(std::string_view text, std::string_view type);
	// ReadDocumentHeaderVersion of a slot document (SaveSlotFormat.Type).
	[[nodiscard]] std::optional<int> ReadSaveSlotVersion(std::string_view text);
	// Reads a slot document's text: Present with its values; Newer when ReadSaveSlotVersion names a version above
	// SaveSlotFormat.Version (the header is read before anything else, size included, so a newer slot is never called
	// damaged); Damaged when the text is larger than MaxSaveSlotBytes, is not JSON, its header is not a SaveSlot's of a
	// version from 1, or its content does not read or ValidateSaveSlot refuses it. Unknown fields are skipped with warnings.
	// Any thread.
	[[nodiscard]] SaveSlotRead ReadSaveSlotText(std::string_view text);
	// The document of a slot in the current version. Any thread.
	[[nodiscard]] Json WriteSaveSlot(SaveSlotData const& slot);
	// The text of a slot's document: exactly what DumpJson writes of WriteSaveSlot's document (tabs, a trailing newline),
	// written without building it (JsonTextWriter). Refuses a slot ValidateSaveSlot refuses and text longer than
	// MaxSaveSlotBytes, stopping at the value that passes it. Any thread.
	[[nodiscard]] Result<std::string> WriteSaveSlotText(SaveSlotData const& slot);
	// The text WriteSaveSlotText writes of a slot a reader gave (ReadSaveSlotText, SaveSlotStore::Read), for the scripting
	// boundary (SaveSlot_Read): neither validated (the reader did) nor bounded, since MaxSaveSlotBytes bounds the file, and
	// a file that reads may give a longer text (written compact, or its numbers short), which scripts read whole. Any
	// thread.
	[[nodiscard]] std::string WriteReadSaveSlotText(SaveSlotData const& slot);

	// The slots of the running game in <game data>/Saves. Stateless, so any thread may call it: two writes of one slot at
	// once leave one of them whole. Each call reads or writes the disk; the game's scripts reach it through the SaveSlot
	// class.
	class SaveSlotStore
	{
	public:
		// The file name (without extension) of a slot name, and the name scripts see: the name in lower case, so that names
		// that differ only in letter case are one slot on every platform. A name is 1 to MaxSaveSlotNameLength ASCII
		// letters, digits, spaces, '-' and '_', without a space at either end, and not a name Windows reserves for a
		// device (FileSystem::MakePortableFileName gives it back unchanged): anything else is refused, never rewritten.
		[[nodiscard]] static Result<std::string> MakeFileStem(std::string_view name);
		// <game data>/Saves; nothing when no game runs. Not created.
		[[nodiscard]] static std::optional<std::filesystem::path> GetDirectory();

		// Reads the slot: Unavailable without a game data folder, Missing without a file, else ReadSaveSlotText of the file.
		// A file larger than MaxSaveSlotBytes is not read past its first MaxSaveSlotHeaderBytes: Newer when its header
		// names a later version, else Damaged. A file that cannot be read, or a folder in the slot's place, is Damaged too
		// (Write and Delete keep it, since it may be a newer one). Fails only for a name MakeFileStem refuses.
		// Opens the profiler zone Saves.ReadSlot.
		[[nodiscard]] static Result<SaveSlotRead> Read(std::string_view name);
		// Writes the slot durably (FileSystem::WriteTextFileDurably), creating Saves/. Refuses an invalid name, no game data
		// folder, a slot WriteSaveSlotText refuses, a file on disk of a newer version (its header, read as Read reads it,
		// without reading past it when it comes first), and a file on disk that cannot be read; a damaged file is replaced.
		// A write that fails leaves the file on disk as it was. Opens the profiler zone Saves.WriteSlot.
		[[nodiscard]] static Result<void> Write(std::string_view name, SaveSlotData const& slot);
		// Removes the slot's file; a slot without one succeeds. Refuses an invalid name, no game data folder, a newer slot
		// and one that cannot be read; a damaged slot is removed.
		[[nodiscard]] static Result<void> Delete(std::string_view name);
		// The names of the slots in Saves/ (files <name>.ssave whose name MakeFileStem gives back unchanged; temporary
		// files of interrupted writes and other files are left out), sorted; empty without a game data folder.
		[[nodiscard]] static std::vector<std::string> GetNames();
	};
}
