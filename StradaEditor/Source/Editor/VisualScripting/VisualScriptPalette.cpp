#include "Editor/VisualScripting/VisualScriptPalette.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view EntryType = "Function.Entry";
		constexpr std::string_view ReturnType = "Function.Return";
		constexpr std::string_view CustomEventType = "Event.Custom";
		constexpr std::string_view SendEventType = "Event.Send";
		constexpr std::string_view CallType = "Function.Call";
		constexpr std::string_view GetVariableType = "Variable.Get";
		constexpr std::string_view SetVariableType = "Variable.Set";
		constexpr std::string_view EventPrefix = "Event.";
		constexpr std::string_view TypeArgumentsSetting = "TypeArguments";
		constexpr std::string_view EntityType = "Strada.Entity";
		constexpr std::string_view StringType = "string";

		// The generic kinds whose wildcards only their settings type (a variable's type, a Literal's, a Convert's target, a
		// struct, a Switch's selection): such a pin would accept every type, so context filtering never connects to it and
		// lists the kinds' typed expansions instead. Wildcards with meaning of their own (operators, Select, IsValid, arrays,
		// Reroute, Convert's Value) stay.
		struct SettingsTypedPin
		{
			std::string_view Node;
			std::string_view Pin;
			VisualScriptPinDirection Direction;
		};
		constexpr std::array<SettingsTypedPin, 7> SettingsTypedPins = {{
			{"Variable.Get", "Value", VisualScriptPinDirection::Out},
			{"Variable.Set", "Value", VisualScriptPinDirection::In},
			{"Variable.Set", "Value", VisualScriptPinDirection::Out},
			{"Literal", "Value", VisualScriptPinDirection::Out},
			{"Convert", "Result", VisualScriptPinDirection::Out},
			{"Make", "Value", VisualScriptPinDirection::Out},
			{"Break", "Value", VisualScriptPinDirection::In},
		}};
		constexpr std::string_view SwitchType = "Flow.Switch";
		constexpr std::string_view LiteralType = "Literal";
		constexpr std::string_view ConvertType = "Convert";
		constexpr std::string_view MakeType = "Make";
		constexpr std::string_view BreakType = "Break";

		// The kinds of types a Literal is offered for, one entry each.
		constexpr std::array<VisualScriptTypeKind, 7> LiteralKinds = {
			VisualScriptTypeKind::Bool,   VisualScriptTypeKind::Integer,    VisualScriptTypeKind::Float, VisualScriptTypeKind::String,
			VisualScriptTypeKind::Vector, VisualScriptTypeKind::Quaternion, VisualScriptTypeKind::Color};

		constexpr size_t MaxQueryWords = 8;
		constexpr size_t MaxRecentEntries = 20;
		constexpr size_t MinSubsequenceLength = 3;
		// Longer words are not looked for as subsequences: no title holds them scattered.
		constexpr size_t MaxSubsequenceLength = 64;
		// Search texts are indexed by uint16_t.
		constexpr size_t MaxSearchedBytes = std::numeric_limits<uint16_t>::max();

		constexpr int32_t WholeTitleScore = 1000;
		constexpr int32_t TitlePrefixScore = 700;
		constexpr int32_t TitleWordScore = 600;
		constexpr int32_t InitialsScore = 500;
		constexpr int32_t TitleContainsScore = 400;
		constexpr int32_t KeywordPrefixScore = 350;
		constexpr int32_t KeywordContainsScore = 300;
		constexpr int32_t SubtitleWordScore = 250;
		constexpr int32_t CategoryWordScore = 200;
		constexpr int32_t TypeContainsScore = 150;
		constexpr int32_t SubsequenceScore = 100;
		constexpr int32_t SubsequenceGapCost = 5;
		constexpr int32_t MinSubsequenceScore = 20;
		constexpr int32_t RecentScore = 300;
		constexpr int32_t RecentPlaceCost = 15;
		constexpr int32_t ExactPinScore = 250;
		constexpr int32_t ConvertingPinScore = 100;
		constexpr int32_t DocumentScore = 50;
		constexpr int32_t TitleByteCost = 2;

		char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		std::string ToLowerAscii(std::string_view text)
		{
			std::string lower(text.substr(0, std::min(text.size(), MaxSearchedBytes)));
			for (char& character : lower)
			{
				character = ToLowerAscii(character);
			}
			return lower;
		}

		// Words are runs of letters and digits; bytes of other scripts (UTF-8 sequences) belong to words too.
		bool IsWordByte(char character)
		{
			unsigned char const byte = static_cast<unsigned char>(character);
			return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte >= 0x80;
		}

		std::vector<uint16_t> FindWordStarts(std::string_view text)
		{
			std::vector<uint16_t> starts;
			for (size_t index = 0; index < text.size(); index++)
			{
				if (IsWordByte(text[index]) && (index == 0 || !IsWordByte(text[index - 1])))
				{
					starts.push_back(static_cast<uint16_t>(index));
				}
			}
			return starts;
		}

		// Every reflected node's type and every type of the scripting API holds it: words that only match it would match them all.
		std::string WithoutApiNamespace(std::string text)
		{
			constexpr std::string_view Namespace = "strada.";
			for (size_t found = text.find(Namespace); found != std::string::npos; found = text.find(Namespace, found))
			{
				text.erase(found, Namespace.size());
			}
			return text;
		}

		uint64_t MaskCharacters(std::string_view text)
		{
			uint64_t mask = 0;
			for (char const character : text)
			{
				mask |= uint64_t(1) << (static_cast<unsigned char>(character) % 64);
			}
			return mask;
		}

		bool IsWhitespace(char character)
		{
			return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
			       character == '\v';
		}

		std::string CompactJson(Json const& json)
		{
			return json.dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		bool IsWildcard(VisualScriptPin const& pin)
		{
			return pin.Type == VisualScriptWildcardType;
		}

		VisualScriptPin MakePin(std::string name, VisualScriptPinDirection direction, std::string type)
		{
			VisualScriptPin pin;
			pin.Name = std::move(name);
			pin.Direction = direction;
			pin.Type = std::move(type);
			return pin;
		}

		VisualScriptPin ExecPin(std::string name, VisualScriptPinDirection direction)
		{
			return MakePin(std::move(name), direction, std::string(VisualScriptExecType));
		}

		// The template's pins with its wildcards typed: what a typed expansion of a generic kind has.
		std::vector<VisualScriptPin> TypeWildcards(std::vector<VisualScriptPin> pins, std::string const& type)
		{
			for (VisualScriptPin& pin : pins)
			{
				if (IsWildcard(pin))
				{
					pin.Type = type;
					pin.Constraint = VisualScriptPinConstraint::None;
					pin.Group.clear();
					pin.ElementOf.clear();
					pin.CarriesExec = false;
				}
			}
			return pins;
		}

		bool IsSettingsTyped(VisualScriptNodeTemplate const& node, VisualScriptPin const& pin)
		{
			if (!IsWildcard(pin))
			{
				return false;
			}
			bool const typeArguments = std::any_of(node.Settings.begin(), node.Settings.end(),
			                                       [](VisualScriptSetting const& setting)
			                                       {
													   return setting.Name == TypeArgumentsSetting;
												   });
			if (typeArguments)
			{
				return true;
			}
			if (node.Type == SwitchType)
			{
				return pin.Direction == VisualScriptPinDirection::In && pin.Name == "Selection";
			}
			return std::any_of(SettingsTypedPins.begin(), SettingsTypedPins.end(),
			                   [&](SettingsTypedPin const& typed)
			                   {
								   return typed.Node == node.Type && typed.Pin == pin.Name && typed.Direction == pin.Direction;
							   });
		}

		bool IsNumericType(VisualScriptLibrary const& library, std::string_view type)
		{
			VisualScriptTypeInfo const* info = library.FindType(type);
			return info != nullptr && (info->Kind == VisualScriptTypeKind::Integer || info->Kind == VisualScriptTypeKind::Float);
		}

		std::string DescribeParameters(std::vector<VisualScriptParameter> const& parameters,
		                               std::function<std::string(std::string_view)> const& titleOfType)
		{
			std::string text;
			for (VisualScriptParameter const& parameter : parameters)
			{
				text += text.empty() ? "" : ", ";
				text += titleOfType(parameter.Type);
				text += ' ';
				text += parameter.Name;
			}
			return text;
		}

		// The Parameters setting of a custom event: [{ "Name", "Type" }] with names and types as strings; nothing when it is
		// not of that form.
		std::optional<std::vector<VisualScriptParameter>> ReadEventParameters(Json const& settings)
		{
			std::vector<VisualScriptParameter> parameters;
			auto const stored = settings.find("Parameters");
			if (stored == settings.end())
			{
				return parameters;
			}
			if (!stored->is_array())
			{
				return std::nullopt;
			}
			for (Json const& parameter : *stored)
			{
				if (!parameter.is_object())
				{
					return std::nullopt;
				}
				auto const name = parameter.find("Name");
				auto const type = parameter.find("Type");
				if (name == parameter.end() || type == parameter.end() || !name->is_string() || !type->is_string())
				{
					return std::nullopt;
				}
				parameters.push_back({name->get<std::string>(), type->get<std::string>()});
			}
			return parameters;
		}

		// Whether a function's graph holds a node of the type.
		bool HasNodeOfType(VisualScriptGraph const& graph, std::string_view type)
		{
			return std::any_of(graph.Nodes.begin(), graph.Nodes.end(),
			                   [type](VisualScriptNode const& node)
			                   {
								   return node.Type == type;
							   });
		}

		// The best place in the title where a word's characters appear in order, with the fewest gaps between them (a gap is
		// a run of title characters between two matched ones). Returns the number of gaps and fills the matched indices; -1
		// when the title does not hold them in order.
		int32_t FindSubsequence(std::string_view title, std::string_view word, std::vector<uint16_t>& matched, std::vector<int32_t>& costs,
		                        std::vector<int32_t>& previous)
		{
			size_t const titleLength = title.size();
			size_t const wordLength = word.size();
			if (wordLength == 0 || wordLength > titleLength)
			{
				return -1;
			}
			constexpr int32_t Unreachable = std::numeric_limits<int32_t>::max() / 2;
			costs.assign(titleLength * wordLength, Unreachable);
			previous.assign(titleLength * wordLength, -1);
			for (size_t position = 0; position < titleLength; position++)
			{
				if (title[position] == word[0])
				{
					costs[position] = 0;
				}
			}
			for (size_t character = 1; character < wordLength; character++)
			{
				int32_t const* const before = &costs[(character - 1) * titleLength];
				int32_t* const current = &costs[character * titleLength];
				int32_t* const from = &previous[character * titleLength];
				// The cheapest match of the previous character at least two places back, which a gap separates.
				int32_t bestBefore = Unreachable;
				int32_t bestBeforePosition = -1;
				for (size_t position = 1; position < titleLength; position++)
				{
					if (position >= 2 && before[position - 2] < bestBefore)
					{
						bestBefore = before[position - 2];
						bestBeforePosition = static_cast<int32_t>(position - 2);
					}
					if (title[position] != word[character])
					{
						continue;
					}
					if (before[position - 1] <= bestBefore + 1 && before[position - 1] < Unreachable)
					{
						current[position] = before[position - 1];
						from[position] = static_cast<int32_t>(position - 1);
					}
					else if (bestBefore < Unreachable)
					{
						current[position] = bestBefore + 1;
						from[position] = bestBeforePosition;
					}
				}
			}
			int32_t const* const last = &costs[(wordLength - 1) * titleLength];
			int32_t best = Unreachable;
			int32_t bestPosition = -1;
			for (size_t position = 0; position < titleLength; position++)
			{
				if (last[position] < best)
				{
					best = last[position];
					bestPosition = static_cast<int32_t>(position);
				}
			}
			if (bestPosition < 0)
			{
				return -1;
			}
			size_t const firstMatched = matched.size();
			int32_t position = bestPosition;
			for (size_t character = wordLength; character-- > 0;)
			{
				matched.push_back(static_cast<uint16_t>(position));
				position = previous[character * titleLength + static_cast<size_t>(position)];
			}
			std::reverse(matched.begin() + static_cast<std::ptrdiff_t>(firstMatched), matched.end());
			return best;
		}

		// A keyword that starts with the word scores more than one that only holds it.
		int32_t ScoreKeywords(std::span<std::string const> keywords, std::string_view word)
		{
			int32_t best = 0;
			for (std::string const& keyword : keywords)
			{
				if (keyword.starts_with(word))
				{
					return KeywordPrefixScore;
				}
				if (keyword.find(word) != std::string::npos)
				{
					best = KeywordContainsScore;
				}
			}
			return best;
		}

		bool StartsWordWith(std::string_view text, std::span<uint16_t const> wordStarts, std::string_view word)
		{
			return std::any_of(wordStarts.begin(), wordStarts.end(),
			                   [&](uint16_t start)
			                   {
								   return text.substr(start).starts_with(word);
							   });
		}

		// How an entry's pin connects to the dragged pin.
		struct ContextFit
		{
			bool Fits = false;
			// 2: exactly the dragged type; 1: through an implicit conversion; 0: a wildcard takes it.
			int Quality = 0;
			std::string Pin;
		};
	}

	std::string MakeVisualScriptRecentKey(std::string_view type, Json const& settings)
	{
		if (type.starts_with(VisualScriptPaletteActionKeyPrefix))
		{
			return std::string(type);
		}
		return std::string(type) + CompactJson(settings);
	}

	void VisualScriptPaletteIndex::Rebuild(VisualScriptLibrary const& library)
	{
		m_LibraryEntries.clear();
		m_TypeTitles.clear();
		m_EventSettings.clear();
		m_ReroutePins.clear();
		m_CustomEventPins.clear();

		std::unordered_set<std::string_view> bases;
		for (VisualScriptTypeInfo const& type : library.GetTypes())
		{
			m_TypeTitles.emplace(type.ID, type.Title);
			if (!type.Base.empty())
			{
				bases.insert(type.Base);
			}
		}

		auto const add = [this](VisualScriptPaletteEntry entry, EntryFacts facts)
		{
			if (entry.RecentKey.empty())
			{
				entry.RecentKey = entry.NodeType + CompactJson(entry.Settings);
			}
			facts.SettingsDependent.resize(entry.Pins.size(), false);
			IndexedEntry indexed{std::move(entry), std::move(facts), {}};
			m_LibraryEntries.push_back(std::move(indexed));
		};

		for (VisualScriptNodeTemplate const& node : library.GetNodes())
		{
			if (node.Kind == VisualScriptNodeKind::Reroute && m_ReroutePins.empty())
			{
				m_ReroutePins = node.Pins;
			}
			if (node.Type == CustomEventType)
			{
				m_CustomEventPins = node.Pins;
			}

			EntryFacts facts;
			facts.Pure = node.Pure;
			facts.Event = node.Kind == VisualScriptNodeKind::Event;
			facts.Ticks = std::any_of(node.Pins.begin(), node.Pins.end(),
			                          [](VisualScriptPin const& pin)
			                          {
										  return pin.Role == VisualScriptPinRole::Tick;
									  });
			facts.Entry = node.Type == EntryType;
			facts.Return = node.Type == ReturnType;
			// Events without settings are the standard ones: one node each per script.
			facts.Unique = facts.Event && node.Settings.empty();

			VisualScriptPaletteEntry entry;
			entry.Title = node.Title;
			entry.Subtitle = node.Subtitle;
			entry.Category = node.Category;
			entry.NodeType = node.Type;
			entry.Description = node.Description;
			entry.Keywords = node.Keywords;
			entry.Kind = node.Kind;
			entry.Pins = node.Pins;
			for (VisualScriptPin const& pin : node.Pins)
			{
				facts.SettingsDependent.push_back(IsSettingsTyped(node, pin));
			}

			// The expansions share the kind's facts, except what makes them typed.
			EntryFacts expansionFacts = facts;
			expansionFacts.SettingsDependent.clear();
			expansionFacts.Unique = false;
			auto const expand = [&](std::string title, Json settings, std::vector<VisualScriptPin> pins, std::vector<std::string> keywords)
			{
				VisualScriptPaletteEntry expansion;
				expansion.Title = std::move(title);
				expansion.Subtitle = node.Subtitle;
				expansion.Category = node.Category;
				expansion.NodeType = node.Type;
				expansion.Description = node.Description;
				expansion.Keywords = node.Keywords;
				expansion.Keywords.insert(expansion.Keywords.end(), keywords.begin(), keywords.end());
				expansion.Settings = std::move(settings);
				expansion.Kind = node.Kind;
				expansion.Pins = std::move(pins);
				add(std::move(expansion), expansionFacts);
			};

			add(std::move(entry), facts);

			// Input events: one per key or button, each unique.
			if (facts.Event)
			{
				for (VisualScriptSetting const& setting : node.Settings)
				{
					std::optional<VisualScriptEnumInfo> const enumInfo =
						setting.Kind == "Enum" ? library.FindEnum(setting.Type) : std::nullopt;
					if (!enumInfo.has_value())
					{
						continue;
					}
					m_EventSettings.emplace(node.Type, EventSetting{setting.Name, *enumInfo});
					for (ScriptEnumerator const& enumerator : enumInfo->Enumerators)
					{
						std::optional<Json> const canonical = EncodeVisualScriptEnumLiteral(enumerator.Value, *enumInfo);
						Json settings = Json::object();
						settings[setting.Name] = enumerator.Name;
						VisualScriptPaletteEntry expansion;
						expansion.Title = fmt::format("{}: {}", node.Title, enumerator.Name);
						expansion.Subtitle = node.Subtitle;
						expansion.Category = node.Category;
						expansion.NodeType = node.Type;
						expansion.Description = node.Description;
						expansion.Keywords = node.Keywords;
						expansion.Settings = std::move(settings);
						expansion.Kind = node.Kind;
						expansion.Pins = node.Pins;
						EntryFacts eventFacts = expansionFacts;
						eventFacts.Unique = true;
						eventFacts.UniqueSetting = setting.Name;
						eventFacts.UniqueValue = CompactJson(canonical.value_or(Json(enumerator.Name)));
						add(std::move(expansion), std::move(eventFacts));
					}
					break;
				}
			}

			// Generic calls: one per component or asset class the type argument can be. The abstract bases (Strada.Component,
			// Strada.Asset) take none: the scripting runtime accepts only the classes of real components and assets.
			for (VisualScriptSetting const& setting : node.Settings)
			{
				if (setting.Name != TypeArgumentsSetting || (setting.Constraint != "Component" && setting.Constraint != "Asset"))
				{
					continue;
				}
				VisualScriptTypeKind const kind =
					setting.Constraint == "Component" ? VisualScriptTypeKind::Component : VisualScriptTypeKind::Asset;
				for (VisualScriptTypeInfo const& type : library.GetTypes())
				{
					if (type.Kind != kind || bases.contains(type.ID))
					{
						continue;
					}
					Json settings = Json::object();
					settings[std::string(TypeArgumentsSetting)] = Json::array({type.ID});
					expand(fmt::format("{}: {}", node.Title, type.Title), std::move(settings), TypeWildcards(node.Pins, type.ID),
					       {type.ID});
				}
			}

			if (node.Type == MakeType || node.Type == BreakType)
			{
				bool const make = node.Type == MakeType;
				for (VisualScriptTypeInfo const& type : library.GetTypes())
				{
					if (!(make ? type.Makeable : type.Breakable))
					{
						continue;
					}
					std::vector<VisualScriptPin> pins;
					if (!make)
					{
						pins.push_back(MakePin("Value", VisualScriptPinDirection::In, type.ID));
					}
					for (VisualScriptStructField const& field : type.Fields)
					{
						pins.push_back(
							MakePin(field.Name, make ? VisualScriptPinDirection::In : VisualScriptPinDirection::Out, field.Type));
					}
					if (make)
					{
						pins.push_back(MakePin("Value", VisualScriptPinDirection::Out, type.ID));
					}
					expand(fmt::format("{} {}", make ? "Make" : "Break", type.Title), Json::object({{"Type", type.ID}}), std::move(pins),
					       {type.ID});
				}
			}
			else if (node.Type == SwitchType)
			{
				for (VisualScriptTypeInfo const& type : library.GetTypes())
				{
					if (type.ID != "int" && type.ID != StringType && type.Kind != VisualScriptTypeKind::Enum)
					{
						continue;
					}
					expand(fmt::format("{} on {}", node.Title, type.Title), Json::object({{"Type", type.ID}}),
					       TypeWildcards(node.Pins, type.ID), {type.ID});
				}
			}
			else if (node.Type == LiteralType)
			{
				for (VisualScriptTypeInfo const& type : library.GetTypes())
				{
					if (!type.HasLiteral || std::find(LiteralKinds.begin(), LiteralKinds.end(), type.Kind) == LiteralKinds.end())
					{
						continue;
					}
					expand(fmt::format("{} Literal", type.Title), Json::object({{"Type", type.ID}}), TypeWildcards(node.Pins, type.ID),
					       {type.ID});
				}
			}
			else if (node.Type == ConvertType)
			{
				// The conversions every value has (to text) and numbers have (to each other), which context mode offers.
				for (VisualScriptTypeInfo const& type : library.GetTypes())
				{
					if (type.ID != StringType && type.Kind != VisualScriptTypeKind::Integer && type.Kind != VisualScriptTypeKind::Float)
					{
						continue;
					}
					std::vector<VisualScriptPin> pins = node.Pins;
					for (VisualScriptPin& pin : pins)
					{
						if (pin.Direction == VisualScriptPinDirection::Out && IsWildcard(pin))
						{
							pin = MakePin(pin.Name, pin.Direction, type.ID);
						}
					}
					EntryFacts convertFacts = expansionFacts;
					convertFacts.ConvertTo = type.ID;
					VisualScriptPaletteEntry expansion;
					expansion.Title = fmt::format("To {}", type.Title);
					expansion.Subtitle = node.Subtitle;
					expansion.Category = node.Category;
					expansion.NodeType = node.Type;
					expansion.Description = node.Description;
					expansion.Keywords = node.Keywords;
					expansion.Keywords.push_back(type.ID);
					expansion.Settings = Json::object({{"To", type.ID}});
					expansion.Kind = node.Kind;
					expansion.Pins = std::move(pins);
					add(std::move(expansion), std::move(convertFacts));
				}
			}
		}
		for (IndexedEntry& indexed : m_LibraryEntries)
		{
			indexed.Key = MakeKey(indexed.Entry);
		}
		Compose();
	}

	void VisualScriptPaletteIndex::SetDocument(VisualScriptDocument const& document, uint32_t function)
	{
		DocumentFacts facts;
		auto const shown = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                [function](VisualScriptFunction const& candidate)
		                                {
											return candidate.ID == function;
										});
		if (function != 0 && shown != document.Functions.end())
		{
			facts.Function = function;
			facts.Pure = shown->Pure;
			facts.HasEntry = HasNodeOfType(shown->Graph, EntryType);
			facts.HasReturn = HasNodeOfType(shown->Graph, ReturnType);
			facts.Locals = shown->Locals;
		}
		facts.Variables = document.Variables;
		for (VisualScriptFunction const& candidate : document.Functions)
		{
			facts.Functions.push_back({candidate.ID, candidate.Name, candidate.Pure, candidate.Inputs, candidate.Outputs});
		}
		std::unordered_set<std::string> eventNames;
		for (VisualScriptNode const& node : document.Graph.Nodes)
		{
			if (!node.Type.starts_with(EventPrefix) || !node.Settings.is_object())
			{
				continue;
			}
			if (facts.Function == 0)
			{
				facts.EventNodes.push_back({node.ID, node.Type, node.Settings});
			}
			if (node.Type != CustomEventType)
			{
				continue;
			}
			auto const name = node.Settings.find("Name");
			if (name == node.Settings.end() || !name->is_string() || !VisualScriptSerializer::IsValidName(name->get<std::string>()) ||
			    !eventNames.insert(name->get<std::string>()).second)
			{
				continue;
			}
			DocumentFacts::CustomEvent event;
			event.Name = name->get<std::string>();
			if (std::optional<std::vector<VisualScriptParameter>> parameters = ReadEventParameters(node.Settings))
			{
				event.ParameterList = std::move(*parameters);
				if (auto const stored = node.Settings.find("Parameters"); stored != node.Settings.end())
				{
					event.Parameters = *stored;
				}
			}
			facts.CustomEvents.push_back(std::move(event));
		}
		if (facts == m_Document && !m_Entries.empty())
		{
			return;
		}
		m_Document = std::move(facts);
		Compose();
	}

	std::string VisualScriptPaletteIndex::TitleOfType(std::string_view type) const
	{
		auto const found = m_TypeTitles.find(std::string(type));
		return found != m_TypeTitles.end() ? found->second : std::string(type);
	}

	std::string VisualScriptPaletteIndex::CanonicalEventValue(std::string const& type, Json const& value) const
	{
		auto const setting = m_EventSettings.find(type);
		if (setting == m_EventSettings.end())
		{
			return CompactJson(value);
		}
		std::optional<ScriptFieldValue> const decoded = DecodeVisualScriptEnumLiteral(value, setting->second.Enum);
		if (!decoded.has_value())
		{
			return {};
		}
		std::optional<Json> const canonical = EncodeVisualScriptEnumLiteral(*decoded, setting->second.Enum);
		return canonical.has_value() ? CompactJson(*canonical) : std::string();
	}

	bool VisualScriptPaletteIndex::IsShown(EntryFacts const& facts) const
	{
		if (m_Document.Function == 0)
		{
			return !facts.Entry && !facts.Return;
		}
		if (facts.Event || facts.Ticks)
		{
			return false;
		}
		if (facts.Entry)
		{
			return !m_Document.HasEntry;
		}
		if (facts.Return)
		{
			return !m_Document.Pure || !m_Document.HasReturn;
		}
		return !m_Document.Pure || facts.Pure;
	}

	VisualScriptPaletteIndex::SearchKey VisualScriptPaletteIndex::MakeKey(VisualScriptPaletteEntry const& entry)
	{
		SearchKey key;
		key.Title = ToLowerAscii(entry.Title);
		key.TitleWords = FindWordStarts(key.Title);
		for (uint16_t const start : key.TitleWords)
		{
			key.Initials += key.Title[start];
		}
		key.Subtitle = ToLowerAscii(entry.Subtitle);
		key.SubtitleWords = FindWordStarts(key.Subtitle);
		key.Category = ToLowerAscii(entry.Category);
		key.CategoryWords = FindWordStarts(key.Category);
		for (std::string const& keyword : entry.Keywords)
		{
			key.Keywords.push_back(WithoutApiNamespace(ToLowerAscii(keyword)));
		}
		key.NodeType = WithoutApiNamespace(ToLowerAscii(entry.NodeType));
		key.Characters =
			MaskCharacters(key.Title) | MaskCharacters(key.Subtitle) | MaskCharacters(key.Category) | MaskCharacters(key.NodeType);
		for (std::string const& keyword : key.Keywords)
		{
			key.Characters |= MaskCharacters(keyword);
		}
		return key;
	}

	void VisualScriptPaletteIndex::Add(VisualScriptPaletteEntry entry, EntryFacts facts)
	{
		if (entry.RecentKey.empty())
		{
			entry.RecentKey = entry.NodeType + CompactJson(entry.Settings);
		}
		facts.SettingsDependent.resize(entry.Pins.size(), false);
		SearchKey key = MakeKey(entry);
		m_Entries.push_back(std::move(entry));
		m_Keys.push_back(std::move(key));
		m_Facts.push_back(std::move(facts));
	}

	void VisualScriptPaletteIndex::Compose()
	{
		m_Entries.clear();
		m_Keys.clear();
		m_Facts.clear();

		// The unique events the event graph handles, by type and the canonical value of their setting; the first node wins.
		std::unordered_map<std::string, uint32_t> existing;
		for (EventNode const& node : m_Document.EventNodes)
		{
			std::string key = node.Type;
			if (auto const setting = m_EventSettings.find(node.Type); setting != m_EventSettings.end())
			{
				auto const value = node.Settings.find(setting->second.Name);
				if (value == node.Settings.end())
				{
					continue;
				}
				std::string const canonical = CanonicalEventValue(node.Type, *value);
				if (canonical.empty())
				{
					continue;
				}
				key += '\n';
				key += canonical;
			}
			existing.emplace(std::move(key), node.ID);
		}

		for (IndexedEntry const& indexed : m_LibraryEntries)
		{
			if (!IsShown(indexed.Facts))
			{
				continue;
			}
			uint32_t existingNode = 0;
			if (indexed.Facts.Unique)
			{
				std::string key = indexed.Entry.NodeType;
				if (!indexed.Facts.UniqueSetting.empty())
				{
					key += '\n';
					key += indexed.Facts.UniqueValue;
				}
				if (auto const found = existing.find(key); found != existing.end())
				{
					existingNode = found->second;
				}
			}
			if (existingNode == 0)
			{
				m_Entries.push_back(indexed.Entry);
				m_Keys.push_back(indexed.Key);
				m_Facts.push_back(indexed.Facts);
				continue;
			}
			VisualScriptPaletteEntry goTo = indexed.Entry;
			goTo.Title += " (go to)";
			goTo.Action = VisualScriptPaletteAction::GoToNode;
			goTo.ExistingNode = existingNode;
			Add(std::move(goTo), indexed.Facts);
		}
		AddDocumentEntries();
		AddActions();
	}

	void VisualScriptPaletteIndex::AddDocumentEntries()
	{
		auto const titleOfType = [this](std::string_view type)
		{
			return TitleOfType(type);
		};
		auto const addVariable =
			[&](uint32_t id, std::string const& name, std::string const& type, std::string const& subtitle, std::string const& description)
		{
			Json const settings = Json::object({{"Variable", id}});
			for (bool const set : {false, true})
			{
				EntryFacts facts;
				facts.Pure = !set;
				if (!IsShown(facts))
				{
					continue;
				}
				VisualScriptPaletteEntry entry;
				entry.Title = fmt::format("{} {}", set ? "Set" : "Get", name);
				entry.Subtitle = subtitle;
				entry.Category = "Variables";
				entry.NodeType = std::string(set ? SetVariableType : GetVariableType);
				entry.Description = description;
				entry.Settings = settings;
				entry.Kind = VisualScriptNodeKind::Variable;
				if (set)
				{
					entry.Pins.push_back(ExecPin("Execute", VisualScriptPinDirection::In));
					entry.Pins.push_back(MakePin("Value", VisualScriptPinDirection::In, type));
					entry.Pins.push_back(ExecPin("Then", VisualScriptPinDirection::Out));
				}
				entry.Pins.push_back(MakePin("Value", VisualScriptPinDirection::Out, type));
				entry.FromDocument = true;
				Add(std::move(entry), std::move(facts));
			}
		};
		for (VisualScriptVariable const& variable : m_Document.Variables)
		{
			addVariable(variable.ID, variable.Name, variable.Type, TitleOfType(variable.Type), variable.Tooltip);
		}
		for (VisualScriptLocal const& local : m_Document.Locals)
		{
			addVariable(local.ID, local.Name, local.Type, fmt::format("{} (local)", TitleOfType(local.Type)), {});
		}

		for (DocumentFacts::FunctionFacts const& function : m_Document.Functions)
		{
			EntryFacts facts;
			facts.Pure = function.Pure;
			if (!IsShown(facts))
			{
				continue;
			}
			VisualScriptPaletteEntry entry;
			entry.Title = function.Name;
			entry.Subtitle = fmt::format("({})", DescribeParameters(function.Inputs, titleOfType));
			if (!function.Outputs.empty())
			{
				entry.Subtitle += fmt::format(" returns {}", DescribeParameters(function.Outputs, titleOfType));
			}
			entry.Category = "Functions";
			entry.NodeType = std::string(CallType);
			entry.Settings = Json::object({{"Function", function.ID}});
			entry.Kind = VisualScriptNodeKind::Function;
			if (!function.Pure)
			{
				entry.Pins.push_back(ExecPin("Execute", VisualScriptPinDirection::In));
			}
			for (VisualScriptParameter const& input : function.Inputs)
			{
				entry.Pins.push_back(MakePin(input.Name, VisualScriptPinDirection::In, input.Type));
			}
			if (!function.Pure)
			{
				entry.Pins.push_back(ExecPin("Then", VisualScriptPinDirection::Out));
			}
			for (VisualScriptParameter const& output : function.Outputs)
			{
				entry.Pins.push_back(MakePin(output.Name, VisualScriptPinDirection::Out, output.Type));
			}
			entry.FromDocument = true;
			Add(std::move(entry), std::move(facts));
		}

		for (DocumentFacts::CustomEvent const& event : m_Document.CustomEvents)
		{
			EntryFacts facts;
			facts.Pure = false;
			if (!IsShown(facts))
			{
				continue;
			}
			VisualScriptPaletteEntry entry;
			entry.Title = fmt::format("Send {}", event.Name);
			entry.Subtitle =
				event.ParameterList.empty() ? std::string() : fmt::format("({})", DescribeParameters(event.ParameterList, titleOfType));
			entry.Category = "Events";
			entry.NodeType = std::string(SendEventType);
			entry.Settings = Json::object({{"Name", event.Name}});
			if (!event.Parameters.is_null())
			{
				entry.Settings["Parameters"] = event.Parameters;
			}
			entry.Kind = VisualScriptNodeKind::Call;
			entry.Pins.push_back(ExecPin("Execute", VisualScriptPinDirection::In));
			VisualScriptPin target = MakePin("Target", VisualScriptPinDirection::In, std::string(EntityType));
			target.DefaultsToSelf = true;
			entry.Pins.push_back(std::move(target));
			for (VisualScriptParameter const& parameter : event.ParameterList)
			{
				entry.Pins.push_back(MakePin(parameter.Name, VisualScriptPinDirection::In, parameter.Type));
			}
			entry.Pins.push_back(ExecPin("Then", VisualScriptPinDirection::Out));
			entry.FromDocument = true;
			Add(std::move(entry), std::move(facts));
		}
	}

	void VisualScriptPaletteIndex::AddActions()
	{
		struct Action
		{
			VisualScriptPaletteAction Kind;
			std::string_view Title;
			std::string_view Key;
			std::string_view NodeType;
			std::string_view Description;
			VisualScriptNodeKind NodeKind;
			std::vector<std::string> Keywords;
		};
		std::array<Action, 4> const actions = {{
			{VisualScriptPaletteAction::AddCommentBox, "Add Comment Box", "AddCommentBox", "",
		     "Adds a comment box: a titled frame for notes, which moves the nodes inside it with it.", VisualScriptNodeKind::Data,
		     std::vector<std::string>{"comment", "note", "frame", "group"}},
			{VisualScriptPaletteAction::AddReroute, "Add Reroute", "AddReroute", "Reroute",
		     "Adds a reroute: a knot that routes a wire, of values or of the flow.", VisualScriptNodeKind::Reroute,
		     std::vector<std::string>{"wire", "knot"}},
			{VisualScriptPaletteAction::AddCustomEvent, "Add Custom Event", "AddCustomEvent", "Event.Custom",
		     "Adds a custom event of a new name, which Event.Send and C# scripts raise.", VisualScriptNodeKind::Event,
		     std::vector<std::string>{"custom", "event", "message"}},
			{VisualScriptPaletteAction::AddVariable, "Add Variable", "AddVariable", "",
		     "Adds a variable to the script, with a Get node of it.", VisualScriptNodeKind::Variable,
		     std::vector<std::string>{"variable", "new"}},
		}};
		for (Action const& action : actions)
		{
			EntryFacts facts;
			facts.Event = action.Kind == VisualScriptPaletteAction::AddCustomEvent;
			if (!IsShown(facts))
			{
				continue;
			}
			VisualScriptPaletteEntry entry;
			entry.Title = std::string(action.Title);
			entry.NodeType = std::string(action.NodeType);
			entry.Description = std::string(action.Description);
			entry.Keywords = action.Keywords;
			entry.Kind = action.NodeKind;
			if (action.Kind == VisualScriptPaletteAction::AddReroute)
			{
				entry.Pins = m_ReroutePins;
			}
			else if (action.Kind == VisualScriptPaletteAction::AddCustomEvent)
			{
				entry.Pins = m_CustomEventPins;
			}
			entry.Action = action.Kind;
			entry.RecentKey = fmt::format("{}{}", VisualScriptPaletteActionKeyPrefix, action.Key);
			Add(std::move(entry), std::move(facts));
		}
	}

	std::vector<VisualScriptPaletteMatch> RankVisualScriptPalette(VisualScriptPaletteIndex const& index, std::string_view query,
	                                                              std::span<std::string const> recents,
	                                                              VisualScriptPaletteContext const* context,
	                                                              VisualScriptLibrary const& library, VisualScriptPaletteRankStats* stats)
	{
		VisualScriptPaletteRankStats counted;
		std::vector<std::string> words;
		std::string word;
		for (char const character : query)
		{
			if (IsWhitespace(character))
			{
				if (!word.empty() && words.size() < MaxQueryWords)
				{
					words.push_back(std::move(word));
				}
				word.clear();
				continue;
			}
			word += ToLowerAscii(character);
		}
		if (!word.empty() && words.size() < MaxQueryWords)
		{
			words.push_back(std::move(word));
		}
		if (words.empty() && context == nullptr)
		{
			if (stats != nullptr)
			{
				*stats = counted;
			}
			return {};
		}
		std::string wholeQuery;
		for (std::string const& queryWord : words)
		{
			wholeQuery += wholeQuery.empty() ? "" : " ";
			wholeQuery += queryWord;
		}

		std::vector<uint64_t> wordMasks;
		for (std::string const& queryWord : words)
		{
			wordMasks.push_back(MaskCharacters(queryWord));
		}

		std::unordered_map<std::string_view, int32_t> recentPlaces;
		for (size_t place = 0; place < recents.size() && place < MaxRecentEntries; place++)
		{
			recentPlaces.emplace(recents[place], static_cast<int32_t>(place));
		}

		VisualScriptPin const* dragged = context != nullptr ? &context->Pin : nullptr;
		bool const draggedHasValues = dragged != nullptr && dragged->Type != VisualScriptExecType && !IsWildcard(*dragged);
		bool const draggedIsNumeric = draggedHasValues && IsNumericType(library, dragged->Type);
		auto const fit = [&](VisualScriptPaletteEntry const& entry, VisualScriptPaletteIndex::EntryFacts const& facts)
		{
			ContextFit found;
			VisualScriptPinDirection const wanted =
				dragged->Direction == VisualScriptPinDirection::Out ? VisualScriptPinDirection::In : VisualScriptPinDirection::Out;
			if (!facts.ConvertTo.empty())
			{
				if (!draggedHasValues)
				{
					return found;
				}
				std::string const& target = facts.ConvertTo;
				bool const targetIsNumeric = IsNumericType(library, target);
				if (dragged->Direction == VisualScriptPinDirection::Out)
				{
					found.Fits = (target == StringType && dragged->Type != StringType) ||
					             (draggedIsNumeric && targetIsNumeric && target != dragged->Type);
				}
				else
				{
					found.Fits = target == dragged->Type && (target == StringType || targetIsNumeric);
					found.Quality = 2;
				}
				auto const pin = std::find_if(entry.Pins.begin(), entry.Pins.end(),
				                              [wanted](VisualScriptPin const& candidate)
				                              {
												  return candidate.Direction == wanted;
											  });
				found.Fits = found.Fits && pin != entry.Pins.end();
				if (found.Fits)
				{
					found.Pin = pin->Name;
				}
				return found;
			}
			for (size_t pinIndex = 0; pinIndex < entry.Pins.size(); pinIndex++)
			{
				VisualScriptPin const& pin = entry.Pins[pinIndex];
				if (pin.Direction != wanted || facts.SettingsDependent[pinIndex])
				{
					continue;
				}
				bool const connects = dragged->Direction == VisualScriptPinDirection::Out ? library.CanConnect(*dragged, pin)
				                                                                          : library.CanConnect(pin, *dragged);
				if (!connects)
				{
					continue;
				}
				int const quality = pin.Type == dragged->Type && !IsWildcard(pin) ? 2 : (draggedHasValues && !IsWildcard(pin) ? 1 : 0);
				if (!found.Fits || quality > found.Quality)
				{
					found.Fits = true;
					found.Quality = quality;
					found.Pin = pin.Name;
				}
				if (quality == 2)
				{
					break;
				}
			}
			return found;
		};

		std::vector<VisualScriptPaletteMatch> matches;
		std::vector<bool> exact;
		std::vector<uint16_t> highlights;
		std::vector<int32_t> costs;
		std::vector<int32_t> previous;
		std::span<VisualScriptPaletteEntry const> const entries = index.GetEntries();
		for (size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++)
		{
			counted.EntriesScored++;
			VisualScriptPaletteEntry const& entry = entries[entryIndex];
			VisualScriptPaletteIndex::EntryFacts const& facts = index.m_Facts[entryIndex];
			VisualScriptPaletteIndex::SearchKey const& key = index.m_Keys[entryIndex];

			ContextFit connection;
			if (dragged != nullptr)
			{
				if (entry.Action == VisualScriptPaletteAction::GoToNode)
				{
					continue;
				}
				connection = fit(entry, facts);
				if (!connection.Fits)
				{
					continue;
				}
			}

			highlights.clear();
			int32_t score = 0;
			bool matchesAll = true;
			for (size_t wordIndex = 0; wordIndex < words.size(); wordIndex++)
			{
				std::string const& queryWord = words[wordIndex];
				size_t const length = queryWord.size();
				int32_t wordScore = 0;
				if ((wordMasks[wordIndex] & ~key.Characters) != 0)
				{
					matchesAll = false;
					break;
				}
				if (key.Title.starts_with(queryWord))
				{
					wordScore = TitlePrefixScore;
					for (size_t at = 0; at < length; at++)
					{
						highlights.push_back(static_cast<uint16_t>(at));
					}
				}
				else if (auto const start = std::find_if(key.TitleWords.begin(), key.TitleWords.end(),
				                                         [&](uint16_t candidate)
				                                         {
															 return std::string_view(key.Title).substr(candidate).starts_with(queryWord);
														 });
				         start != key.TitleWords.end())
				{
					wordScore = TitleWordScore;
					for (size_t at = 0; at < length; at++)
					{
						highlights.push_back(static_cast<uint16_t>(*start + at));
					}
				}
				else if (size_t const initials = key.Initials.find(queryWord); initials != std::string::npos)
				{
					wordScore = InitialsScore;
					for (size_t at = 0; at < length; at++)
					{
						highlights.push_back(key.TitleWords[initials + at]);
					}
				}
				else if (size_t const contained = key.Title.find(queryWord); contained != std::string::npos)
				{
					wordScore = TitleContainsScore;
					for (size_t at = 0; at < length; at++)
					{
						highlights.push_back(static_cast<uint16_t>(contained + at));
					}
				}
				else if (int32_t const keywordScore = ScoreKeywords(key.Keywords, queryWord); keywordScore > 0)
				{
					wordScore = keywordScore;
				}
				else if (StartsWordWith(key.Subtitle, key.SubtitleWords, queryWord))
				{
					wordScore = SubtitleWordScore;
				}
				else if (StartsWordWith(key.Category, key.CategoryWords, queryWord))
				{
					wordScore = CategoryWordScore;
				}
				else if (key.NodeType.find(queryWord) != std::string::npos)
				{
					wordScore = TypeContainsScore;
				}
				else if (length >= MinSubsequenceLength && length <= MaxSubsequenceLength)
				{
					counted.SubsequenceSearches++;
					int32_t const gaps = FindSubsequence(key.Title, queryWord, highlights, costs, previous);
					if (gaps >= 0)
					{
						wordScore = std::max(MinSubsequenceScore, SubsequenceScore - SubsequenceGapCost * gaps);
					}
				}
				if (wordScore == 0)
				{
					matchesAll = false;
					break;
				}
				score += wordScore;
			}
			if (!matchesAll)
			{
				continue;
			}
			if (!words.empty() && wholeQuery == key.Title)
			{
				score += WholeTitleScore;
			}
			if (auto const recent = recentPlaces.find(entry.RecentKey); recent != recentPlaces.end())
			{
				score += RecentScore - RecentPlaceCost * recent->second;
			}
			if (connection.Fits)
			{
				score += connection.Quality == 2 ? ExactPinScore : (connection.Quality == 1 ? ConvertingPinScore : 0);
			}
			if (entry.FromDocument)
			{
				score += DocumentScore;
			}
			score -= TitleByteCost * static_cast<int32_t>(key.Title.size());

			std::sort(highlights.begin(), highlights.end());
			highlights.erase(std::unique(highlights.begin(), highlights.end()), highlights.end());
			VisualScriptPaletteMatch match;
			match.Entry = static_cast<uint32_t>(entryIndex);
			match.Score = score;
			match.Highlights = highlights;
			match.ConnectPin = std::move(connection.Pin);
			match.Converts = connection.Fits && connection.Quality == 1;
			matches.push_back(std::move(match));
			exact.push_back(connection.Fits && connection.Quality == 2 && facts.ConvertTo.empty());
		}

		// Without a query, Convert's rows are for what nothing listed takes as it is.
		if (dragged != nullptr && words.empty() && std::find(exact.begin(), exact.end(), true) != exact.end())
		{
			std::erase_if(matches,
			              [&index](VisualScriptPaletteMatch const& match)
			              {
							  return !index.m_Facts[match.Entry].ConvertTo.empty();
						  });
		}
		std::stable_sort(matches.begin(), matches.end(),
		                 [](VisualScriptPaletteMatch const& a, VisualScriptPaletteMatch const& b)
		                 {
							 return a.Score > b.Score;
						 });
		if (stats != nullptr)
		{
			*stats = counted;
		}
		return matches;
	}
}
