#include "Editor/VisualScripting/VisualScriptLanguageEditing.h"

#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace Strada::VisualScriptEditing
{
	namespace
	{
		using Format = VisualScriptSerializer;

		constexpr std::string_view ArrayMakeType = "Array.Make";
		constexpr std::string_view ExpressionType = "Expression";
		constexpr char const* CountSetting = "Count";
		constexpr char const* FormulaSetting = "Formula";
		constexpr std::string_view ElementPrefix = "Element";
		// An Array.Make without Count has the node library's default.
		constexpr int DefaultArrayElements = 2;
		// The longest identifier that names an input (a name's limit).
		constexpr size_t MaxIdentifierBytes = Format::MaxNameBytes;
		constexpr std::array<std::string_view, 4> FormulaConstants = {"true", "false", "pi", "tau"};
		// The name of an Expression's output, which no input may have.
		constexpr std::string_view ResultPin = "Result";

		// A node of the document, with the graph that holds it.
		struct NodePlace
		{
			VisualScriptGraph* Graph = nullptr;
			VisualScriptNode* Node = nullptr;
		};

		Result<NodePlace> GetNode(VisualScriptDocument& document, uint32_t id)
		{
			auto const find = [id](VisualScriptGraph& graph) -> VisualScriptNode*
			{
				auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
				                                [id](VisualScriptNode const& node)
				                                {
													return node.ID == id;
												});
				return found != graph.Nodes.end() ? &*found : nullptr;
			};
			for (VisualScriptFunction& function : document.Functions)
			{
				if (VisualScriptNode* const node = find(function.Graph))
				{
					return NodePlace{&function.Graph, node};
				}
			}
			if (VisualScriptNode* const node = find(document.Graph))
			{
				return NodePlace{&document.Graph, node};
			}
			return MakeError("the document has no node {}", id);
		}

		// A node of the type, whose Settings and Inputs are objects.
		Result<NodePlace> GetNodeOfType(VisualScriptDocument& document, uint32_t id, std::string_view type)
		{
			Result<NodePlace> place = GetNode(document, id);
			if (!place)
			{
				return place;
			}
			VisualScriptNode const& node = *place.GetValue().Node;
			if (node.Type != type)
			{
				return MakeError("node {} is no {}: its type is {}", id, type, node.Type);
			}
			if (!node.Settings.is_object() || !node.Inputs.is_object())
			{
				return MakeError("node {} has Settings or Inputs that are not objects", id);
			}
			return place;
		}

		// The index of an Array.Make's element pin ("Element3", or a field of it, "Element3.X"); nothing for other pins.
		std::optional<int> ParseElementPin(std::string_view pin)
		{
			if (!pin.starts_with(ElementPrefix))
			{
				return std::nullopt;
			}
			std::string_view digits = pin.substr(ElementPrefix.size());
			digits = digits.substr(0, digits.find('.'));
			if (digits.empty() || digits.size() > 2 || (digits.size() > 1 && digits.front() == '0') ||
			    !std::all_of(digits.begin(), digits.end(),
			                 [](char digit)
			                 {
								 return digit >= '0' && digit <= '9';
							 }))
			{
				return std::nullopt;
			}
			int index = 0;
			for (char const digit : digits)
			{
				index = index * 10 + (digit - '0');
			}
			return index;
		}

		// The elements an Array.Make has: Count, the default when it is missing, and none known when it is not a count.
		std::optional<int> ReadElementCount(VisualScriptNode const& node)
		{
			auto const stored = node.Settings.find(CountSetting);
			if (stored == node.Settings.end())
			{
				return DefaultArrayElements;
			}
			if (stored->is_number_unsigned())
			{
				uint64_t const value = stored->get<uint64_t>();
				return value <= static_cast<uint64_t>(MaxArrayElements) ? std::optional<int>(static_cast<int>(value)) : std::nullopt;
			}
			if (stored->is_number_integer())
			{
				int64_t const value = stored->get<int64_t>();
				return value >= 0 && value <= MaxArrayElements ? std::optional<int>(static_cast<int>(value)) : std::nullopt;
			}
			return std::nullopt;
		}

		// The names of an Array.Make's element pins its stored pins name (links into it, literals, split paths), each once.
		std::vector<int> CollectElementIndices(VisualScriptGraph const& graph, VisualScriptNode const& node)
		{
			std::vector<int> indices;
			auto const add = [&indices](std::string_view pin)
			{
				if (std::optional<int> const index = ParseElementPin(pin);
				    index && std::find(indices.begin(), indices.end(), *index) == indices.end())
				{
					indices.push_back(*index);
				}
			};
			for (VisualScriptLink const& link : graph.Links)
			{
				if (link.To.Node == node.ID)
				{
					add(link.To.Pin);
				}
			}
			for (auto const& [name, value] : node.Inputs.items())
			{
				static_cast<void>(value);
				add(name);
			}
			for (std::string const& path : node.Split.Inputs)
			{
				add(path);
			}
			return indices;
		}

		// The lexer of §3.3.2, as far as the input rule needs it: tokens with their kinds and extents. Characters that start no
		// token are passed over (the scripting runtime reports them and goes on), one byte at a time: no byte of a character
		// outside ASCII starts a token.
		enum class TokenKind : uint8_t
		{
			Identifier = 0,
			Number,
			OpenParenthesis,
			Dot,
			OtherPunctuation,
			End
		};

		struct Token
		{
			TokenKind Kind = TokenKind::End;
			size_t Start = 0;
			size_t End = 0;
		};

		bool IsLetter(char character)
		{
			return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
		}

		bool IsDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		bool IsWhiteSpace(char character)
		{
			return character == ' ' || character == '\t' || character == '\n' || character == '\r';
		}

		class FormulaLexer
		{
		public:
			explicit FormulaLexer(std::string_view text)
				: m_Text(text)
			{
			}

			std::vector<Token> Run()
			{
				while (m_At < m_Text.size())
				{
					char const character = m_Text[m_At];
					if (IsWhiteSpace(character))
					{
						m_At++;
					}
					else if (IsLetter(character) || character == '_')
					{
						Identifier();
					}
					else if (IsDigit(character))
					{
						Number();
					}
					else
					{
						Punctuation(character);
					}
				}
				m_Tokens.push_back({TokenKind::End, m_Text.size(), m_Text.size()});
				return std::move(m_Tokens);
			}

		private:
			void Identifier()
			{
				size_t const start = m_At;
				while (m_At < m_Text.size() && (IsLetter(m_Text[m_At]) || IsDigit(m_Text[m_At]) || m_Text[m_At] == '_'))
				{
					m_At++;
				}
				m_Tokens.push_back({TokenKind::Identifier, start, m_At});
			}

			// digits ["." digits] [("e" | "E") ["+" | "-"] digits] ["d" | "D"], each optional part only when it is complete.
			void Number()
			{
				size_t const start = m_At;
				SkipDigits();
				if (m_At + 1 < m_Text.size() && m_Text[m_At] == '.' && IsDigit(m_Text[m_At + 1]))
				{
					m_At++;
					SkipDigits();
				}
				if (m_At < m_Text.size() && (m_Text[m_At] == 'e' || m_Text[m_At] == 'E'))
				{
					size_t exponent = m_At + 1;
					if (exponent < m_Text.size() && (m_Text[exponent] == '+' || m_Text[exponent] == '-'))
					{
						exponent++;
					}
					if (exponent < m_Text.size() && IsDigit(m_Text[exponent]))
					{
						m_At = exponent;
						SkipDigits();
					}
				}
				if (m_At < m_Text.size() && (m_Text[m_At] == 'd' || m_Text[m_At] == 'D'))
				{
					m_At++;
				}
				m_Tokens.push_back({TokenKind::Number, start, m_At});
			}

			void SkipDigits()
			{
				while (m_At < m_Text.size() && IsDigit(m_Text[m_At]))
				{
					m_At++;
				}
			}

			void Punctuation(char character)
			{
				size_t const start = m_At;
				char const next = m_At + 1 < m_Text.size() ? m_Text[m_At + 1] : '\0';
				size_t length = 0;
				TokenKind kind = TokenKind::OtherPunctuation;
				switch (character)
				{
					case '(':
						kind = TokenKind::OpenParenthesis;
						length = 1;
						break;
					case '.':
						kind = TokenKind::Dot;
						length = 1;
						break;
					case '+':
					case '-':
					case '*':
					case '/':
					case '%':
					case ')':
					case ',':
					case '?':
					case ':':
					case '!':
					case '<':
					case '>':
						length = (character == '!' || character == '<' || character == '>') && next == '=' ? 2 : 1;
						break;
					case '=':
					case '&':
					case '|':
						length = next == character ? 2 : 0;
						break;
					default:
						break;
				}
				if (length == 0)
				{
					m_At++;
					return;
				}
				m_At += length;
				m_Tokens.push_back({kind, start, m_At});
			}

			std::string_view m_Text;
			size_t m_At = 0;
			std::vector<Token> m_Tokens;
		};

		std::vector<std::string> GetFormulaInputs(VisualScriptNode const& node)
		{
			auto const formula = node.Settings.find(FormulaSetting);
			if (formula == node.Settings.end() || !formula->is_string())
			{
				return {};
			}
			return ListVisualScriptFormulaInputs(formula->get_ref<std::string const&>());
		}
	}

	std::string GetVisualScriptArrayElementPin(int index)
	{
		return std::string(ElementPrefix) + std::to_string(index);
	}

	Result<uint32_t> SetArrayElements(VisualScriptDocument& document, uint32_t node, int count, std::optional<int> insertAt,
	                                  std::optional<int> removeAt)
	{
		Result<NodePlace> place = GetNodeOfType(document, node, ArrayMakeType);
		if (!place)
		{
			return Error{place.GetError()};
		}
		VisualScriptGraph& graph = *place.GetValue().Graph;
		VisualScriptNode& make = *place.GetValue().Node;
		if (count < 0 || count > MaxArrayElements)
		{
			return MakeError("a Make Array has 0 to {} elements", MaxArrayElements);
		}
		if (insertAt && removeAt)
		{
			return Error{"elements are inserted or removed, not both at once"};
		}
		std::optional<int> const current = ReadElementCount(make);
		if ((insertAt || removeAt) && !current)
		{
			return MakeError("node {} has no Count of 0 to {} to insert or remove elements at", node, MaxArrayElements);
		}
		if (insertAt && (count <= *current || *insertAt < 0 || *insertAt > *current))
		{
			return MakeError("inserting elements at {} takes a count above {} and an index from 0 to {}", *insertAt, *current, *current);
		}
		if (removeAt && (count >= *current || *removeAt < 0 || *removeAt > count))
		{
			return MakeError("removing elements at {} takes a count below {} and an index from 0 to {}", *removeAt, *current, count);
		}

		// The element pins the node's stored pins name, renamed or removed as their index says.
		VisualScriptPinChanges changes;
		for (int const index : CollectElementIndices(graph, make))
		{
			std::string pin = GetVisualScriptArrayElementPin(index);
			if (insertAt)
			{
				if (index >= *insertAt && index < *current)
				{
					changes.Renamed.emplace_back(std::move(pin), GetVisualScriptArrayElementPin(index + (count - *current)));
				}
			}
			else if (removeAt)
			{
				int const removed = *current - count;
				if (index >= *removeAt && index < *removeAt + removed)
				{
					changes.Removed.push_back(std::move(pin));
				}
				else if (index >= *removeAt + removed && index < *current)
				{
					changes.Renamed.emplace_back(std::move(pin), GetVisualScriptArrayElementPin(index - removed));
				}
			}
			else if (index >= count && (!current || index < *current))
			{
				changes.Removed.push_back(std::move(pin));
			}
		}
		uint32_t const removed = ApplyVisualScriptPinChanges(graph, make, VisualScriptPinDirection::In, changes);
		make.Settings[CountSetting] = count;
		return removed;
	}

	std::vector<std::string> ListVisualScriptFormulaInputs(std::string_view formula)
	{
		std::vector<std::string> inputs;
		if (formula.size() > MaxFormulaBytes)
		{
			return inputs;
		}
		std::vector<Token> const tokens = FormulaLexer(formula).Run();
		for (size_t index = 0; index < tokens.size() && inputs.size() < MaxFormulaInputs; index++)
		{
			Token const& token = tokens[index];
			if (token.Kind != TokenKind::Identifier || token.End - token.Start > MaxIdentifierBytes)
			{
				continue;
			}
			// The End token follows every other token.
			if (tokens[index + 1].Kind == TokenKind::OpenParenthesis || (index > 0 && tokens[index - 1].Kind == TokenKind::Dot))
			{
				continue;
			}
			std::string_view const name = formula.substr(token.Start, token.End - token.Start);
			if (std::find(FormulaConstants.begin(), FormulaConstants.end(), name) != FormulaConstants.end() ||
			    std::find(inputs.begin(), inputs.end(), name) != inputs.end() || IsReservedPinName(name) || name == ResultPin)
			{
				continue;
			}
			inputs.emplace_back(name);
		}
		return inputs;
	}

	Result<VisualScriptFormulaChange> SetFormula(VisualScriptDocument& document, uint32_t node, std::string formula)
	{
		Result<NodePlace> place = GetNodeOfType(document, node, ExpressionType);
		if (!place)
		{
			return Error{place.GetError()};
		}
		if (formula.size() > MaxFormulaBytes)
		{
			return MakeError("a formula has at most {} bytes", MaxFormulaBytes);
		}
		if (!Format::IsValidLongText(formula))
		{
			return Error{"a formula must be UTF-8"};
		}
		VisualScriptGraph& graph = *place.GetValue().Graph;
		VisualScriptNode& expression = *place.GetValue().Node;
		if (!expression.Settings.contains(FormulaSetting) && expression.Settings.size() >= Format::MaxSettingsMembers)
		{
			return MakeError("a node holds at most {} settings", Format::MaxSettingsMembers);
		}

		VisualScriptPinChanges const changes =
			CompareVisualScriptPinLists(GetFormulaInputs(expression), ListVisualScriptFormulaInputs(formula));
		VisualScriptFormulaChange change;
		change.RemovedLinks = ApplyVisualScriptPinChanges(graph, expression, VisualScriptPinDirection::In, changes);
		change.RenamedInputs = changes.Renamed;
		expression.Settings[FormulaSetting] = std::move(formula);
		return change;
	}

	Result<uint32_t> SetTimeline(VisualScriptDocument& document, uint32_t node, VisualScriptTimeline const& timeline,
	                             std::span<VisualScriptTrackRename const> renames)
	{
		Result<NodePlace> place = GetNodeOfType(document, node, VisualScriptTimelineNodeType);
		if (!place)
		{
			return Error{place.GetError()};
		}
		if (Result<void> checked = CheckVisualScriptTimeline(timeline); !checked)
		{
			return Error{checked.GetError()};
		}
		VisualScriptGraph& graph = *place.GetValue().Graph;
		VisualScriptNode& owner = *place.GetValue().Node;
		std::string const setting(VisualScriptTimelineSetting);
		if (!owner.Settings.contains(setting) && owner.Settings.size() >= Format::MaxSettingsMembers)
		{
			return MakeError("a node holds at most {} settings", Format::MaxSettingsMembers);
		}
		auto const current = owner.Settings.find(setting);
		std::vector<std::pair<std::string, std::string>> const before = current != owner.Settings.end()
		                                                                    ? ListVisualScriptTimelineTrackPins(*current)
		                                                                    : std::vector<std::pair<std::string, std::string>>();
		auto const findTrack = [&timeline](std::string_view name) -> VisualScriptTimelineTrack const*
		{
			auto const found = std::find_if(timeline.Tracks.begin(), timeline.Tracks.end(),
			                                [name](VisualScriptTimelineTrack const& track)
			                                {
												return track.Name == name;
											});
			return found != timeline.Tracks.end() ? &*found : nullptr;
		};
		std::unordered_set<std::string> from;
		std::unordered_set<std::string> to;
		for (VisualScriptTrackRename const& rename : renames)
		{
			bool const known = std::any_of(before.begin(), before.end(),
			                               [&rename](auto const& pin)
			                               {
											   return pin.first == rename.From;
										   });
			if (!known)
			{
				return MakeError("the timeline of node {} has no track {} to rename", node, rename.From);
			}
			if (findTrack(rename.To) == nullptr)
			{
				return MakeError("the new timeline has no track {}: a rename names the track's new name", rename.To);
			}
			if (!from.insert(rename.From).second)
			{
				return MakeError("track {} is renamed twice", rename.From);
			}
			if (!to.insert(rename.To).second)
			{
				return MakeError("two tracks are renamed to {}", rename.To);
			}
		}

		// Each current track pin goes to the track it is renamed to, else the one of its name; its links stay when that
		// track has its type.
		VisualScriptPinChanges changes;
		for (auto const& [name, type] : before)
		{
			auto const renamed = std::find_if(renames.begin(), renames.end(),
			                                  [&name](VisualScriptTrackRename const& rename)
			                                  {
												  return rename.From == name;
											  });
			std::string const& target = renamed != renames.end() ? renamed->To : name;
			VisualScriptTimelineTrack const* const track = findTrack(target);
			if (track == nullptr || track->Type != type || (renamed == renames.end() && to.contains(name)))
			{
				changes.Removed.push_back(name);
			}
			else if (target != name)
			{
				changes.Renamed.emplace_back(name, target);
			}
		}
		uint32_t const removed = ApplyVisualScriptPinChanges(graph, owner, VisualScriptPinDirection::Out, changes);
		owner.Settings[setting] = WriteVisualScriptTimeline(timeline);
		return removed;
	}
}
