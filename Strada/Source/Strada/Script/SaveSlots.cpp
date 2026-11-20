#include "stpch.h"
#include "Strada/Script/SaveSlots.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Utf8.h"
#include "Strada/Math/Math.h"
#include "Strada/Serialization/JsonScanner.h"
#include "Strada/Serialization/JsonTextWriter.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Strada
{
	// A slot's values are script fields: a change to the script field form takes the next version of slots (and of the
	// player's settings, scenes and prefabs; Docs/Architecture.md §7.3, "Adding a version", step 3), so that an older engine
	// reads a later game's save as Newer, never as Damaged and replaceable. These pin the form version 1 holds: raise
	// SaveSlotFormat.Version, with its migration and a fixture of version 1 kept, and these checks with it.
	static_assert(SaveSlotFormat.Version == 1 && LastScriptFieldElementType == ScriptFieldType::Asset &&
	                  static_cast<uint8_t>(LastScriptFieldElementType) == 16 && MaxScriptFieldListElements == 4096,
	              "the script field form changed: save slots need their next version");
	static_assert(SaveSlotMigrations.size() + 1 == SaveSlotFormat.Version, "one migration per older version");
	static_assert(MaxSaveSlotHeaderBytes < MaxSaveSlotBytes);

	namespace
	{
		constexpr std::string_view HeaderKey = "\"Strada\"";

		bool IsJsonWhitespace(char c)
		{
			return c == ' ' || c == '\t' || c == '\n' || c == '\r';
		}

		size_t SkipWhitespace(std::string_view text, size_t offset)
		{
			while (offset < text.size() && IsJsonWhitespace(text[offset]))
			{
				offset++;
			}
			return offset;
		}

		// One past the closing quote of the string whose opening quote is at offset; npos when the text ends first.
		size_t FindStringEnd(std::string_view text, size_t offset)
		{
			for (size_t index = offset + 1; index < text.size(); index++)
			{
				if (text[index] == '\\')
				{
					index++;
				}
				else if (text[index] == '"')
				{
					return index + 1;
				}
			}
			return std::string_view::npos;
		}

		// One past the bracket that closes the object or array opening at offset, walking strings and brackets; npos when
		// the text ends first, a bracket closes another kind, or more than MaxJsonScanDepth are open.
		size_t FindContainerEnd(std::string_view text, size_t offset)
		{
			std::vector<char> open;
			for (size_t index = offset; index < text.size(); index++)
			{
				char const c = text[index];
				if (c == '"')
				{
					size_t const end = FindStringEnd(text, index);
					if (end == std::string_view::npos)
					{
						return std::string_view::npos;
					}
					index = end - 1;
				}
				else if (c == '{' || c == '[')
				{
					if (open.size() == MaxJsonScanDepth)
					{
						return std::string_view::npos;
					}
					open.push_back(c == '{' ? '}' : ']');
				}
				else if (c == '}' || c == ']')
				{
					if (open.empty() || open.back() != c)
					{
						return std::string_view::npos;
					}
					open.pop_back();
					if (open.empty())
					{
						return index + 1;
					}
				}
			}
			return std::string_view::npos;
		}

		// The value of the text's first top-level member when that member is "Strada" and its value an object delimited
		// within the text; nothing otherwise. The text may end anywhere after it.
		std::optional<std::string_view> FindLeadingHeader(std::string_view text)
		{
			// A byte order mark is skipped, as ReadTextFile strips one.
			size_t offset = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
			offset = SkipWhitespace(text, offset);
			if (offset >= text.size() || text[offset] != '{')
			{
				return std::nullopt;
			}
			offset = SkipWhitespace(text, offset + 1);
			if (text.substr(offset, HeaderKey.size()) != HeaderKey)
			{
				return std::nullopt;
			}
			offset = SkipWhitespace(text, offset + HeaderKey.size());
			if (offset >= text.size() || text[offset] != ':')
			{
				return std::nullopt;
			}
			offset = SkipWhitespace(text, offset + 1);
			if (offset >= text.size() || text[offset] != '{')
			{
				return std::nullopt;
			}
			size_t const end = FindContainerEnd(text, offset);
			if (end == std::string_view::npos)
			{
				return std::nullopt;
			}
			return text.substr(offset, end - offset);
		}

		// The version a header object's text names when its Type is `documentType`, brought within 0 and INT_MAX.
		std::optional<int> ReadHeaderVersion(std::string_view header, std::string_view documentType)
		{
			Result<Json> const parsed = ParseJson(header);
			if (!parsed || !parsed.GetValue().is_object())
			{
				return std::nullopt;
			}
			Json const& value = parsed.GetValue();
			auto const type = value.find("Type");
			if (type == value.end() || !type->is_string() || type->get_ref<std::string const&>() != documentType)
			{
				return std::nullopt;
			}
			auto const version = value.find("Version");
			if (version == value.end() || !version->is_number_integer())
			{
				return std::nullopt;
			}
			constexpr uint64_t Largest = static_cast<uint64_t>(std::numeric_limits<int>::max());
			if (version->is_number_unsigned())
			{
				uint64_t const number = version->get<uint64_t>();
				return number > Largest ? std::numeric_limits<int>::max() : static_cast<int>(number);
			}
			int64_t const number = version->get<int64_t>();
			if (number < 1)
			{
				return 0;
			}
			return static_cast<uint64_t>(number) > Largest ? std::numeric_limits<int>::max() : static_cast<int>(number);
		}

		bool IsControlCharacter(char32_t codePoint)
		{
			return codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
		}

		bool IsFinite(glm::vec4 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		Result<void> ValidateElement(ScriptFieldValue const& value)
		{
			switch (value.GetType())
			{
				case ScriptFieldType::Float:
					return std::isfinite(value.GetFloat()) ? Result<void>() : Error{"the number is not finite"};
				case ScriptFieldType::Double:
					return std::isfinite(value.GetDouble()) ? Result<void>() : Error{"the number is not finite"};
				case ScriptFieldType::Vector2:
					return IsFinite(glm::vec4(value.GetVector2(), 0.0f, 0.0f)) ? Result<void>() : Error{"a component is not finite"};
				case ScriptFieldType::Vector3:
					return IsFinite(glm::vec4(value.GetVector3(), 0.0f)) ? Result<void>() : Error{"a component is not finite"};
				case ScriptFieldType::Vector4:
				case ScriptFieldType::Color:
					return IsFinite(value.GetVector4()) ? Result<void>() : Error{"a component is not finite"};
				case ScriptFieldType::Quaternion:
				{
					glm::quat const quaternion = value.GetQuaternion();
					if (!IsFinite(glm::vec4(quaternion.x, quaternion.y, quaternion.z, quaternion.w)))
					{
						return Error{"a component is not finite"};
					}
					if (!(Math::QuaternionLength(quaternion) >= MinSavedQuaternionLength))
					{
						return MakeError("the quaternion's length is below {}", MinSavedQuaternionLength);
					}
					return {};
				}
				case ScriptFieldType::None:
					return Error{"the value has no type"};
				case ScriptFieldType::Bool:
				case ScriptFieldType::Int32:
				case ScriptFieldType::UInt32:
				case ScriptFieldType::Int64:
				case ScriptFieldType::UInt64:
				case ScriptFieldType::String:
				case ScriptFieldType::Entity:
				case ScriptFieldType::Prefab:
				case ScriptFieldType::Asset:
					return {};
			}
			return Error{"the value has no type"};
		}

		// One element as ScriptFieldValueToJson gives it, written as DumpJson writes that.
		void WriteElement(JsonTextWriter& writer, ScriptFieldValue const& value)
		{
			auto const writeFloats = [&writer](std::initializer_list<float> components)
			{
				writer.BeginArray();
				for (float const component : components)
				{
					writer.Double(static_cast<double>(component));
				}
				writer.EndArray();
			};
			switch (value.GetType())
			{
				case ScriptFieldType::Bool:
					writer.Bool(value.GetBool());
					return;
				case ScriptFieldType::Int32:
					writer.Integer(value.GetInt32());
					return;
				case ScriptFieldType::UInt32:
					writer.Unsigned(value.GetUInt32());
					return;
				case ScriptFieldType::Int64:
					writer.String(std::to_string(value.GetInt64()));
					return;
				case ScriptFieldType::UInt64:
					writer.String(std::to_string(value.GetUInt64()));
					return;
				case ScriptFieldType::Float:
					writer.Double(static_cast<double>(value.GetFloat()));
					return;
				case ScriptFieldType::Double:
					writer.Double(value.GetDouble());
					return;
				case ScriptFieldType::String:
					writer.String(value.GetString());
					return;
				case ScriptFieldType::Vector2:
				{
					glm::vec2 const vector = value.GetVector2();
					writeFloats({vector.x, vector.y});
					return;
				}
				case ScriptFieldType::Vector3:
				{
					glm::vec3 const vector = value.GetVector3();
					writeFloats({vector.x, vector.y, vector.z});
					return;
				}
				case ScriptFieldType::Vector4:
				case ScriptFieldType::Color:
				{
					glm::vec4 const vector = value.GetVector4();
					writeFloats({vector.x, vector.y, vector.z, vector.w});
					return;
				}
				case ScriptFieldType::Quaternion:
				{
					glm::quat const quaternion = value.GetQuaternion();
					writeFloats({quaternion.x, quaternion.y, quaternion.z, quaternion.w});
					return;
				}
				case ScriptFieldType::Entity:
					writer.String(value.GetEntity().ToString());
					return;
				case ScriptFieldType::Prefab:
				case ScriptFieldType::Asset:
					writer.String(value.GetAsset().ToString());
					return;
				case ScriptFieldType::None:
					break;
			}
			writer.Null();
		}

		std::filesystem::path GetSlotPath(std::filesystem::path const& directory, std::string_view stem)
		{
			return directory / FileSystem::PathFromUtf8(std::string(stem) + std::string(SaveSlotExtension));
		}

		// Up to the first `count` bytes of a file, without a leading byte order mark.
		Result<std::string> ReadFilePrefix(std::filesystem::path const& path, uint64_t count)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				return MakeError("Failed to open '{}' for reading", FileSystem::PathToUtf8(path));
			}
			std::string text(static_cast<size_t>(count), '\0');
			stream.read(text.data(), static_cast<std::streamsize>(count));
			if (stream.bad())
			{
				return MakeError("Failed to read '{}'", FileSystem::PathToUtf8(path));
			}
			text.resize(static_cast<size_t>(stream.gcount()));
			if (text.starts_with("\xEF\xBB\xBF"))
			{
				text.erase(0, 3);
			}
			return text;
		}

		// What a slot's file holds: nothing when there is no file; the whole text up to MaxSaveSlotBytes, else the first
		// MaxSaveSlotHeaderBytes (Truncated).
		struct SlotFile
		{
			bool Exists = false;
			bool Truncated = false;
			uint64_t Size = 0;
			std::string Text;
		};

		// Fails when a file stands at the path that cannot be read (or a folder does).
		Result<SlotFile> ReadSlotFile(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::file_status const status = std::filesystem::status(path, errorCode);
			SlotFile file;
			if (!std::filesystem::exists(status))
			{
				return file;
			}
			file.Exists = true;
			if (!std::filesystem::is_regular_file(status))
			{
				return MakeError("'{}' is no file", FileSystem::PathToUtf8(path.filename()));
			}
			std::optional<uint64_t> const size = FileSystem::GetFileSize(path);
			if (!size)
			{
				return MakeError("the size of '{}' cannot be read", FileSystem::PathToUtf8(path.filename()));
			}
			file.Size = *size;
			file.Truncated = *size > MaxSaveSlotBytes;
			Result<std::string> text = file.Truncated ? ReadFilePrefix(path, MaxSaveSlotHeaderBytes) : FileSystem::ReadTextFile(path);
			if (!text)
			{
				return Error{text.GetError()};
			}
			file.Text = text.TakeValue();
			return file;
		}

		// The version of the slot file on disk, read as ReadSlotFile and ReadSaveSlotText read it, without reading past the
		// header when it comes first (as writers write it): a write checks it before replacing the file. Nothing when there
		// is no file or its header names none; fails when a file stands there that cannot be read.
		Result<std::optional<int>> ReadSlotFileVersion(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::file_status const status = std::filesystem::status(path, errorCode);
			if (!std::filesystem::exists(status))
			{
				return std::optional<int>();
			}
			if (!std::filesystem::is_regular_file(status))
			{
				return MakeError("'{}' is no file", FileSystem::PathToUtf8(path.filename()));
			}
			std::optional<uint64_t> const size = FileSystem::GetFileSize(path);
			if (!size)
			{
				return MakeError("the size of '{}' cannot be read", FileSystem::PathToUtf8(path.filename()));
			}
			Result<std::string> const prefix = ReadFilePrefix(path, std::min(*size, MaxSaveSlotHeaderBytes));
			if (!prefix)
			{
				return Error{prefix.GetError()};
			}
			if (std::optional<std::string_view> const header = FindLeadingHeader(prefix.GetValue()))
			{
				return ReadHeaderVersion(*header, SaveSlotFormat.Type);
			}
			if (*size <= MaxSaveSlotHeaderBytes || *size > MaxSaveSlotBytes)
			{
				// The prefix is all a read looks at.
				return ReadSaveSlotVersion(prefix.GetValue());
			}
			Result<std::string> const text = FileSystem::ReadTextFile(path);
			if (!text)
			{
				return Error{text.GetError()};
			}
			return ReadSaveSlotVersion(text.GetValue());
		}

		// Refuses to replace or remove the slot on disk when it is of a newer version or cannot be read (it may be one).
		Result<void> CheckReplaceable(std::filesystem::path const& path)
		{
			Result<std::optional<int>> const version = ReadSlotFileVersion(path);
			if (!version)
			{
				return MakeError("the slot on disk cannot be read, so it is kept: {}", version.GetError());
			}
			if (version.GetValue() && *version.GetValue() > static_cast<int>(SaveSlotFormat.Version))
			{
				return MakeError("the slot on disk is of version {}, newer than this engine's {}: it is kept", *version.GetValue(),
				                 SaveSlotFormat.Version);
			}
			return {};
		}

		// A slot's text as DumpJson(WriteSaveSlot(slot)) writes it, without building the document (a slot's values are most of
		// the cost of a save); refused when it passes maxBytes, stopping at the value that passes it.
		Result<std::string> WriteSlotText(SaveSlotData const& slot, uint64_t maxBytes)
		{
			auto const tooLarge = [maxBytes](size_t bytes)
			{
				return MakeError("the slot's text has {} bytes, more than {}", bytes, maxBytes);
			};
			JsonTextWriter writer({});
			writer.BeginObject();
			writer.Key("Strada");
			writer.BeginObject();
			writer.Key("Version");
			writer.Integer(static_cast<int64_t>(SaveSlotFormat.Version));
			writer.Key("Type");
			writer.String(SaveSlotFormat.Type);
			writer.EndObject();
			writer.Key(SaveSlotFormat.Type);
			writer.BeginObject();
			writer.Key("Values");
			writer.BeginObject();
			for (auto const& [name, value] : slot.Values)
			{
				writer.Key(name);
				writer.BeginObject();
				writer.Key("Type");
				writer.String(ScriptFieldTypeToString(value.GetType()));
				writer.Key("Value");
				if (value.IsList())
				{
					writer.BeginArray();
					for (ScriptFieldValue const& element : value.GetElements())
					{
						WriteElement(writer, element);
					}
					writer.EndArray();
				}
				else
				{
					WriteElement(writer, value);
				}
				writer.EndObject();
				if (writer.GetSize() > maxBytes)
				{
					return tooLarge(writer.GetSize());
				}
			}
			writer.EndObject();
			writer.EndObject();
			writer.EndObject();
			std::string text = writer.TakeText();
			text += '\n';
			if (text.size() > maxBytes)
			{
				return tooLarge(text.size());
			}
			return text;
		}

		Error MakeUnavailableError()
		{
			return Error{"no game runs (no game data folder): nothing is written"};
		}
	}

	Result<void> ValidateSavedValueName(std::string_view name)
	{
		if (name.empty())
		{
			return Error{"a value's name is empty"};
		}
		if (name.size() > MaxSavedValueNameLength)
		{
			return MakeError("a value's name has at most {} bytes, not {}", MaxSavedValueNameLength, name.size());
		}
		if (!Utf8::IsValid(name))
		{
			return Error{"a value's name is not UTF-8"};
		}
		for (size_t offset = 0; offset < name.size();)
		{
			if (IsControlCharacter(Utf8::DecodeNext(name, offset)))
			{
				return Error{"a value's name holds a control character"};
			}
		}
		return {};
	}

	Result<void> ValidateSavedValue(ScriptFieldValue const& value)
	{
		if (!IsValidScriptFieldType(value.GetType()))
		{
			return Error{"the value has no type"};
		}
		if (!value.IsList())
		{
			return ValidateElement(value);
		}
		std::span<ScriptFieldValue const> const elements = value.GetElements();
		if (elements.size() > MaxScriptFieldListElements)
		{
			return MakeError("a list holds at most {} elements, not {}", MaxScriptFieldListElements, elements.size());
		}
		for (size_t index = 0; index < elements.size(); index++)
		{
			if (Result<void> valid = ValidateElement(elements[index]); !valid)
			{
				return MakeError("element {}: {}", index, valid.GetError());
			}
		}
		return {};
	}

	Result<void> ValidateSaveSlot(SaveSlotData const& slot)
	{
		if (slot.Values.size() > MaxSaveSlotValues)
		{
			return MakeError("a slot holds at most {} values, not {}", MaxSaveSlotValues, slot.Values.size());
		}
		for (auto const& [name, value] : slot.Values)
		{
			if (Result<void> valid = ValidateSavedValueName(name); !valid)
			{
				return valid;
			}
			if (Result<void> valid = ValidateSavedValue(value); !valid)
			{
				return MakeError("value '{}': {}", name, valid.GetError());
			}
		}
		return {};
	}

	std::optional<int> ReadDocumentHeaderVersion(std::string_view text, std::string_view type)
	{
		if (std::optional<std::string_view> const header = FindLeadingHeader(text))
		{
			return ReadHeaderVersion(*header, type);
		}
		std::optional<std::vector<JsonScanMember>> const members = ScanJsonTopLevel(text);
		if (!members)
		{
			return std::nullopt;
		}
		auto const last = std::find_if(members->rbegin(), members->rend(),
		                               [](JsonScanMember const& member)
		                               {
										   return member.Key == "Strada";
									   });
		if (last == members->rend())
		{
			return std::nullopt;
		}
		return ReadHeaderVersion(text.substr(last->Value.first, last->Value.second), type);
	}

	std::optional<int> ReadSaveSlotVersion(std::string_view text)
	{
		return ReadDocumentHeaderVersion(text, SaveSlotFormat.Type);
	}

	SaveSlotRead ReadSaveSlotText(std::string_view text)
	{
		SaveSlotRead read;
		std::optional<int> const version = ReadSaveSlotVersion(text);
		if (version && *version > static_cast<int>(SaveSlotFormat.Version))
		{
			read.Status = SaveSlotStatus::Newer;
			read.Version = *version;
			read.Problem = fmt::format("it is of version {}, newer than this engine's {}", *version, SaveSlotFormat.Version);
			return read;
		}
		read.Status = SaveSlotStatus::Damaged;
		read.Version = version.value_or(0);
		if (text.size() > MaxSaveSlotBytes)
		{
			read.Problem = fmt::format("it has more than {} bytes", MaxSaveSlotBytes);
			return read;
		}
		Result<Json> const document = ParseJson(text);
		if (!document)
		{
			read.Problem = document.GetError();
			return read;
		}
		std::vector<std::string> warnings;
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		context.Warnings = &warnings;
		Result<SaveSlotData> data = ReadStructDocument<SaveSlotData>(document.GetValue(), SaveSlotFormat, &ValidateSaveSlot, context);
		if (!data)
		{
			read.Problem = data.GetError();
			return read;
		}
		read.Status = SaveSlotStatus::Present;
		read.Version = static_cast<int>(SaveSlotFormat.Version);
		read.Data = data.TakeValue();
		read.Warnings = std::move(warnings);
		return read;
	}

	Json WriteSaveSlot(SaveSlotData const& slot)
	{
		return WriteStructDocument(slot, SaveSlotFormat);
	}

	Result<std::string> WriteSaveSlotText(SaveSlotData const& slot)
	{
		if (Result<void> valid = ValidateSaveSlot(slot); !valid)
		{
			return Error{valid.GetError()};
		}
		return WriteSlotText(slot, MaxSaveSlotBytes);
	}

	std::string WriteReadSaveSlotText(SaveSlotData const& slot)
	{
		// Without a bound the writer refuses nothing.
		Result<std::string> text = WriteSlotText(slot, std::numeric_limits<uint64_t>::max());
		return text ? text.TakeValue() : std::string();
	}

	Result<std::string> SaveSlotStore::MakeFileStem(std::string_view name)
	{
		if (name.empty())
		{
			return Error{"a slot's name is empty"};
		}
		if (name.size() > MaxSaveSlotNameLength)
		{
			return MakeError("a slot's name has at most {} characters, not {}", MaxSaveSlotNameLength, name.size());
		}
		if (FileSystem::MakePortableFileName(name) != name)
		{
			return MakeError("a slot's name is ASCII letters, digits, spaces, '-' and '_', with no space at either end and not a "
			                 "name Windows keeps for a device, not '{}'",
			                 Utf8::Sanitize(name));
		}
		std::string stem(name);
		std::transform(stem.begin(), stem.end(), stem.begin(),
		               [](unsigned char c)
		               {
						   return static_cast<char>(std::tolower(c));
					   });
		return stem;
	}

	std::optional<std::filesystem::path> SaveSlotStore::GetDirectory()
	{
		std::optional<std::filesystem::path> directory = GameData::GetDirectory();
		if (!directory)
		{
			return std::nullopt;
		}
		return *directory / FileSystem::PathFromUtf8(SaveSlotDirectoryName);
	}

	Result<SaveSlotRead> SaveSlotStore::Read(std::string_view name)
	{
		ST_PROFILE_SCOPE("Saves.ReadSlot");
		Result<std::string> const stem = MakeFileStem(name);
		if (!stem)
		{
			return Error{stem.GetError()};
		}
		std::optional<std::filesystem::path> const directory = GetDirectory();
		if (!directory)
		{
			SaveSlotRead read;
			read.Status = SaveSlotStatus::Unavailable;
			return read;
		}
		Result<SlotFile> const file = ReadSlotFile(GetSlotPath(*directory, stem.GetValue()));
		if (!file)
		{
			// A file that cannot be read reads as empty; writes refuse to replace it, since it may be of a newer version.
			SaveSlotRead read;
			read.Status = SaveSlotStatus::Damaged;
			read.Problem = file.GetError();
			return read;
		}
		if (!file.GetValue().Exists)
		{
			return SaveSlotRead{};
		}
		if (file.GetValue().Truncated)
		{
			SaveSlotRead read;
			std::optional<int> const version = ReadSaveSlotVersion(file.GetValue().Text);
			read.Version = version.value_or(0);
			if (version && *version > static_cast<int>(SaveSlotFormat.Version))
			{
				read.Status = SaveSlotStatus::Newer;
				read.Problem = fmt::format("it is of version {}, newer than this engine's {}", *version, SaveSlotFormat.Version);
			}
			else
			{
				read.Status = SaveSlotStatus::Damaged;
				read.Problem = fmt::format("it has {} bytes, more than {}", file.GetValue().Size, MaxSaveSlotBytes);
			}
			return read;
		}
		return ReadSaveSlotText(file.GetValue().Text);
	}

	Result<void> SaveSlotStore::Write(std::string_view name, SaveSlotData const& slot)
	{
		ST_PROFILE_SCOPE("Saves.WriteSlot");
		Result<std::string> const stem = MakeFileStem(name);
		if (!stem)
		{
			return Error{stem.GetError()};
		}
		std::optional<std::filesystem::path> const directory = GetDirectory();
		if (!directory)
		{
			return MakeUnavailableError();
		}
		Result<std::string> const text = WriteSaveSlotText(slot);
		if (!text)
		{
			return Error{text.GetError()};
		}
		std::filesystem::path const path = GetSlotPath(*directory, stem.GetValue());
		if (Result<void> replaceable = CheckReplaceable(path); !replaceable)
		{
			return replaceable;
		}
		return FileSystem::WriteTextFileDurably(path, text.GetValue());
	}

	Result<void> SaveSlotStore::Delete(std::string_view name)
	{
		Result<std::string> const stem = MakeFileStem(name);
		if (!stem)
		{
			return Error{stem.GetError()};
		}
		std::optional<std::filesystem::path> const directory = GetDirectory();
		if (!directory)
		{
			return Error{"no game runs (no game data folder): nothing is deleted"};
		}
		std::filesystem::path const path = GetSlotPath(*directory, stem.GetValue());
		std::error_code errorCode;
		if (!std::filesystem::exists(std::filesystem::symlink_status(path, errorCode)))
		{
			return {};
		}
		if (Result<void> replaceable = CheckReplaceable(path); !replaceable)
		{
			return replaceable;
		}
		return FileSystem::Remove(path);
	}

	std::vector<std::string> SaveSlotStore::GetNames()
	{
		std::vector<std::string> names;
		std::optional<std::filesystem::path> const directory = GetDirectory();
		if (!directory)
		{
			return names;
		}
		std::error_code errorCode;
		std::filesystem::directory_iterator entries(*directory, errorCode);
		if (errorCode)
		{
			return names;
		}
		for (std::filesystem::directory_iterator const end{}; entries != end; entries.increment(errorCode))
		{
			if (errorCode)
			{
				break;
			}
			std::filesystem::directory_entry const& entry = *entries;
			std::error_code statusError;
			if (!entry.is_regular_file(statusError) || entry.path().extension() != FileSystem::PathFromUtf8(SaveSlotExtension))
			{
				continue;
			}
			std::string const stem = FileSystem::PathToUtf8(entry.path().stem());
			Result<std::string> const fileStem = MakeFileStem(stem);
			if (fileStem && fileStem.GetValue() == stem)
			{
				names.push_back(stem);
			}
		}
		std::sort(names.begin(), names.end());
		return names;
	}
}
