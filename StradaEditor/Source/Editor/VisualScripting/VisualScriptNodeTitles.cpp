#include "Editor/VisualScripting/VisualScriptNodeTitles.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		// The prefixes of the type strings of the scripting API's members.
		constexpr std::string_view CallPrefix = "Call:";
		constexpr std::string_view GetPrefix = "Get:";
		constexpr std::string_view SetPrefix = "Set:";
		constexpr std::string_view NewPrefix = "New:";

		bool IsAsciiUpper(char character)
		{
			return character >= 'A' && character <= 'Z';
		}

		bool IsAsciiLower(char character)
		{
			return character >= 'a' && character <= 'z';
		}

		bool IsAsciiDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		// A name in words, as the scripting runtime titles the node library's types and members (Titles.Words in
		// Strada-ScriptCore/Source/VisualScripting/Library/Titles.cs). UI::FormatDisplayName, which titles component fields,
		// splits differently: it keeps "VSync" whole and drops the m_ prefix of script fields.
		std::string FormatWords(std::string_view name)
		{
			std::string words;
			words.reserve(name.size() + 8);
			for (size_t index = 0; index < name.size(); index++)
			{
				char const character = name[index];
				if (character == '_')
				{
					if (!words.empty() && words.back() != ' ')
					{
						words += ' ';
					}
					continue;
				}
				if (index > 0 && IsAsciiUpper(character) && !words.empty() && words.back() != ' ')
				{
					char const previous = name[index - 1];
					bool const afterWord = IsAsciiLower(previous) || IsAsciiDigit(previous);
					bool const endsAcronym = IsAsciiUpper(previous) && index + 1 < name.size() && IsAsciiLower(name[index + 1]);
					if (afterWord || endsAcronym)
					{
						words += ' ';
					}
				}
				words += character;
			}
			return words;
		}

		// A type string without its parameter list and type arguments: what the runtime looks members up by
		// ("Call:Strada.Entity.GetComponent" of "Call:Strada.Entity.GetComponent<T>()").
		std::string_view GetLookupKey(std::string_view type)
		{
			return type.substr(0, std::min(type.find('('), type.find('<')));
		}

		// The name a type string ends with: what follows its prefix and the last '.' of its lookup key ("IsKeyDown" of
		// "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", "ForLoop" of "Flow.ForLoop"), as the runtime titles the nodes of
		// types it does not know.
		std::string_view GetLastName(std::string_view type)
		{
			std::string_view name = GetLookupKey(type);
			if (size_t const colon = name.find(':'); colon != std::string_view::npos)
			{
				name.remove_prefix(colon + 1);
			}
			if (size_t const dot = name.rfind('.'); dot != std::string_view::npos && dot + 1 < name.size())
			{
				name.remove_prefix(dot + 1);
			}
			return name;
		}

		// The title the runtime gives a type, from its ID: the primitives by their C# names, arrays as "<element> Array" and
		// other types by their name in words without a "Component" suffix (Titles.OfType).
		std::string FormatTypeTitle(std::string_view id)
		{
			static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> Primitives = {{
				{"bool", "Bool"},
				{"int", "Int"},
				{"uint", "UInt"},
				{"long", "Long"},
				{"ulong", "ULong"},
				{"float", "Float"},
				{"double", "Double"},
				{"string", "String"},
			}};
			constexpr std::string_view ArraySuffix = "[]";
			constexpr std::string_view ComponentSuffix = " Component";

			size_t arrays = 0;
			while (id.size() > ArraySuffix.size() && id.ends_with(ArraySuffix))
			{
				id.remove_suffix(ArraySuffix.size());
				arrays++;
			}
			std::string title;
			auto const primitive = std::find_if(Primitives.begin(), Primitives.end(),
			                                    [id](std::pair<std::string_view, std::string_view> const& entry)
			                                    {
													return entry.first == id;
												});
			if (primitive != Primitives.end())
			{
				title = primitive->second;
			}
			else
			{
				// Type IDs are full names, and a nested type's name follows a '+'.
				size_t const separator = id.find_last_of(".+");
				title = FormatWords(separator != std::string_view::npos ? id.substr(separator + 1) : id);
				if (title.size() > ComponentSuffix.size() && title.ends_with(ComponentSuffix))
				{
					title.resize(title.size() - ComponentSuffix.size());
				}
			}
			for (size_t array = 0; array < arrays; array++)
			{
				title += " Array";
			}
			return title;
		}

		// A setting that holds an ID (a node's Variable or Function): an integer the runtime reads as 32 bits without sign.
		std::optional<uint32_t> ReadId(Json const& settings, char const* name)
		{
			if (!settings.is_object())
			{
				return std::nullopt;
			}
			auto const found = settings.find(name);
			if (found == settings.end())
			{
				return std::nullopt;
			}
			constexpr uint64_t MaxId = std::numeric_limits<uint32_t>::max();
			if (found->is_number_unsigned())
			{
				uint64_t const id = found->get<uint64_t>();
				return id <= MaxId ? std::optional<uint32_t>(static_cast<uint32_t>(id)) : std::nullopt;
			}
			if (found->is_number_integer())
			{
				int64_t const id = found->get<int64_t>();
				return id >= 0 && static_cast<uint64_t>(id) <= MaxId ? std::optional<uint32_t>(static_cast<uint32_t>(id)) : std::nullopt;
			}
			return std::nullopt;
		}

		// The custom event's name a node's Name setting holds; nothing when it holds no valid name (the runtime applies the
		// format's name rule to custom events' names).
		std::optional<std::string_view> ReadEventName(Json const& settings)
		{
			if (!settings.is_object())
			{
				return std::nullopt;
			}
			auto const found = settings.find("Name");
			if (found == settings.end() || !found->is_string())
			{
				return std::nullopt;
			}
			std::string const& name = found->get_ref<std::string const&>();
			return VisualScriptSerializer::IsValidName(name) ? std::optional<std::string_view>(name) : std::nullopt;
		}

		// The name of what a Variable.Get/Set node reads or writes, as the runtime resolves its Variable setting: a local of
		// the function whose graph holds the node, else a variable of the document. Null when it names neither.
		std::string const* FindVariableName(VisualScriptNode const& node, VisualScriptDocument const& document)
		{
			std::optional<uint32_t> const id = ReadId(node.Settings, "Variable");
			if (!id)
			{
				return nullptr;
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				auto const local = std::find_if(function.Locals.begin(), function.Locals.end(),
				                                [&](VisualScriptLocal const& candidate)
				                                {
													return candidate.ID == *id;
												});
				bool const inGraph = local != function.Locals.end() && std::any_of(function.Graph.Nodes.begin(), function.Graph.Nodes.end(),
				                                                                   [&](VisualScriptNode const& candidate)
				                                                                   {
																					   return candidate.ID == node.ID;
																				   });
				if (inGraph)
				{
					return &local->Name;
				}
			}
			auto const variable = std::find_if(document.Variables.begin(), document.Variables.end(),
			                                   [&](VisualScriptVariable const& candidate)
			                                   {
												   return candidate.ID == *id;
											   });
			return variable != document.Variables.end() ? &variable->Name : nullptr;
		}

		// The name of the function a Function.Call node calls; null when its Function setting names none.
		std::string const* FindFunctionName(VisualScriptNode const& node, VisualScriptDocument const& document)
		{
			std::optional<uint32_t> const id = ReadId(node.Settings, "Function");
			if (!id)
			{
				return nullptr;
			}
			auto const function = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                   [&](VisualScriptFunction const& candidate)
			                                   {
												   return candidate.ID == *id;
											   });
			return function != document.Functions.end() ? &function->Name : nullptr;
		}

		// "<prefix><name in words>", or the type itself when it ends with no name (as the runtime titles such types).
		std::string FormatMemberTitle(std::string_view prefix, std::string_view type)
		{
			std::string_view const name = GetLastName(type);
			return name.empty() ? std::string(type) : std::string(prefix) + FormatWords(name);
		}
	}

	std::string DeriveVisualScriptNodeTitle(VisualScriptNode const& node, VisualScriptDocument const& document)
	{
		std::string_view const type = node.Type;
		if (type == "Variable.Get" || type == "Variable.Set")
		{
			std::string const* const name = FindVariableName(node, document);
			return std::string(type == "Variable.Get" ? "Get " : "Set ") + (name != nullptr ? *name : std::string("Variable"));
		}
		if (type == "Function.Call")
		{
			std::string const* const name = FindFunctionName(node, document);
			return name != nullptr ? *name : std::string("Call Function");
		}
		if (type == "Event.Custom")
		{
			std::optional<std::string_view> const name = ReadEventName(node.Settings);
			return name ? std::string(*name) : std::string("Custom Event");
		}
		if (type == "Event.Send")
		{
			std::optional<std::string_view> const name = ReadEventName(node.Settings);
			return "Send " + (name ? std::string(*name) : std::string("Event"));
		}
		if (type.starts_with(CallPrefix))
		{
			return FormatMemberTitle({}, type);
		}
		if (type.starts_with(GetPrefix))
		{
			return FormatMemberTitle("Get ", type);
		}
		if (type.starts_with(SetPrefix))
		{
			return FormatMemberTitle("Set ", type);
		}
		if (type.starts_with(NewPrefix))
		{
			std::string const declaring = FormatTypeTitle(GetLookupKey(type.substr(NewPrefix.size())));
			return declaring.empty() ? std::string(type) : "Make " + declaring;
		}
		return FormatMemberTitle({}, type);
	}
}
