#include "stpch.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include "Strada/Script/ScriptFieldSerialization.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<std::string_view, 17> TypeKindNames = {"Exec",       "Bool",   "Integer", "Float",  "String",    "Vector",
		                                                            "Quaternion", "Color",  "Matrix",  "Entity", "Component", "Asset",
		                                                            "Enum",       "Struct", "Class",   "Array",  "Object"};
		static_assert(TypeKindNames.size() == static_cast<size_t>(VisualScriptTypeKind::Object) + 1);

		constexpr std::array<std::string_view, 11> NodeKindNames = {"Event",    "Flow",     "Call", "Pure",    "Latent", "Variable",
		                                                            "Function", "Operator", "Data", "Reroute", "Unknown"};
		static_assert(NodeKindNames.size() == static_cast<size_t>(VisualScriptNodeKind::Unknown) + 1);

		constexpr std::array<std::string_view, 2> DirectionNames = {"In", "Out"};
		static_assert(DirectionNames.size() == static_cast<size_t>(VisualScriptPinDirection::Out) + 1);

		constexpr std::array<std::string_view, 3> RoleNames = {"None", "Continuation", "Tick"};
		static_assert(RoleNames.size() == static_cast<size_t>(VisualScriptPinRole::Tick) + 1);

		constexpr std::array<std::string_view, 5> ConstraintNames = {"None", "Any", "Numeric", "Reference", "Array"};
		static_assert(ConstraintNames.size() == static_cast<size_t>(VisualScriptPinConstraint::Array) + 1);

		constexpr std::array<std::string_view, 4> ResolutionNames = {"Exact", "Redirect", "Unique", "Unknown"};
		static_assert(ResolutionNames.size() == static_cast<size_t>(VisualScriptResolution::Unknown) + 1);

		constexpr std::array<std::string_view, 2> SeverityNames = {"Error", "Warning"};
		static_assert(SeverityNames.size() == static_cast<size_t>(VisualScriptSeverity::Warning) + 1);

		enum class Presence : uint8_t
		{
			Required = 0,
			Optional
		};

		template<size_t Count>
		std::string JoinNames(std::array<std::string_view, Count> const& names)
		{
			std::string joined;
			for (std::string_view const name : names)
			{
				joined += joined.empty() ? "" : ", ";
				joined += name;
			}
			return joined;
		}

		// Reads the members of one object of the runtime's JSON and keeps the first problem, so a parse function reads every
		// member it needs and then asks how it went. Problems name the member: "Nodes[3].Pins[1].Direction: ...".
		class ObjectReader
		{
		public:
			ObjectReader(Json const& json, std::string path)
				: m_Json(json),
				  m_Path(std::move(path))
			{
				if (!m_Json.is_object())
				{
					Fail({}, "expected an object");
				}
			}

			// The path of a member; the object's own for an empty key.
			std::string GetPath(std::string_view key) const
			{
				if (key.empty())
				{
					return m_Path;
				}
				return m_Path.empty() ? std::string(key) : fmt::format("{}.{}", m_Path, key);
			}

			// Null when the member is missing.
			Json const* Find(char const* key) const
			{
				if (!m_Json.is_object())
				{
					return nullptr;
				}
				auto const member = m_Json.find(key);
				return member != m_Json.end() ? &*member : nullptr;
			}

			// Empty when an optional string is missing.
			std::string ReadString(char const* key, Presence presence = Presence::Required)
			{
				Json const* member = Find(key);
				if (member == nullptr || !member->is_string())
				{
					if (member != nullptr || presence == Presence::Required)
					{
						Fail(key, "expected a string");
					}
					return {};
				}
				return member->get<std::string>();
			}

			// A string that is there and not empty: IDs, types and names.
			std::string ReadName(char const* key)
			{
				std::string name = ReadString(key);
				if (name.empty())
				{
					Fail(key, "expected a string that is not empty");
				}
				return name;
			}

			// False when an optional flag is missing.
			bool ReadBool(char const* key, Presence presence = Presence::Required)
			{
				Json const* member = Find(key);
				if (member == nullptr || !member->is_boolean())
				{
					if (member != nullptr || presence == Presence::Required)
					{
						Fail(key, "expected true or false");
					}
					return false;
				}
				return member->get<bool>();
			}

			uint32_t ReadUInt32(char const* key)
			{
				Json const* member = Find(key);
				if (member == nullptr)
				{
					Fail(key, "expected an integer of 0 to 4294967295");
					return 0;
				}
				return ToUInt32(key, *member).value_or(0);
			}

			std::optional<uint32_t> ReadOptionalUInt32(char const* key)
			{
				Json const* member = Find(key);
				return member != nullptr ? ToUInt32(key, *member) : std::nullopt;
			}

			std::optional<int64_t> ReadOptionalInteger(char const* key)
			{
				Json const* member = Find(key);
				if (member == nullptr)
				{
					return std::nullopt;
				}
				if (member->is_number_unsigned() && member->get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
				{
					return static_cast<int64_t>(member->get<uint64_t>());
				}
				if (member->is_number_integer() && !member->is_number_unsigned())
				{
					return member->get<int64_t>();
				}
				Fail(key, "expected a 64-bit integer");
				return std::nullopt;
			}

			// Empty when an optional list is missing.
			std::vector<std::string> ReadStrings(char const* key, Presence presence)
			{
				Json const* member = ReadArray(key, presence);
				std::vector<std::string> strings;
				if (member == nullptr)
				{
					return strings;
				}
				strings.reserve(member->size());
				for (Json const& element : *member)
				{
					if (!element.is_string())
					{
						Fail(key, "expected an array of strings");
						return {};
					}
					strings.push_back(element.get<std::string>());
				}
				return strings;
			}

			// One of the names, as its index in the enum; missing when an optional member is not there.
			template<typename TEnum, size_t Count>
			TEnum ReadEnum(char const* key, std::array<std::string_view, Count> const& names, Presence presence, TEnum missing)
			{
				Json const* member = Find(key);
				if (member == nullptr)
				{
					if (presence == Presence::Required)
					{
						Fail(key, fmt::format("expected one of {}", JoinNames(names)));
					}
					return missing;
				}
				if (std::optional<TEnum> const value = ReadEnumValue<TEnum>(key, *member, names))
				{
					return *value;
				}
				return missing;
			}

			template<typename TEnum, size_t Count>
			std::optional<TEnum> ReadOptionalEnum(char const* key, std::array<std::string_view, Count> const& names)
			{
				Json const* member = Find(key);
				return member != nullptr ? ReadEnumValue<TEnum>(key, *member, names) : std::nullopt;
			}

			// A copy of the member; null when it is missing.
			Json ReadValue(char const* key) const
			{
				Json const* member = Find(key);
				return member != nullptr ? *member : Json();
			}

			// Null when an optional array is missing.
			Json const* ReadArray(char const* key, Presence presence)
			{
				Json const* member = Find(key);
				if (member == nullptr || !member->is_array())
				{
					if (member != nullptr || presence == Presence::Required)
					{
						Fail(key, "expected an array");
					}
					return nullptr;
				}
				return member;
			}

			// Keeps the first problem only: later ones often follow from it.
			void Fail(std::string_view key, std::string_view problem)
			{
				if (!m_Error)
				{
					m_Error = fmt::format("{}: {}", key.empty() && m_Path.empty() ? std::string("the document") : GetPath(key), problem);
				}
			}

			// A problem of a member read by another reader, whose message names its path already.
			void Keep(std::string const& message)
			{
				if (!m_Error)
				{
					m_Error = message;
				}
			}

			bool IsOk() const { return !m_Error.has_value(); }

			Result<void> Finish() const
			{
				if (m_Error)
				{
					return Error{*m_Error};
				}
				return {};
			}

		private:
			std::optional<uint32_t> ToUInt32(std::string_view key, Json const& value)
			{
				if (value.is_number_unsigned() && value.get<uint64_t>() <= std::numeric_limits<uint32_t>::max())
				{
					return static_cast<uint32_t>(value.get<uint64_t>());
				}
				Fail(key, "expected an integer of 0 to 4294967295");
				return std::nullopt;
			}

			template<typename TEnum, size_t Count>
			std::optional<TEnum> ReadEnumValue(std::string_view key, Json const& value, std::array<std::string_view, Count> const& names)
			{
				if (value.is_string())
				{
					std::string const& text = value.get_ref<std::string const&>();
					auto const name = std::find(names.begin(), names.end(), text);
					if (name != names.end())
					{
						return static_cast<TEnum>(name - names.begin());
					}
				}
				Fail(key, fmt::format("expected one of {}", JoinNames(names)));
				return std::nullopt;
			}

			Json const& m_Json;
			std::string m_Path;
			std::optional<std::string> m_Error;
		};

		std::string GetElementPath(ObjectReader const& reader, std::string_view key, size_t index)
		{
			return fmt::format("{}[{}]", reader.GetPath(key), index);
		}

		bool IsIntegerType(ScriptFieldType type)
		{
			return type == ScriptFieldType::Int32 || type == ScriptFieldType::UInt32 || type == ScriptFieldType::Int64 ||
			       type == ScriptFieldType::UInt64;
		}

		char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		bool ContainsIgnoringCase(std::string_view text, std::string_view part)
		{
			return std::search(text.begin(), text.end(), part.begin(), part.end(),
			                   [](char a, char b)
			                   {
								   return ToLowerAscii(a) == ToLowerAscii(b);
							   }) != text.end();
		}

		Result<VisualScriptPin> ReadPin(Json const& json, std::string path, VisualScriptNodeKind nodeKind)
		{
			ObjectReader reader(json, std::move(path));
			VisualScriptPin pin;
			pin.Name = reader.ReadName("Name");
			pin.Direction = reader.ReadEnum("Direction", DirectionNames, Presence::Required, VisualScriptPinDirection::In);
			pin.Type = reader.ReadName("Type");
			pin.Label = reader.ReadString("Label", Presence::Optional);
			pin.Default = reader.ReadValue("Default");
			pin.DefaultsToSelf = reader.ReadBool("DefaultsToSelf", Presence::Optional);
			pin.Role = reader.ReadEnum("Role", RoleNames, Presence::Optional, VisualScriptPinRole::None);
			pin.Constraint = reader.ReadEnum("Constraint", ConstraintNames, Presence::Optional, VisualScriptPinConstraint::None);
			pin.Group = reader.ReadString("Group", Presence::Optional);
			pin.ElementOf = reader.ReadString("ElementOf", Presence::Optional);
			// A Reroute in a chain of exec links is part of the flow.
			pin.CarriesExec = nodeKind == VisualScriptNodeKind::Reroute && pin.Type == VisualScriptWildcardType;
			if (Result<void> finished = reader.Finish(); !finished)
			{
				return Error{finished.GetError()};
			}
			return pin;
		}

		Result<std::vector<VisualScriptPin>> ReadPins(ObjectReader& reader, VisualScriptNodeKind nodeKind)
		{
			std::vector<VisualScriptPin> pins;
			Json const* json = reader.ReadArray("Pins", Presence::Required);
			if (json == nullptr)
			{
				return pins;
			}
			pins.reserve(json->size());
			for (size_t index = 0; index < json->size(); ++index)
			{
				Result<VisualScriptPin> pin = ReadPin((*json)[index], GetElementPath(reader, "Pins", index), nodeKind);
				if (!pin)
				{
					return Error{pin.GetError()};
				}
				pins.push_back(pin.TakeValue());
			}
			return pins;
		}

		Result<VisualScriptSetting> ReadSetting(Json const& json, std::string path)
		{
			ObjectReader reader(json, std::move(path));
			VisualScriptSetting setting;
			setting.Name = reader.ReadName("Name");
			setting.Kind = reader.ReadName("Kind");
			setting.Default = reader.ReadValue("Default");
			setting.Required = reader.ReadBool("Required", Presence::Optional);
			setting.Min = reader.ReadOptionalInteger("Min");
			setting.Max = reader.ReadOptionalInteger("Max");
			setting.Constraint = reader.ReadString("Constraint", Presence::Optional);
			setting.Type = reader.ReadString("Type", Presence::Optional);
			setting.Choices = reader.ReadStrings("Choices", Presence::Optional);
			if (Result<void> finished = reader.Finish(); !finished)
			{
				return Error{finished.GetError()};
			}
			return setting;
		}

		// An enum's enumerators, as values of its field type.
		void ReadEnumerators(ObjectReader& reader, VisualScriptTypeInfo& type)
		{
			Json const* enumerators = reader.ReadArray("Enumerators", Presence::Required);
			if (!IsIntegerType(type.FieldType))
			{
				reader.Fail("FieldType", "an enum is stored as Int32, UInt32, Int64 or UInt64");
				return;
			}
			if (enumerators == nullptr)
			{
				return;
			}
			for (size_t index = 0; index < enumerators->size(); ++index)
			{
				ObjectReader enumerator((*enumerators)[index], GetElementPath(reader, "Enumerators", index));
				std::string name = enumerator.ReadName("Name");
				Json const* value = enumerator.Find("Value");
				if (value == nullptr)
				{
					enumerator.Fail("Value", "expected the enumerator's value");
				}
				else if (Result<ScriptFieldValue> parsed = ScriptFieldValueFromJson(type.FieldType, *value, DeserializationContext{});
				         !parsed)
				{
					enumerator.Fail("Value", parsed.GetError());
				}
				else
				{
					type.Enumerators.push_back({std::move(name), parsed.TakeValue()});
				}
				if (Result<void> finished = enumerator.Finish(); !finished)
				{
					reader.Keep(finished.GetError());
					return;
				}
			}
		}

		void ReadStructFields(ObjectReader& reader, VisualScriptTypeInfo& type)
		{
			Json const* fields = reader.ReadArray("Fields", Presence::Optional);
			if (fields == nullptr)
			{
				return;
			}
			for (size_t index = 0; index < fields->size(); ++index)
			{
				ObjectReader field((*fields)[index], GetElementPath(reader, "Fields", index));
				VisualScriptStructField read;
				read.Name = field.ReadName("Name");
				read.Type = field.ReadName("Type");
				if (Result<void> finished = field.Finish(); !finished)
				{
					reader.Keep(finished.GetError());
					return;
				}
				type.Fields.push_back(std::move(read));
			}
		}

		// Reads a type of the library; its JSON moves into the type's Source.
		Result<VisualScriptTypeInfo> ReadType(Json& json, std::string path)
		{
			ObjectReader reader(json, std::move(path));
			VisualScriptTypeInfo type;
			type.ID = reader.ReadName("ID");
			type.Title = reader.ReadString("Title");
			type.Kind = reader.ReadEnum("Kind", TypeKindNames, Presence::Required, VisualScriptTypeKind::Object);
			if (std::string const fieldType = reader.ReadString("FieldType", Presence::Optional); !fieldType.empty())
			{
				std::optional<ScriptFieldType> const parsed = ScriptFieldTypeFromString(fieldType);
				if (!parsed)
				{
					reader.Fail("FieldType", fmt::format("{} is not a script field type", fieldType));
				}
				type.FieldType = parsed.value_or(ScriptFieldType::None);
			}
			if (std::string const assetType = reader.ReadString("AssetType", Presence::Optional); !assetType.empty())
			{
				std::optional<AssetType> const parsed = AssetTypeFromString(assetType);
				if (!parsed)
				{
					reader.Fail("AssetType", fmt::format("{} is not an asset type", assetType));
				}
				type.AcceptedAssetType = parsed.value_or(AssetType::None);
			}
			type.HasLiteral = reader.ReadBool("Literal", Presence::Optional);
			if (type.Kind == VisualScriptTypeKind::Enum)
			{
				ReadEnumerators(reader, type);
				type.IsFlags = reader.ReadBool("Flags");
			}
			else if (reader.Find("Enumerators") != nullptr)
			{
				reader.Fail("Enumerators", "only enums have enumerators");
			}
			type.Makeable = reader.ReadBool("Makeable", Presence::Optional);
			type.Breakable = reader.ReadBool("Breakable", Presence::Optional);
			ReadStructFields(reader, type);
			type.Base = reader.ReadString("Base", Presence::Optional);
			type.Element = reader.ReadString("Element", Presence::Optional);
			type.ImplicitTo = reader.ReadStrings("ImplicitTo", Presence::Required);
			if (Result<void> finished = reader.Finish(); !finished)
			{
				return Error{finished.GetError()};
			}
			type.Source = std::move(json);
			return type;
		}

		// Reads a node kind of the library; its JSON moves into the node's Source.
		Result<VisualScriptNodeTemplate> ReadNodeTemplate(Json& json, std::string path)
		{
			ObjectReader reader(json, std::move(path));
			VisualScriptNodeTemplate node;
			node.Type = reader.ReadName("Type");
			node.Title = reader.ReadString("Title");
			node.Subtitle = reader.ReadString("Subtitle", Presence::Optional);
			node.Category = reader.ReadString("Category");
			node.Description = reader.ReadString("Description", Presence::Optional);
			node.Keywords = reader.ReadStrings("Keywords", Presence::Required);
			node.Kind = reader.ReadEnum("Kind", NodeKindNames, Presence::Required, VisualScriptNodeKind::Unknown);
			node.Pure = reader.ReadBool("Pure");
			node.Latent = reader.ReadBool("Latent");
			if (Result<std::vector<VisualScriptPin>> pins = ReadPins(reader, node.Kind); pins)
			{
				node.Pins = pins.TakeValue();
			}
			else
			{
				reader.Keep(pins.GetError());
			}
			if (Json const* settings = reader.ReadArray("Settings", Presence::Required))
			{
				for (size_t index = 0; index < settings->size() && reader.IsOk(); ++index)
				{
					Result<VisualScriptSetting> setting = ReadSetting((*settings)[index], GetElementPath(reader, "Settings", index));
					if (!setting)
					{
						reader.Keep(setting.GetError());
						break;
					}
					node.Settings.push_back(setting.TakeValue());
				}
			}
			if (Result<void> finished = reader.Finish(); !finished)
			{
				return Error{finished.GetError()};
			}
			node.Source = std::move(json);
			return node;
		}

		Result<VisualScriptPinRename> ReadPinRename(Json const& json, std::string path)
		{
			ObjectReader reader(json, std::move(path));
			VisualScriptPinRename rename;
			rename.Direction = reader.ReadEnum("Direction", DirectionNames, Presence::Required, VisualScriptPinDirection::In);
			rename.From = reader.ReadName("From");
			rename.To = reader.ReadName("To");
			if (Result<void> finished = reader.Finish(); !finished)
			{
				return Error{finished.GetError()};
			}
			return rename;
		}

		Result<VisualScriptAnalyzedNode> ReadAnalyzedNode(Json const& json, std::string path)
		{
			ObjectReader reader(json, std::move(path));
			VisualScriptAnalyzedNode node;
			node.ID = reader.ReadUInt32("ID");
			node.Function = reader.ReadUInt32("Function");
			node.Type = reader.ReadName("Type");
			node.Resolved = reader.ReadEnum("Resolved", ResolutionNames, Presence::Required, VisualScriptResolution::Unknown);
			if (node.Resolved == VisualScriptResolution::Redirect || node.Resolved == VisualScriptResolution::Unique)
			{
				node.CanonicalType = reader.ReadName("CanonicalType");
				if (Json const* renames = reader.ReadArray("PinRenames", Presence::Required))
				{
					for (size_t index = 0; index < renames->size(); ++index)
					{
						Result<VisualScriptPinRename> rename =
							ReadPinRename((*renames)[index], GetElementPath(reader, "PinRenames", index));
						if (!rename)
						{
							reader.Keep(rename.GetError());
							break;
						}
						node.PinRenames.push_back(rename.TakeValue());
					}
				}
			}
			node.Title = reader.ReadString("Title");
			node.Subtitle = reader.ReadString("Subtitle", Presence::Optional);
			node.Kind = reader.ReadEnum("Kind", NodeKindNames, Presence::Required, VisualScriptNodeKind::Unknown);
			node.Pure = reader.ReadBool("Pure");
			node.Latent = reader.ReadBool("Latent");
			if (Result<std::vector<VisualScriptPin>> pins = ReadPins(reader, node.Kind); pins)
			{
				node.Pins = pins.TakeValue();
			}
			else
			{
				reader.Keep(pins.GetError());
			}
			// Only disabled nodes have it, and only where their flow continues.
			node.PassThrough = reader.ReadString("PassThrough", Presence::Optional);
			if (Result<void> finished = reader.Finish(); !finished)
			{
				return Error{finished.GetError()};
			}
			return node;
		}

		// A link end of a diagnostic: [node, pin].
		std::optional<VisualScriptPinReference> ReadPinReference(Json const& json)
		{
			if (!json.is_array() || json.size() != 2 || !json[0].is_number_unsigned() ||
			    json[0].get<uint64_t>() > std::numeric_limits<uint32_t>::max() || !json[1].is_string())
			{
				return std::nullopt;
			}
			return VisualScriptPinReference{static_cast<uint32_t>(json[0].get<uint64_t>()), json[1].get<std::string>()};
		}

		Result<VisualScriptDiagnostic> ReadDiagnostic(Json const& json, std::string path)
		{
			ObjectReader reader(json, std::move(path));
			VisualScriptDiagnostic diagnostic;
			diagnostic.Severity = reader.ReadEnum("Severity", SeverityNames, Presence::Required, VisualScriptSeverity::Error);
			diagnostic.Code = reader.ReadName("Code");
			diagnostic.Function = reader.ReadUInt32("Function");
			diagnostic.Node = reader.ReadOptionalUInt32("Node");
			diagnostic.Pin = reader.ReadString("Pin", Presence::Optional);
			diagnostic.Direction = reader.ReadOptionalEnum<VisualScriptPinDirection>("Direction", DirectionNames);
			if (Json const* link = reader.Find("Link"))
			{
				ObjectReader linkReader(*link, reader.GetPath("Link"));
				Json const* from = linkReader.Find("From");
				Json const* to = linkReader.Find("To");
				std::optional<VisualScriptPinReference> fromPin = from != nullptr ? ReadPinReference(*from) : std::nullopt;
				std::optional<VisualScriptPinReference> toPin = to != nullptr ? ReadPinReference(*to) : std::nullopt;
				if (!fromPin)
				{
					linkReader.Fail("From", "expected [node, pin]");
				}
				if (!toPin)
				{
					linkReader.Fail("To", "expected [node, pin]");
				}
				if (Result<void> finished = linkReader.Finish(); !finished)
				{
					reader.Keep(finished.GetError());
				}
				else
				{
					diagnostic.Link = VisualScriptLink{std::move(*fromPin), std::move(*toPin)};
				}
			}
			diagnostic.Variable = reader.ReadOptionalUInt32("Variable");
			diagnostic.Message = reader.ReadString("Message");
			diagnostic.Suggestions = reader.ReadStrings("Suggestions", Presence::Optional);
			if (Result<void> finished = reader.Finish(); !finished)
			{
				return Error{finished.GetError()};
			}
			return diagnostic;
		}

		Result<std::vector<VisualScriptDiagnostic>> ReadDiagnostics(Json const& json, std::string_view path)
		{
			if (!json.is_array())
			{
				return MakeError("{}: expected an array", path.empty() ? "the document" : path);
			}
			std::vector<VisualScriptDiagnostic> diagnostics;
			diagnostics.reserve(json.size());
			for (size_t index = 0; index < json.size(); ++index)
			{
				Result<VisualScriptDiagnostic> diagnostic = ReadDiagnostic(json[index], fmt::format("{}[{}]", path, index));
				if (!diagnostic)
				{
					return Error{diagnostic.GetError()};
				}
				diagnostics.push_back(diagnostic.TakeValue());
			}
			return diagnostics;
		}

		// Files the node under its category, creating the categories on its path (in name order).
		void AddToCategories(VisualScriptCategory& root, VisualScriptNodeTemplate const& node)
		{
			VisualScriptCategory* category = &root;
			std::string_view rest = node.Category;
			while (!rest.empty())
			{
				size_t const slash = rest.find('/');
				std::string_view const name = rest.substr(0, slash);
				rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
				if (name.empty())
				{
					continue;
				}
				auto child = std::lower_bound(category->Children.begin(), category->Children.end(), name,
				                              [](VisualScriptCategory const& candidate, std::string_view value)
				                              {
												  return std::string_view(candidate.Name) < value;
											  });
				if (child == category->Children.end() || child->Name != name)
				{
					VisualScriptCategory created;
					created.Name = std::string(name);
					created.Path = category->Path.empty() ? created.Name : fmt::format("{}/{}", category->Path, name);
					child = category->Children.insert(child, std::move(created));
				}
				category = &*child;
			}
			category->Nodes.push_back(&node);
		}

		template<typename TNode>
		VisualScriptPin const* FindPinOf(TNode const& node, std::string_view name, VisualScriptPinDirection direction)
		{
			auto const pin = std::find_if(node.Pins.begin(), node.Pins.end(),
			                              [name, direction](VisualScriptPin const& candidate)
			                              {
											  return candidate.Direction == direction && candidate.Name == name;
										  });
			return pin != node.Pins.end() ? &*pin : nullptr;
		}
	}

	VisualScriptPin const* VisualScriptNodeTemplate::FindPin(std::string_view name, VisualScriptPinDirection direction) const
	{
		return FindPinOf(*this, name, direction);
	}

	VisualScriptPin const* VisualScriptAnalyzedNode::FindPin(std::string_view name, VisualScriptPinDirection direction) const
	{
		return FindPinOf(*this, name, direction);
	}

	Result<Scope<VisualScriptLibrary>> VisualScriptLibrary::Parse(std::string_view json)
	{
		Result<Json> parsed = ParseJson(json);
		if (!parsed)
		{
			return Error{parsed.GetError()};
		}
		Json document = parsed.TakeValue();
		if (!document.is_object())
		{
			return Error{"the document: expected an object { \"Types\", \"Nodes\" }"};
		}
		auto const types = document.find("Types");
		auto const nodes = document.find("Nodes");
		if (types == document.end() || !types->is_array())
		{
			return Error{"Types: expected an array"};
		}
		if (nodes == document.end() || !nodes->is_array())
		{
			return Error{"Nodes: expected an array"};
		}

		Scope<VisualScriptLibrary> library = CreateScope<VisualScriptLibrary>();
		library->m_Types.reserve(types->size());
		for (size_t index = 0; index < types->size(); ++index)
		{
			Result<VisualScriptTypeInfo> type = ReadType((*types)[index], fmt::format("Types[{}]", index));
			if (!type)
			{
				return Error{type.GetError()};
			}
			library->m_Types.push_back(type.TakeValue());
		}
		library->m_Nodes.reserve(nodes->size());
		for (size_t index = 0; index < nodes->size(); ++index)
		{
			Result<VisualScriptNodeTemplate> node = ReadNodeTemplate((*nodes)[index], fmt::format("Nodes[{}]", index));
			if (!node)
			{
				return Error{node.GetError()};
			}
			library->m_Nodes.push_back(node.TakeValue());
		}

		// The vectors are final from here on: the indices view their strings.
		std::sort(library->m_Types.begin(), library->m_Types.end(),
		          [](VisualScriptTypeInfo const& a, VisualScriptTypeInfo const& b)
		          {
					  return a.ID < b.ID;
				  });
		for (size_t index = 0; index < library->m_Types.size(); ++index)
		{
			if (!library->m_TypeIndices.emplace(library->m_Types[index].ID, index).second)
			{
				return MakeError("Types: {} is described twice", library->m_Types[index].ID);
			}
		}
		for (size_t index = 0; index < library->m_Nodes.size(); ++index)
		{
			if (!library->m_NodeIndices.emplace(library->m_Nodes[index].Type, index).second)
			{
				return MakeError("Nodes: {} is described twice", library->m_Nodes[index].Type);
			}
			AddToCategories(library->m_Categories, library->m_Nodes[index]);
		}
		return library;
	}

	VisualScriptTypeInfo const* VisualScriptLibrary::FindType(std::string_view id) const
	{
		auto const found = m_TypeIndices.find(id);
		return found != m_TypeIndices.end() ? &m_Types[found->second] : nullptr;
	}

	VisualScriptNodeTemplate const* VisualScriptLibrary::FindNode(std::string_view type) const
	{
		auto const found = m_NodeIndices.find(type);
		return found != m_NodeIndices.end() ? &m_Nodes[found->second] : nullptr;
	}

	std::vector<VisualScriptNodeTemplate const*> VisualScriptLibrary::Search(std::string_view text) const
	{
		std::vector<VisualScriptNodeTemplate const*> found;
		for (VisualScriptNodeTemplate const& node : m_Nodes)
		{
			bool const matches = text.empty() || ContainsIgnoringCase(node.Title, text) || ContainsIgnoringCase(node.Category, text) ||
			                     ContainsIgnoringCase(node.Type, text) ||
			                     std::any_of(node.Keywords.begin(), node.Keywords.end(),
			                                 [text](std::string const& keyword)
			                                 {
												 return ContainsIgnoringCase(keyword, text);
											 });
			if (matches)
			{
				found.push_back(&node);
			}
		}
		return found;
	}

	bool VisualScriptLibrary::Satisfies(std::string_view type, VisualScriptPinConstraint constraint) const
	{
		bool const hasValues = type != VisualScriptExecType && type != VisualScriptWildcardType;
		VisualScriptTypeInfo const* info = hasValues ? FindType(type) : nullptr;
		switch (constraint)
		{
			case VisualScriptPinConstraint::None:
			case VisualScriptPinConstraint::Any:
				return hasValues;
			case VisualScriptPinConstraint::Numeric:
				return info != nullptr && (info->Kind == VisualScriptTypeKind::Integer || info->Kind == VisualScriptTypeKind::Float);
			case VisualScriptPinConstraint::Reference:
				return info != nullptr && (info->Kind == VisualScriptTypeKind::Entity || info->Kind == VisualScriptTypeKind::Component ||
				                           info->Kind == VisualScriptTypeKind::Asset || info->Kind == VisualScriptTypeKind::Class ||
				                           info->Kind == VisualScriptTypeKind::Array || info->Kind == VisualScriptTypeKind::Object);
			case VisualScriptPinConstraint::Array:
				return info != nullptr && info->Kind == VisualScriptTypeKind::Array;
		}
		return false;
	}

	bool VisualScriptLibrary::CanConnect(VisualScriptPin const& output, VisualScriptPin const& input) const
	{
		// The same type: exec to exec, and a wildcard to a wildcard.
		if (output.Type == input.Type)
		{
			return true;
		}
		bool const outputIsWildcard = output.Type == VisualScriptWildcardType;
		bool const inputIsWildcard = input.Type == VisualScriptWildcardType;
		// Exec connects only to exec, which a Reroute's wildcards pass on too.
		if (output.Type == VisualScriptExecType)
		{
			return inputIsWildcard && input.CarriesExec;
		}
		if (input.Type == VisualScriptExecType)
		{
			return outputIsWildcard && output.CarriesExec;
		}
		// A wildcard input takes what its constraint accepts (analysis resolves it from the link); a wildcard output goes to
		// an input of a type its constraint accepts, or to another wildcard.
		if (inputIsWildcard)
		{
			return outputIsWildcard || Satisfies(output.Type, input.Constraint);
		}
		if (outputIsWildcard)
		{
			return Satisfies(input.Type, output.Constraint);
		}
		// The conversions the library lists for the output's type, and any value to text.
		VisualScriptTypeInfo const* type = FindType(output.Type);
		if (type != nullptr && std::find(type->ImplicitTo.begin(), type->ImplicitTo.end(), input.Type) != type->ImplicitTo.end())
		{
			return true;
		}
		return input.Type == "string";
	}

	std::optional<VisualScriptEnumInfo> VisualScriptLibrary::FindEnum(std::string_view typeId) const
	{
		VisualScriptTypeInfo const* type = FindType(typeId);
		if (type == nullptr || type->Kind != VisualScriptTypeKind::Enum)
		{
			return std::nullopt;
		}
		return VisualScriptEnumInfo{type->FieldType, type->Enumerators, type->IsFlags};
	}

	VisualScriptAnalyzedNode const* VisualScriptAnalysis::FindNode(uint32_t id) const
	{
		auto const node = std::find_if(Nodes.begin(), Nodes.end(),
		                               [id](VisualScriptAnalyzedNode const& candidate)
		                               {
										   return candidate.ID == id;
									   });
		return node != Nodes.end() ? &*node : nullptr;
	}

	Result<VisualScriptAnalysis> VisualScriptAnalysis::Parse(std::string_view json)
	{
		Result<Json> parsed = ParseJson(json);
		if (!parsed)
		{
			return Error{parsed.GetError()};
		}
		VisualScriptAnalysis analysis;
		analysis.Source = parsed.TakeValue();
		Json const& document = analysis.Source;
		if (!document.is_object())
		{
			return Error{"the document: expected an object { \"Nodes\", \"Diagnostics\" }"};
		}
		auto const nodes = document.find("Nodes");
		auto const diagnostics = document.find("Diagnostics");
		if (nodes == document.end() || !nodes->is_array())
		{
			return Error{"Nodes: expected an array"};
		}
		if (diagnostics == document.end())
		{
			return Error{"Diagnostics: expected an array"};
		}
		analysis.Nodes.reserve(nodes->size());
		for (size_t index = 0; index < nodes->size(); ++index)
		{
			Result<VisualScriptAnalyzedNode> node = ReadAnalyzedNode((*nodes)[index], fmt::format("Nodes[{}]", index));
			if (!node)
			{
				return Error{node.GetError()};
			}
			analysis.Nodes.push_back(node.TakeValue());
		}
		Result<std::vector<VisualScriptDiagnostic>> read = ReadDiagnostics(*diagnostics, "Diagnostics");
		if (!read)
		{
			return Error{read.GetError()};
		}
		analysis.Diagnostics = read.TakeValue();
		return analysis;
	}

	Result<std::vector<VisualScriptDiagnostic>> ParseVisualScriptDiagnostics(std::string_view json)
	{
		Result<Json> parsed = ParseJson(json);
		if (!parsed)
		{
			return Error{parsed.GetError()};
		}
		return ReadDiagnostics(parsed.GetValue(), {});
	}

	size_t CountVisualScriptErrors(std::span<VisualScriptDiagnostic const> diagnostics)
	{
		return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
		                                         [](VisualScriptDiagnostic const& diagnostic)
		                                         {
													 return diagnostic.Severity == VisualScriptSeverity::Error;
												 }));
	}

	std::string DescribeVisualScriptErrors(std::span<VisualScriptDiagnostic const> diagnostics)
	{
		auto const first = std::find_if(diagnostics.begin(), diagnostics.end(),
		                                [](VisualScriptDiagnostic const& diagnostic)
		                                {
											return diagnostic.Severity == VisualScriptSeverity::Error;
										});
		if (first == diagnostics.end())
		{
			return {};
		}
		size_t const count = CountVisualScriptErrors(diagnostics);
		std::string location;
		if (first->Node)
		{
			location = fmt::format(" (node {})", *first->Node);
		}
		else if (first->Variable)
		{
			location = fmt::format(" (variable {})", *first->Variable);
		}
		return fmt::format("{} {}; first: {}{}", count, count == 1 ? "error" : "errors", first->Message, location);
	}
}
