#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		using Format = VisualScriptSerializer;

		constexpr std::array<std::string_view, 5> ReservedPinNames = {"Execute", "Then", "Target", "Self", "Default"};
		// What MakeUniqueVisualScriptName makes of a base without a usable character.
		constexpr std::string_view FallbackName = "Value";
		constexpr float MaxCoordinate = static_cast<float>(Format::MaxCoordinate);
		constexpr float MinCommentSize = static_cast<float>(Format::MinCommentSize);
		constexpr float MaxCommentSize = static_cast<float>(Format::MaxCommentSize);
		// Split paths nest no deeper than their 128 bytes allow; a bound for the walks over struct fields.
		constexpr size_t MaxSplitDepth = Format::MaxPinNameBytes / 2;

		// Node types and the setting keys whose meaning the operations rely on (v2 §4).
		constexpr std::string_view RerouteType = "Reroute";
		constexpr std::string_view SequenceType = "Flow.Sequence";
		constexpr std::string_view SwitchType = "Flow.Switch";
		constexpr std::string_view CustomEventType = "Event.Custom";
		constexpr std::string_view SendEventType = "Event.Send";
		constexpr std::string_view EntryType = "Function.Entry";
		constexpr std::string_view ReturnType = "Function.Return";
		constexpr std::string_view CallType = "Function.Call";
		constexpr std::string_view GetVariableType = "Variable.Get";
		constexpr std::string_view SetVariableType = "Variable.Set";
		constexpr char const* CountSetting = "Count";
		constexpr char const* CasesSetting = "Cases";
		constexpr char const* TypeSetting = "Type";
		constexpr char const* ParametersSetting = "Parameters";
		constexpr char const* NameSetting = "Name";
		constexpr char const* VariableSetting = "Variable";
		constexpr char const* FunctionSetting = "Function";
		constexpr std::string_view CasePrefix = "Case:";
		constexpr std::string_view SequenceOutputPrefix = "Then";
		constexpr int MinSequenceOutputs = 2;
		constexpr int MaxSequenceOutputs = 64;

		// Where a promoted variable's node stands, from the anchor of the pin it serves.
		constexpr float PromotedGetOffset = -240.0f;
		constexpr float PromotedSetOffset = 80.0f;
		constexpr float PromotedNodeRise = 16.0f;
		// The size of a comment box that frames nothing.
		constexpr glm::vec2 DefaultCommentSize{400.0f, 200.0f};

		std::string_view DescribeDirection(VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? "input" : "output";
		}

		// A text as a quoted JSON string, for messages about text that may hold anything; long text is cut short.
		std::string Quote(std::string_view text)
		{
			constexpr size_t MaxQuotedBytes = 64;
			std::string quoted = Json(std::string(text.substr(0, MaxQuotedBytes))).dump(-1, ' ', false, Json::error_handler_t::replace);
			return text.size() > MaxQuotedBytes ? quoted + "..." : quoted;
		}

		Error MakeNameError(std::string_view name)
		{
			return MakeError("{}: expected a name of 1 to {} bytes without control characters, '.' or a leading or trailing space",
			                 Quote(name), Format::MaxNameBytes);
		}

		bool HasControlCharacters(std::string_view text)
		{
			return std::any_of(text.begin(), text.end(),
			                   [](char character)
			                   {
								   auto const code = static_cast<unsigned char>(character);
								   return code < 0x20 || code == 0x7F;
							   });
		}

		// Types, pin names and the names of Settings and Inputs members: 1 to maxBytes of UTF-8 without control characters, as
		// the format requires of them.
		bool IsValidText(std::string_view text, size_t maxBytes)
		{
			return !text.empty() && text.size() <= maxBytes && !HasControlCharacters(text) && Utf8::IsValid(text);
		}

		// Descriptions, tooltips and the texts of comments and nodes: at most MaxTextBytes of UTF-8 (lines allowed).
		bool IsValidLongText(std::string_view text)
		{
			return text.size() <= Format::MaxTextBytes && Utf8::IsValid(text);
		}

		Error MakeLongTextError(std::string_view what)
		{
			return MakeError("{} must be UTF-8 of at most {} bytes", what, Format::MaxTextBytes);
		}

		// The entries of Split: pin and field names separated by '.', none of them empty.
		bool IsValidPinPath(std::string_view path)
		{
			return IsValidText(path, Format::MaxPinNameBytes) && path.front() != '.' && path.back() != '.' &&
			       path.find("..") == std::string_view::npos;
		}

		// A value as variables, Settings and Inputs store it that saves and loads back as it is: nested at most MaxValueDepth
		// levels (the value itself is one), strings and member names of at most MaxTextBytes of UTF-8, and finite numbers.
		bool IsValidValue(Json const& value, size_t depth = 1)
		{
			if (depth > Format::MaxValueDepth)
			{
				return false;
			}
			if (value.is_string())
			{
				return IsValidLongText(value.get_ref<std::string const&>());
			}
			if (value.is_number_float())
			{
				return std::isfinite(value.get<double>());
			}
			if (value.is_object())
			{
				for (auto const& item : value.items())
				{
					if (!IsValidLongText(item.key()) || !IsValidValue(item.value(), depth + 1))
					{
						return false;
					}
				}
				return true;
			}
			if (value.is_array())
			{
				return std::all_of(value.begin(), value.end(),
				                   [depth](Json const& element)
				                   {
									   return IsValidValue(element, depth + 1);
								   });
			}
			return value.is_null() || value.is_boolean() || value.is_number();
		}

		Error MakeValueError(std::string_view what)
		{
			return MakeError("{} must nest at most {} levels deep, with finite numbers and strings of at most {} bytes of UTF-8", what,
			                 Format::MaxValueDepth, Format::MaxTextBytes);
		}

		// The types of variables, locals and parameters: what the format accepts as a type, and a type with values.
		Result<void> CheckValueType(std::string_view type, std::string_view what)
		{
			if (!IsValidText(type, Format::MaxTypeBytes))
			{
				return MakeError("{}: expected a type of 1 to {} bytes without control characters", Quote(type), Format::MaxTypeBytes);
			}
			if (type == VisualScriptExecType || type == VisualScriptWildcardType)
			{
				return MakeError("{} cannot have the type {}", what, type);
			}
			return {};
		}

		bool IsFinite(glm::vec2 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		bool IsFinite(glm::vec4 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		glm::vec2 ClampPosition(glm::vec2 position)
		{
			return glm::clamp(position, glm::vec2(-MaxCoordinate), glm::vec2(MaxCoordinate));
		}

		glm::vec2 RoundPosition(glm::vec2 position)
		{
			return ClampPosition(glm::vec2(std::round(position.x), std::round(position.y)));
		}

		// The text cut to at most maxBytes, at a character boundary of its UTF-8.
		std::string_view CutAtCharacter(std::string_view text, size_t maxBytes)
		{
			if (text.size() <= maxBytes)
			{
				return text;
			}
			size_t end = maxBytes;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
			{
				end--;
			}
			return text.substr(0, end);
		}

		std::string_view TrimSpaces(std::string_view text)
		{
			size_t const first = text.find_first_not_of(' ');
			if (first == std::string_view::npos)
			{
				return {};
			}
			return text.substr(first, text.find_last_not_of(' ') - first + 1);
		}

		// The base without what names cannot hold (control characters, '.', bytes that are not UTF-8), trimmed and cut to
		// MaxNameBytes; FallbackName when nothing is left.
		std::string SanitizeName(std::string_view base)
		{
			std::string kept;
			kept.reserve(base.size());
			size_t offset = 0;
			while (offset < base.size())
			{
				size_t const start = offset;
				char32_t const character = Utf8::DecodeNext(base, offset);
				bool const invalidByte = character == Utf8::ReplacementCharacter && offset - start == 1;
				if (invalidByte || character < 0x20 || character == 0x7F || character == U'.')
				{
					continue;
				}
				kept.append(base.substr(start, offset - start));
			}
			std::string_view const name = TrimSpaces(CutAtCharacter(TrimSpaces(kept), Format::MaxNameBytes));
			return name.empty() ? std::string(FallbackName) : std::string(name);
		}

		// A value in compact JSON, for messages; long values are cut short.
		std::string DescribeValue(Json const& value)
		{
			constexpr size_t MaxDescribedBytes = 64;
			std::string const text = value.dump(-1, ' ', false, Json::error_handler_t::replace);
			return text.size() > MaxDescribedBytes ? std::string(CutAtCharacter(text, MaxDescribedBytes)) + "..." : text;
		}

		// Graphs, nodes and comments.

		VisualScriptFunction* FindFunction(VisualScriptDocument& document, uint32_t id)
		{
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [id](VisualScriptFunction const& function)
			                                {
												return function.ID == id;
											});
			return found != document.Functions.end() ? &*found : nullptr;
		}

		VisualScriptFunction const* FindFunction(VisualScriptDocument const& document, uint32_t id)
		{
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [id](VisualScriptFunction const& function)
			                                {
												return function.ID == id;
											});
			return found != document.Functions.end() ? &*found : nullptr;
		}

		Result<VisualScriptFunction*> GetFunction(VisualScriptDocument& document, uint32_t id)
		{
			if (VisualScriptFunction* const function = FindFunction(document, id))
			{
				return function;
			}
			return MakeError("the document has no function {}", id);
		}

		Result<VisualScriptGraph*> GetGraph(VisualScriptDocument& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			if (VisualScriptFunction* const found = FindFunction(document, function))
			{
				return &found->Graph;
			}
			return MakeError("the document has no function {}", function);
		}

		VisualScriptNode* FindNode(VisualScriptGraph& graph, uint32_t id)
		{
			auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                                [id](VisualScriptNode const& node)
			                                {
												return node.ID == id;
											});
			return found != graph.Nodes.end() ? &*found : nullptr;
		}

		VisualScriptComment* FindComment(VisualScriptGraph& graph, uint32_t id)
		{
			auto const found = std::find_if(graph.Comments.begin(), graph.Comments.end(),
			                                [id](VisualScriptComment const& comment)
			                                {
												return comment.ID == id;
											});
			return found != graph.Comments.end() ? &*found : nullptr;
		}

		// Every graph of the document: the functions' in order, then the event graph.
		std::vector<VisualScriptGraph*> GetGraphs(VisualScriptDocument& document)
		{
			std::vector<VisualScriptGraph*> graphs;
			graphs.reserve(document.Functions.size() + 1);
			for (VisualScriptFunction& function : document.Functions)
			{
				graphs.push_back(&function.Graph);
			}
			graphs.push_back(&document.Graph);
			return graphs;
		}

		// A node of the document, with the graph that holds it.
		struct NodePlace
		{
			VisualScriptGraph* Graph = nullptr;
			VisualScriptNode* Node = nullptr;
		};

		Result<NodePlace> GetNode(VisualScriptDocument& document, uint32_t id)
		{
			for (VisualScriptGraph* const graph : GetGraphs(document))
			{
				if (VisualScriptNode* const node = FindNode(*graph, id))
				{
					return NodePlace{graph, node};
				}
			}
			return MakeError("the document has no node {}", id);
		}

		Result<VisualScriptComment*> GetComment(VisualScriptDocument& document, uint32_t id)
		{
			for (VisualScriptGraph* const graph : GetGraphs(document))
			{
				if (VisualScriptComment* const comment = FindComment(*graph, id))
				{
					return comment;
				}
			}
			return MakeError("the document has no comment {}", id);
		}

		// The nodes of a list of IDs, each found in whichever graph holds it (as GetNode finds it), in one pass over the
		// document; an ID listed twice gives its node once.
		Result<std::vector<VisualScriptNode*>> GetNodes(VisualScriptDocument& document, std::span<uint32_t const> ids)
		{
			std::unordered_set<uint32_t> const wanted(ids.begin(), ids.end());
			std::unordered_map<uint32_t, VisualScriptNode*> found;
			for (VisualScriptGraph* const graph : GetGraphs(document))
			{
				for (VisualScriptNode& node : graph->Nodes)
				{
					if (wanted.contains(node.ID))
					{
						found.emplace(node.ID, &node);
					}
				}
			}
			std::vector<VisualScriptNode*> nodes;
			nodes.reserve(found.size());
			std::unordered_set<uint32_t> listed;
			for (uint32_t const id : ids)
			{
				auto const node = found.find(id);
				if (node == found.end())
				{
					return MakeError("the document has no node {}", id);
				}
				if (listed.insert(id).second)
				{
					nodes.push_back(node->second);
				}
			}
			return nodes;
		}

		// The nodes and comments of a graph that a list of IDs names, found in one pass over the graph: for each ID the node
		// FindNode finds, else the comment FindComment finds. Valid while the graph's lists keep their elements.
		class ElementLookup
		{
		public:
			ElementLookup(VisualScriptGraph& graph, std::span<uint32_t const> ids)
			{
				std::unordered_set<uint32_t> const wanted(ids.begin(), ids.end());
				for (VisualScriptNode& node : graph.Nodes)
				{
					if (wanted.contains(node.ID))
					{
						m_Nodes.emplace(node.ID, &node);
					}
				}
				for (VisualScriptComment& comment : graph.Comments)
				{
					if (wanted.contains(comment.ID))
					{
						m_Comments.emplace(comment.ID, &comment);
					}
				}
			}

			VisualScriptNode* FindNode(uint32_t id) const
			{
				auto const found = m_Nodes.find(id);
				return found != m_Nodes.end() ? found->second : nullptr;
			}

			VisualScriptComment* FindComment(uint32_t id) const
			{
				auto const found = m_Comments.find(id);
				return found != m_Comments.end() ? found->second : nullptr;
			}

		private:
			std::unordered_map<uint32_t, VisualScriptNode*> m_Nodes;
			std::unordered_map<uint32_t, VisualScriptComment*> m_Comments;
		};

		// The first Function.Entry of a graph: the entry a function keeps; null when it has none.
		VisualScriptNode* FindEntry(VisualScriptGraph& graph)
		{
			auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                                [](VisualScriptNode const& node)
			                                {
												return node.Type == EntryType;
											});
			return found != graph.Nodes.end() ? &*found : nullptr;
		}

		// IDs.

		// The first of count new IDs: the document's largest plus one.
		Result<uint32_t> TakeIDs(VisualScriptDocument const& document, size_t count)
		{
			uint32_t const largest = VisualScriptEditing::GetLargestVisualScriptID(document);
			if (count > Format::MaxID || largest > Format::MaxID - count)
			{
				return MakeError("the document has no free ID left: IDs end at {}", Format::MaxID);
			}
			return largest + 1;
		}

		// Integer settings that name an element by ID, however the JSON stores the integer.
		bool NamesID(Json const& settings, char const* key, uint32_t id)
		{
			if (!settings.is_object())
			{
				return false;
			}
			auto const found = settings.find(key);
			if (found == settings.end() || !found->is_number_integer())
			{
				return false;
			}
			if (found->is_number_unsigned())
			{
				return found->get<uint64_t>() == id;
			}
			return found->get<int64_t>() == static_cast<int64_t>(id);
		}

		// A string setting; null when the node has none.
		std::string const* FindStringSetting(VisualScriptNode const& node, char const* key)
		{
			if (!node.Settings.is_object())
			{
				return nullptr;
			}
			auto const found = node.Settings.find(key);
			return found != node.Settings.end() && found->is_string() ? &found->get_ref<std::string const&>() : nullptr;
		}

		Result<void> CheckMembers(VisualScriptNode const& node)
		{
			if (!node.Settings.is_object() || !node.Inputs.is_object())
			{
				return MakeError("node {} has Settings or Inputs that are not objects", node.ID);
			}
			return {};
		}

		// Pins.

		std::vector<VisualScriptNodePin> GetPinRow(VisualScriptNodePins const& pins, uint32_t node)
		{
			return pins ? pins(node) : std::vector<VisualScriptNodePin>();
		}

		VisualScriptNodePin const* FindPin(std::span<VisualScriptNodePin const> row, std::string_view name,
		                                   VisualScriptPinDirection direction)
		{
			auto const found = std::find_if(row.begin(), row.end(),
			                                [name, direction](VisualScriptNodePin const& pin)
			                                {
												return pin.Direction == direction && pin.Name == name;
											});
			return found != row.end() ? &*found : nullptr;
		}

		bool IsExecPin(VisualScriptNodePin const& pin)
		{
			return pin.Type == VisualScriptExecType;
		}

		std::vector<std::string>& GetSplitList(VisualScriptNode& node, VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? node.Split.Inputs : node.Split.Outputs;
		}

		std::vector<std::string> const& GetSplitList(VisualScriptNode const& node, VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? node.Split.Inputs : node.Split.Outputs;
		}

		bool IsSplit(VisualScriptNode const& node, std::string_view path, VisualScriptPinDirection direction)
		{
			std::vector<std::string> const& split = GetSplitList(node, direction);
			return std::find(split.begin(), split.end(), path) != split.end();
		}

		// Whether a pin name names a field (at any depth) of the pin of a path: "Force.X" of "Force".
		bool IsUnder(std::string_view pin, std::string_view path)
		{
			return pin.size() > path.size() && pin.starts_with(path) && pin[path.size()] == '.';
		}

		bool IsAtOrUnder(std::string_view pin, std::string_view path)
		{
			return pin == path || IsUnder(pin, path);
		}

		// Whether a pin name of a node addresses a field of a pin the node does not split ("Force.X" while Force is not
		// split), which documents never hold. A Flow.Switch's case outputs are pins of their own whatever they hold.
		bool AddressesUnsplitField(VisualScriptNode const& node, std::string_view pin, VisualScriptPinDirection direction)
		{
			size_t const dot = pin.rfind('.');
			if (dot == std::string_view::npos ||
			    (direction == VisualScriptPinDirection::Out && node.Type == SwitchType && pin.starts_with(CasePrefix)))
			{
				return false;
			}
			return !IsSplit(node, pin.substr(0, dot), direction);
		}

		// Links.

		bool HasLink(std::span<VisualScriptLink const> links, VisualScriptLink const& link)
		{
			return std::find(links.begin(), links.end(), link) != links.end();
		}

		bool IsLinkedInput(std::span<VisualScriptLink const> links, uint32_t node, std::string_view pin)
		{
			return std::any_of(links.begin(), links.end(),
			                   [node, pin](VisualScriptLink const& link)
			                   {
								   return link.To.Node == node && link.To.Pin == pin;
							   });
		}

		// What a promoted input passes while nothing is linked to it, as a literal: its own, else its default; null for its
		// type's default. Refused for an unlinked input that means the entity running the graph, which no literal stands for.
		Result<Json> GetPromotedLiteral(std::span<VisualScriptLink const> links, VisualScriptNode const& node,
		                                VisualScriptNodePin const& pin)
		{
			if (auto const literal = node.Inputs.find(pin.Name); literal != node.Inputs.end())
			{
				return *literal;
			}
			if (pin.DefaultsToSelf && !IsLinkedInput(links, node.ID, pin.Name))
			{
				return MakeError("{} means the entity running the graph while nothing is linked to it: no value stands for that", pin.Name);
			}
			if (!IsValidValue(pin.Default))
			{
				return MakeValueError(fmt::format("the default of {}", pin.Name));
			}
			return pin.Default;
		}

		// The end of a link at a node in a direction: To for inputs, From for outputs.
		VisualScriptPinReference& GetEnd(VisualScriptLink& link, VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? link.To : link.From;
		}

		VisualScriptPinReference const& GetEnd(VisualScriptLink const& link, VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? link.To : link.From;
		}

		// Of links that are the same, the first stays (the loader drops the others). Returns how many went.
		uint32_t DropRepeatedLinks(std::vector<VisualScriptLink>& links)
		{
			std::set<std::tuple<uint32_t, std::string, uint32_t, std::string>> seen;
			size_t const before = links.size();
			std::erase_if(links,
			              [&seen](VisualScriptLink const& link)
			              {
							  return !seen.emplace(link.From.Node, link.From.Pin, link.To.Node, link.To.Pin).second;
						  });
			return static_cast<uint32_t>(before - links.size());
		}

		Result<void> CheckLinkCount(size_t count)
		{
			if (count > Format::MaxLinksPerGraph)
			{
				return MakeError("a graph holds at most {} links", Format::MaxLinksPerGraph);
			}
			return {};
		}

		// Checks a link Connect would add to a graph: its nodes are the graph's, are not one node, and have the pins it names
		// (fields only of split pins). Returns whether it links exec pins.
		Result<bool> CheckNewLink(VisualScriptGraph& graph, VisualScriptLink const& link, VisualScriptNodePins const& pins)
		{
			VisualScriptNode const* const from = FindNode(graph, link.From.Node);
			if (from == nullptr)
			{
				return MakeError("the graph has no node {}", link.From.Node);
			}
			VisualScriptNode const* const to = FindNode(graph, link.To.Node);
			if (to == nullptr)
			{
				return MakeError("the graph has no node {}", link.To.Node);
			}
			if (from == to)
			{
				return MakeError("a link cannot connect node {} to itself", from->ID);
			}
			// A split pin itself is no pin of the node: its fields are.
			std::vector<VisualScriptNodePin> const fromPins = GetPinRow(pins, from->ID);
			VisualScriptNodePin const* const output = FindPin(fromPins, link.From.Pin, VisualScriptPinDirection::Out);
			if (output == nullptr || !IsValidText(link.From.Pin, Format::MaxPinNameBytes) ||
			    AddressesUnsplitField(*from, link.From.Pin, VisualScriptPinDirection::Out) ||
			    IsSplit(*from, link.From.Pin, VisualScriptPinDirection::Out))
			{
				return MakeError("node {} has no output {}", from->ID, link.From.Pin);
			}
			std::vector<VisualScriptNodePin> const toPins = GetPinRow(pins, to->ID);
			VisualScriptNodePin const* const input = FindPin(toPins, link.To.Pin, VisualScriptPinDirection::In);
			if (input == nullptr || !IsValidText(link.To.Pin, Format::MaxPinNameBytes) ||
			    AddressesUnsplitField(*to, link.To.Pin, VisualScriptPinDirection::In) ||
			    IsSplit(*to, link.To.Pin, VisualScriptPinDirection::In))
			{
				return MakeError("node {} has no input {}", to->ID, link.To.Pin);
			}
			return IsExecPin(*output) || IsExecPin(*input);
		}

		// Adds a checked link: an exec link replaces the other links of its output, a data link those of its input. Returns
		// how many it replaced; a link the list has already changes nothing.
		uint32_t AddLink(std::vector<VisualScriptLink>& links, VisualScriptLink const& link, bool exec)
		{
			if (HasLink(links, link))
			{
				return 0;
			}
			size_t const before = links.size();
			std::erase_if(links,
			              [&link, exec](VisualScriptLink const& other)
			              {
							  return exec ? other.From == link.From : other.To == link.To;
						  });
			auto const replaced = static_cast<uint32_t>(before - links.size());
			links.push_back(link);
			return replaced;
		}

		// Removes the nodes a predicate picks from a graph, with their links. Returns how many it removed.
		template<typename TPredicate>
		uint32_t RemoveNodesIf(VisualScriptGraph& graph, TPredicate const& predicate)
		{
			std::unordered_set<uint32_t> removed;
			for (VisualScriptNode const& node : graph.Nodes)
			{
				if (predicate(node))
				{
					removed.insert(node.ID);
				}
			}
			if (removed.empty())
			{
				return 0;
			}
			std::erase_if(graph.Links,
			              [&removed](VisualScriptLink const& link)
			              {
							  return removed.contains(link.From.Node) || removed.contains(link.To.Node);
						  });
			std::erase_if(graph.Nodes,
			              [&removed](VisualScriptNode const& node)
			              {
							  return removed.contains(node.ID);
						  });
			return static_cast<uint32_t>(removed.size());
		}

		// Bridging removed nodes (RemoveElements).

		// The pins a removed node passes the flow through: what enters Input leaves at Output.
		struct Bridge
		{
			std::string Input;
			std::string Output;
		};

		// Where a link into a removed node continues among the kept nodes: a bridged node passes what enters its bridged input
		// to every target of its bridged output, through other removed nodes, each visited once. Targets come in link order.
		std::vector<VisualScriptPinReference> ResolveBridge(std::vector<VisualScriptLink> const& links,
		                                                    std::unordered_set<uint32_t> const& removed,
		                                                    std::unordered_map<uint32_t, Bridge> const& bridges,
		                                                    std::unordered_map<uint32_t, std::vector<size_t>> const& outgoing,
		                                                    VisualScriptPinReference const& target)
		{
			std::vector<VisualScriptPinReference> resolved;
			std::vector<VisualScriptPinReference> pending{target};
			std::unordered_set<uint32_t> visited;
			while (!pending.empty())
			{
				VisualScriptPinReference const current = std::move(pending.back());
				pending.pop_back();
				if (!removed.contains(current.Node))
				{
					resolved.push_back(current);
					continue;
				}
				auto const bridge = bridges.find(current.Node);
				if (bridge == bridges.end() || bridge->second.Input != current.Pin || !visited.insert(current.Node).second)
				{
					continue;
				}
				auto const leaving = outgoing.find(current.Node);
				if (leaving == outgoing.end())
				{
					continue;
				}
				for (auto index = leaving->second.rbegin(); index != leaving->second.rend(); ++index)
				{
					VisualScriptLink const& link = links[*index];
					if (link.From.Pin == bridge->second.Output)
					{
						pending.push_back(link.To);
					}
				}
			}
			return resolved;
		}

		// Parameters (of functions and custom events) and the pins named after them.

		Result<void> CheckParameters(std::span<VisualScriptParameter const> parameters, std::string_view list)
		{
			if (parameters.size() > Format::MaxParameters)
			{
				return MakeError("{} hold at most {} parameters", list, Format::MaxParameters);
			}
			for (size_t index = 0; index < parameters.size(); index++)
			{
				VisualScriptParameter const& parameter = parameters[index];
				if (!Format::IsValidName(parameter.Name))
				{
					return MakeNameError(parameter.Name);
				}
				if (VisualScriptEditing::IsReservedPinName(parameter.Name))
				{
					return MakeError("{} is a name of pins every node may have: a parameter cannot have it", parameter.Name);
				}
				for (size_t earlier = 0; earlier < index; earlier++)
				{
					if (parameters[earlier].Name == parameter.Name)
					{
						return MakeError("{} have two parameters named {}", list, parameter.Name);
					}
				}
				if (Result<void> type = CheckValueType(parameter.Type, "a parameter"); !type)
				{
					return type;
				}
			}
			return {};
		}

		std::vector<std::string> GetParameterNames(std::span<VisualScriptParameter const> parameters)
		{
			std::vector<std::string> names;
			names.reserve(parameters.size());
			for (VisualScriptParameter const& parameter : parameters)
			{
				names.push_back(parameter.Name);
			}
			return names;
		}

		// How the pins named after a list of parameters change with the list.
		struct ParameterChanges
		{
			// Pins that keep their links, literals and split pins under a new name: (old, new).
			std::vector<std::pair<std::string, std::string>> Renamed;
			// Pins whose links, literals and split pins go.
			std::vector<std::string> Removed;

			bool IsEmpty() const { return Renamed.empty() && Removed.empty(); }

			// The new name of a pin (or of a field of one), or nothing when it keeps it.
			std::optional<std::string> Rename(std::string const& pin) const
			{
				for (auto const& [from, to] : Renamed)
				{
					if (IsAtOrUnder(pin, from))
					{
						return to + pin.substr(from.size());
					}
				}
				return std::nullopt;
			}

			bool IsRemoved(std::string_view pin) const
			{
				return std::any_of(Removed.begin(), Removed.end(),
				                   [pin](std::string const& removed)
				                   {
									   return IsAtOrUnder(pin, removed);
								   });
			}
		};

		// A parameter kept (by name) keeps its pins. When the list keeps its length, the k-th name that went becomes the k-th
		// that came; the other names that went lose their pins.
		ParameterChanges CompareParameters(std::vector<std::string> before, std::vector<std::string> const& after)
		{
			std::vector<std::string> unique;
			for (std::string& name : before)
			{
				if (std::find(unique.begin(), unique.end(), name) == unique.end())
				{
					unique.push_back(std::move(name));
				}
			}
			std::vector<std::string> went;
			for (std::string const& name : unique)
			{
				if (std::find(after.begin(), after.end(), name) == after.end())
				{
					went.push_back(name);
				}
			}
			std::vector<std::string> came;
			for (std::string const& name : after)
			{
				if (std::find(unique.begin(), unique.end(), name) == unique.end())
				{
					came.push_back(name);
				}
			}
			ParameterChanges changes;
			size_t const renamed = unique.size() == after.size() ? std::min(went.size(), came.size()) : 0;
			for (size_t index = 0; index < went.size(); index++)
			{
				if (index < renamed)
				{
					changes.Renamed.emplace_back(went[index], came[index]);
				}
				else
				{
					changes.Removed.push_back(went[index]);
				}
			}
			return changes;
		}

		// Renames and removes the pins of parameters on a node in a direction: its links there, and the literals and split
		// entries of the pins with the paths of their fields. A renamed literal replaces one stored under its new name. Returns
		// how many links it removed.
		uint32_t ApplyParameterChanges(std::vector<VisualScriptLink>& links, VisualScriptNode& node, VisualScriptPinDirection direction,
		                               ParameterChanges const& changes)
		{
			if (changes.IsEmpty())
			{
				return 0;
			}
			size_t const before = links.size();
			std::erase_if(links,
			              [&node, direction, &changes](VisualScriptLink const& link)
			              {
							  VisualScriptPinReference const& end = GetEnd(link, direction);
							  return end.Node == node.ID && changes.IsRemoved(end.Pin);
						  });
			auto const removed = static_cast<uint32_t>(before - links.size());
			for (VisualScriptLink& link : links)
			{
				VisualScriptPinReference& end = GetEnd(link, direction);
				if (end.Node == node.ID)
				{
					if (std::optional<std::string> name = changes.Rename(end.Pin))
					{
						end.Pin = std::move(*name);
					}
				}
			}

			if (direction == VisualScriptPinDirection::In && node.Inputs.is_object())
			{
				Json inputs = Json::object();
				for (auto const& [name, value] : node.Inputs.items())
				{
					if (changes.IsRemoved(name))
					{
						continue;
					}
					if (std::optional<std::string> const renamed = changes.Rename(name))
					{
						inputs[*renamed] = value;
					}
					else if (!inputs.contains(name))
					{
						inputs[name] = value;
					}
				}
				node.Inputs = std::move(inputs);
			}

			std::vector<std::string>& split = GetSplitList(node, direction);
			std::vector<std::string> entries;
			entries.reserve(split.size());
			for (std::string& entry : split)
			{
				if (changes.IsRemoved(entry))
				{
					continue;
				}
				std::optional<std::string> renamed = changes.Rename(entry);
				std::string path = renamed ? std::move(*renamed) : std::move(entry);
				if (std::find(entries.begin(), entries.end(), path) == entries.end())
				{
					entries.push_back(std::move(path));
				}
			}
			split = std::move(entries);
			return removed;
		}

		// The names of a Parameters setting's entries ({ "Name": ..., "Type": ... } objects) that have a name.
		std::vector<std::string> ReadParameterNames(VisualScriptNode const& node)
		{
			std::vector<std::string> names;
			if (!node.Settings.is_object())
			{
				return names;
			}
			auto const found = node.Settings.find(ParametersSetting);
			if (found == node.Settings.end() || !found->is_array())
			{
				return names;
			}
			for (Json const& parameter : *found)
			{
				if (!parameter.is_object())
				{
					continue;
				}
				auto const name = parameter.find(NameSetting);
				if (name != parameter.end() && name->is_string())
				{
					names.push_back(name->get<std::string>());
				}
			}
			return names;
		}

		// Switch cases and sequence outputs.

		// What a Switch's cases are: integers (Type int), strings (Type string or an enum's enumerator names), or either while
		// its Type is not known.
		enum class CaseKind : uint8_t
		{
			Integer = 0,
			Text,
			Any
		};

		CaseKind GetCaseKind(VisualScriptNode const& node)
		{
			std::string const* const type = FindStringSetting(node, TypeSetting);
			if (type == nullptr || !IsValidText(*type, Format::MaxTypeBytes))
			{
				return CaseKind::Any;
			}
			return *type == "int" ? CaseKind::Integer : CaseKind::Text;
		}

		// A Switch on an enum: without Cases it has a case for every enumerator.
		bool SwitchesOnEnum(VisualScriptNode const& node)
		{
			std::string const* const type = FindStringSetting(node, TypeSetting);
			return type != nullptr && IsValidText(*type, Format::MaxTypeBytes) && *type != "int" && *type != "string";
		}

		// The output pin of a case value: "Case:" and an integer in decimal or a string as it is; nothing for other values.
		std::optional<std::string> GetCasePin(Json const& value)
		{
			if (value.is_number_unsigned())
			{
				return std::string(CasePrefix) + std::to_string(value.get<uint64_t>());
			}
			if (value.is_number_integer())
			{
				return std::string(CasePrefix) + std::to_string(value.get<int64_t>());
			}
			if (value.is_string() && !value.get_ref<std::string const&>().empty())
			{
				return std::string(CasePrefix) + value.get<std::string>();
			}
			return std::nullopt;
		}

		// The output pins of a Switch's stored Cases, in order; nothing when they are every enumerator (no Cases on an enum).
		std::optional<std::vector<std::string>> ReadCasePins(VisualScriptNode const& node)
		{
			auto const found = node.Settings.find(CasesSetting);
			if (found == node.Settings.end() || !found->is_array())
			{
				return SwitchesOnEnum(node) ? std::nullopt : std::optional<std::vector<std::string>>(std::vector<std::string>());
			}
			std::vector<std::string> pins;
			for (Json const& value : *found)
			{
				std::optional<std::string> pin = GetCasePin(value);
				if (pin && std::find(pins.begin(), pins.end(), *pin) == pins.end())
				{
					pins.push_back(std::move(*pin));
				}
			}
			return pins;
		}

		// The index of a Sequence's output pin ("Then3"); nothing for other pins.
		std::optional<int> ParseSequenceOutput(std::string_view pin)
		{
			if (!pin.starts_with(SequenceOutputPrefix))
			{
				return std::nullopt;
			}
			std::string_view const digits = pin.substr(SequenceOutputPrefix.size());
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

		std::string GetSequenceOutput(int index)
		{
			return std::string(SequenceOutputPrefix) + std::to_string(index);
		}

		// Struct literals (SplitPin, RecombinePin).

		// How many numbers make up a literal of the type when it is a number or a flat array of numbers built from the type's
		// fields: 1 for float, double, int and uint, the sum of its fields' for the structs with literals (vectors, quaternions,
		// colours and matrices). Nothing for every other type.
		std::optional<size_t> GetLiteralWidth(VisualScriptLibrary const& library, std::string_view typeId, size_t depth = 0)
		{
			VisualScriptTypeInfo const* const type = library.FindType(typeId);
			if (type == nullptr || depth > MaxSplitDepth)
			{
				return std::nullopt;
			}
			if (type->Kind == VisualScriptTypeKind::Float)
			{
				return 1;
			}
			if (type->Kind == VisualScriptTypeKind::Integer)
			{
				bool const number = type->FieldType == ScriptFieldType::Int32 || type->FieldType == ScriptFieldType::UInt32;
				return number ? std::optional<size_t>(1) : std::nullopt;
			}
			if (!type->HasLiteral || type->Fields.empty())
			{
				return std::nullopt;
			}
			size_t width = 0;
			for (VisualScriptStructField const& field : type->Fields)
			{
				std::optional<size_t> const fieldWidth = GetLiteralWidth(library, field.Type, depth + 1);
				if (!fieldWidth)
				{
					return std::nullopt;
				}
				width += *fieldWidth;
			}
			return width;
		}

		// The literals of a struct's fields from a literal of the struct: its numbers in field order, a field of width 1 taking
		// one number and a wider field an array of its numbers, each as stored. Nothing when the literal is not an array of
		// the struct's width in numbers.
		std::optional<std::vector<Json>> SplitLiteral(VisualScriptLibrary const& library, VisualScriptTypeInfo const& type,
		                                              Json const& literal)
		{
			std::optional<size_t> const width = GetLiteralWidth(library, type.ID);
			if (!width || *width < 2 || !literal.is_array() || literal.size() != *width ||
			    !std::all_of(
					literal.begin(), literal.end(),
					[](Json const& component)
					{
						return component.is_number();
					}))
			{
				return std::nullopt;
			}
			std::vector<Json> fields;
			size_t offset = 0;
			for (VisualScriptStructField const& field : type.Fields)
			{
				size_t const fieldWidth = GetLiteralWidth(library, field.Type).value_or(1);
				if (fieldWidth == 1)
				{
					fields.push_back(literal[offset]);
				}
				else
				{
					Json part = Json::array();
					for (size_t index = 0; index < fieldWidth; index++)
					{
						part.push_back(literal[offset + index]);
					}
					fields.push_back(std::move(part));
				}
				offset += fieldWidth;
			}
			return fields;
		}

		// Appends the numbers of a field's literal to a struct literal being built: a number for a field of width 1, an array of
		// width numbers otherwise. False when the literal is not of that form.
		bool AppendFieldLiteral(Json& combined, Json const& literal, size_t width)
		{
			if (width == 1)
			{
				if (!literal.is_number())
				{
					return false;
				}
				combined.push_back(literal);
				return true;
			}
			if (!literal.is_array() || literal.size() != width ||
			    !std::all_of(literal.begin(), literal.end(),
			                 [](Json const& component)
			                 {
								 return component.is_number();
							 }))
			{
				return false;
			}
			for (Json const& component : literal)
			{
				combined.push_back(component);
			}
			return true;
		}

		// The literal of a split input built back from its fields' literals (a field split again built first): the numbers of
		// every field in order. Nothing when a field is linked or has no literal of its form, or when the struct has none.
		std::optional<Json> CombineFieldLiterals(VisualScriptLibrary const& library, std::span<VisualScriptLink const> links,
		                                         VisualScriptNode const& node, std::string const& path, VisualScriptTypeInfo const& type,
		                                         size_t depth)
		{
			if (depth > MaxSplitDepth || !GetLiteralWidth(library, type.ID) || type.Fields.empty() || !node.Inputs.is_object())
			{
				return std::nullopt;
			}
			Json combined = Json::array();
			for (VisualScriptStructField const& field : type.Fields)
			{
				std::string const fieldPath = path + "." + field.Name;
				if (IsSplit(node, fieldPath, VisualScriptPinDirection::In))
				{
					VisualScriptTypeInfo const* const fieldType = library.FindType(field.Type);
					std::optional<Json> const fieldLiteral =
						fieldType != nullptr ? CombineFieldLiterals(library, links, node, fieldPath, *fieldType, depth + 1) : std::nullopt;
					if (!fieldLiteral)
					{
						return std::nullopt;
					}
					for (Json const& component : *fieldLiteral)
					{
						combined.push_back(component);
					}
					continue;
				}
				auto const literal = node.Inputs.find(fieldPath);
				std::optional<size_t> const fieldWidth = GetLiteralWidth(library, field.Type);
				if (IsLinkedInput(links, node.ID, fieldPath) || literal == node.Inputs.end() || !fieldWidth ||
				    !AppendFieldLiteral(combined, *literal, *fieldWidth))
				{
					return std::nullopt;
				}
			}
			return combined;
		}

		// The type the node's library template gives the pin of a path ("Hit", "Hit.Point"): the root pin's type when it is not
		// a wildcard, then the types of the fields the path names. Null when the template does not tell.
		VisualScriptTypeInfo const* FindTemplatePathType(VisualScriptLibrary const& library, VisualScriptNode const& node,
		                                                 std::string_view path, VisualScriptPinDirection direction)
		{
			VisualScriptNodeTemplate const* const kind = library.FindNode(node.Type);
			if (kind == nullptr)
			{
				return nullptr;
			}
			size_t dot = path.find('.');
			VisualScriptPin const* const root = kind->FindPin(path.substr(0, dot), direction);
			if (root == nullptr || root->Type == VisualScriptWildcardType)
			{
				return nullptr;
			}
			VisualScriptTypeInfo const* type = library.FindType(root->Type);
			while (type != nullptr && dot != std::string_view::npos)
			{
				size_t const next = path.find('.', dot + 1);
				std::string_view const fieldName =
					path.substr(dot + 1, next == std::string_view::npos ? std::string_view::npos : next - dot - 1);
				auto const field = std::find_if(type->Fields.begin(), type->Fields.end(),
				                                [fieldName](VisualScriptStructField const& candidate)
				                                {
													return candidate.Name == fieldName;
												});
				type = field != type->Fields.end() ? library.FindType(field->Type) : nullptr;
				dot = next;
			}
			return type;
		}

		// The direct fields a pin row shows for a split path, in their order: each name with its type, or with no type for a
		// field split again (its own fields stand in its place).
		std::vector<std::pair<std::string, std::string>> GetShownFields(std::span<VisualScriptNodePin const> row, std::string_view path,
		                                                                VisualScriptPinDirection direction)
		{
			std::vector<std::pair<std::string, std::string>> fields;
			for (VisualScriptNodePin const& pin : row)
			{
				if (pin.Direction != direction || !IsUnder(pin.Name, path))
				{
					continue;
				}
				std::string_view const rest = std::string_view(pin.Name).substr(path.size() + 1);
				size_t const dot = rest.find('.');
				std::string name(rest.substr(0, dot));
				bool const listed = std::any_of(fields.begin(), fields.end(),
				                                [&name](std::pair<std::string, std::string> const& field)
				                                {
													return field.first == name;
												});
				if (!listed)
				{
					fields.emplace_back(std::move(name), dot == std::string_view::npos ? pin.Type : std::string());
				}
			}
			return fields;
		}

		// Whether a library type has exactly the fields a row shows for a split path: their names and types in order, a field
		// split again matching when its type has exactly the fields shown for it.
		bool HasShownFields(VisualScriptLibrary const& library, VisualScriptTypeInfo const& type, std::span<VisualScriptNodePin const> row,
		                    std::string const& path, VisualScriptPinDirection direction, size_t depth)
		{
			std::vector<std::pair<std::string, std::string>> const fields = GetShownFields(row, path, direction);
			if (depth > MaxSplitDepth || fields.size() != type.Fields.size())
			{
				return false;
			}
			for (size_t index = 0; index < fields.size(); index++)
			{
				auto const& [name, shownType] = fields[index];
				VisualScriptStructField const& field = type.Fields[index];
				if (name != field.Name)
				{
					return false;
				}
				if (!shownType.empty())
				{
					if (shownType != field.Type)
					{
						return false;
					}
					continue;
				}
				VisualScriptTypeInfo const* const fieldType = library.FindType(field.Type);
				if (fieldType == nullptr || !HasShownFields(library, *fieldType, row, path + "." + name, direction, depth + 1))
				{
					return false;
				}
			}
			return true;
		}

		// The struct type of a split pin: the one the node's template gives the pin, else every type of the library that splits
		// in the direction and has exactly the fields the row shows.
		std::vector<VisualScriptTypeInfo const*> FindSplitPinTypes(VisualScriptLibrary const& library, VisualScriptNode const& node,
		                                                           std::span<VisualScriptNodePin const> row, std::string const& path,
		                                                           VisualScriptPinDirection direction)
		{
			if (VisualScriptTypeInfo const* const type = FindTemplatePathType(library, node, path, direction))
			{
				return {type};
			}
			std::vector<VisualScriptTypeInfo const*> types;
			for (VisualScriptTypeInfo const& type : library.GetTypes())
			{
				bool const splits = direction == VisualScriptPinDirection::In ? type.Makeable : type.Breakable;
				if (splits && type.Fields.size() >= 2 && HasShownFields(library, type, row, path, direction, 0))
				{
					types.push_back(&type);
				}
			}
			return types;
		}

		// Variables and locals.

		// Why a variable (a new one, or the one at index replaced) could not have its name; nothing when it can.
		std::optional<std::string> FindVariableNameClash(VisualScriptDocument const& document, VisualScriptVariable const& variable,
		                                                 std::optional<size_t> index)
		{
			for (size_t other = 0; other < document.Variables.size(); other++)
			{
				if (other == index)
				{
					continue;
				}
				VisualScriptVariable const& existing = document.Variables[other];
				if (existing.Name == variable.Name)
				{
					return fmt::format("a variable is named {} already", variable.Name);
				}
				if (std::find(existing.FormerNames.begin(), existing.FormerNames.end(), variable.Name) != existing.FormerNames.end())
				{
					return fmt::format("{} is a former name of the variable {}", variable.Name, existing.Name);
				}
			}
			if (std::find(variable.FormerNames.begin(), variable.FormerNames.end(), variable.Name) != variable.FormerNames.end())
			{
				return fmt::format("{} is one of the variable's own former names: forget it first", variable.Name);
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				for (VisualScriptLocal const& local : function.Locals)
				{
					if (local.Name == variable.Name)
					{
						return fmt::format("a local of {} is named {}", function.Name, variable.Name);
					}
				}
			}
			return std::nullopt;
		}

		// The former names of a variable (the one at index replaced): valid names, at most MaxFormerNames, unique in the
		// document and none of them a variable's name.
		Result<void> CheckFormerNames(VisualScriptDocument const& document, VisualScriptVariable const& variable, size_t index)
		{
			if (variable.FormerNames.size() > Format::MaxFormerNames)
			{
				return MakeError("a variable keeps at most {} former names", Format::MaxFormerNames);
			}
			for (size_t entry = 0; entry < variable.FormerNames.size(); entry++)
			{
				std::string const& name = variable.FormerNames[entry];
				if (!Format::IsValidName(name))
				{
					return MakeNameError(name);
				}
				if (std::find(variable.FormerNames.begin(), variable.FormerNames.begin() + static_cast<std::ptrdiff_t>(entry), name) !=
				    variable.FormerNames.begin() + static_cast<std::ptrdiff_t>(entry))
				{
					return MakeError("{} is listed twice among the former names", name);
				}
				for (size_t other = 0; other < document.Variables.size(); other++)
				{
					if (other == index)
					{
						continue;
					}
					VisualScriptVariable const& existing = document.Variables[other];
					if (existing.Name == name)
					{
						return MakeError("the former name {} is the name of a variable", name);
					}
					if (std::find(existing.FormerNames.begin(), existing.FormerNames.end(), name) != existing.FormerNames.end())
					{
						return MakeError("{} is a former name of the variable {} already", name, existing.Name);
					}
				}
			}
			return {};
		}

		// Why a local (a new one, or the one at index replaced) of a function could not have its name; nothing when it can.
		std::optional<std::string> FindLocalNameClash(VisualScriptDocument const& document, VisualScriptFunction const& function,
		                                              std::string_view name, std::optional<size_t> index)
		{
			for (size_t other = 0; other < function.Locals.size(); other++)
			{
				if (other != index && function.Locals[other].Name == name)
				{
					return fmt::format("a local of {} is named {} already", function.Name, name);
				}
			}
			bool const variable = std::any_of(document.Variables.begin(), document.Variables.end(),
			                                  [name](VisualScriptVariable const& existing)
			                                  {
												  return existing.Name == name;
											  });
			if (variable)
			{
				return fmt::format("a variable is named {}", name);
			}
			return std::nullopt;
		}

		// Whether a public variable of the type can be stored on components: an exposable type, or an enum (which only the
		// scripting runtime knows: without a lookup, any type that could be one).
		bool CanBePublic(std::string_view type, VisualScriptEnumLookup const& enums)
		{
			if (FindVisualScriptExposableType(type))
			{
				return true;
			}
			if (enums)
			{
				std::optional<VisualScriptEnumInfo> const info = enums(type);
				return info && info->IsUsable();
			}
			return CanBeVisualScriptEnum(type);
		}

		// Whether a value the type changes to keeps it: it decodes as the new type.
		bool KeepsValue(Json const& value, std::string_view type, VisualScriptEnumLookup const& enums)
		{
			return value.is_null() || DecodeVisualScriptLiteral(value, type, enums).has_value();
		}

		// Elements of a graph as rectangles (align, distribute, comment frames).

		struct ElementRect
		{
			uint32_t ID = 0;
			// Null for a comment.
			VisualScriptNode* Node = nullptr;
			VisualScriptComment* Comment = nullptr;
			glm::vec2 Position{0.0f};
			glm::vec2 Size{0.0f};
		};

		// The rectangles of nodes and comments of a graph, in the order given, each once: nodes measured by extents (which must
		// be placed), comments by their size.
		Result<std::vector<ElementRect>> GetElementRects(VisualScriptGraph& graph, std::span<uint32_t const> elements,
		                                                 VisualScriptNodeExtents const& extents)
		{
			ElementLookup const lookup(graph, elements);
			std::vector<ElementRect> rects;
			rects.reserve(elements.size());
			std::unordered_set<uint32_t> listed;
			for (uint32_t const id : elements)
			{
				if (!listed.insert(id).second)
				{
					continue;
				}
				if (VisualScriptNode* const node = lookup.FindNode(id))
				{
					if (!node->Position)
					{
						return MakeError("node {} has no position yet", id);
					}
					if (!extents)
					{
						return MakeError("the size of node {} is not known", id);
					}
					glm::vec2 const size = extents(id);
					if (!IsFinite(size) || size.x < 0.0f || size.y < 0.0f)
					{
						return MakeError("the size of node {} is not a size", id);
					}
					rects.push_back({id, node, nullptr, *node->Position, size});
					continue;
				}
				if (VisualScriptComment* const comment = lookup.FindComment(id))
				{
					rects.push_back({id, nullptr, comment, comment->Position, comment->Size});
					continue;
				}
				return MakeError("the graph has no node or comment {}", id);
			}
			return rects;
		}

		void SetRectPosition(ElementRect const& rect, glm::vec2 position)
		{
			if (rect.Node != nullptr)
			{
				rect.Node->Position = position;
			}
			else
			{
				rect.Comment->Position = position;
			}
		}
	}

	namespace VisualScriptEditing
	{
		std::string MakeUniqueVisualScriptName(std::string_view base, std::function<bool(std::string_view)> const& taken)
		{
			std::string const name = SanitizeName(base);
			if (!taken || !taken(name))
			{
				return name;
			}
			for (uint64_t number = 1;; number++)
			{
				std::string const suffix = std::to_string(number);
				std::string candidate(TrimSpaces(CutAtCharacter(name, Format::MaxNameBytes - suffix.size())));
				candidate += suffix;
				if (!taken(candidate))
				{
					return candidate;
				}
			}
		}

		bool IsVariableNameTaken(VisualScriptDocument const& document, std::string_view name)
		{
			for (VisualScriptVariable const& variable : document.Variables)
			{
				if (variable.Name == name ||
				    std::find(variable.FormerNames.begin(), variable.FormerNames.end(), name) != variable.FormerNames.end())
				{
					return true;
				}
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				for (VisualScriptLocal const& local : function.Locals)
				{
					if (local.Name == name)
					{
						return true;
					}
				}
			}
			return false;
		}

		bool IsLocalNameTaken(VisualScriptDocument const& document, uint32_t function, std::string_view name)
		{
			if (VisualScriptFunction const* const owner = FindFunction(document, function))
			{
				for (VisualScriptLocal const& local : owner->Locals)
				{
					if (local.Name == name)
					{
						return true;
					}
				}
			}
			return std::any_of(document.Variables.begin(), document.Variables.end(),
			                   [name](VisualScriptVariable const& variable)
			                   {
								   return variable.Name == name;
							   });
		}

		bool IsParameterNameTaken(std::span<VisualScriptParameter const> parameters, std::string_view name)
		{
			return IsReservedPinName(name) || std::any_of(parameters.begin(), parameters.end(),
			                                              [name](VisualScriptParameter const& parameter)
			                                              {
															  return parameter.Name == name;
														  });
		}

		bool IsFunctionNameTaken(VisualScriptDocument const& document, std::string_view name)
		{
			return std::any_of(document.Functions.begin(), document.Functions.end(),
			                   [name](VisualScriptFunction const& function)
			                   {
								   return function.Name == name;
							   });
		}

		bool IsCustomEventNameTaken(VisualScriptDocument const& document, std::string_view name)
		{
			return std::any_of(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
			                   [name](VisualScriptNode const& node)
			                   {
								   std::string const* const eventName = FindStringSetting(node, NameSetting);
								   return node.Type == CustomEventType && eventName != nullptr && *eventName == name;
							   });
		}

		bool IsReservedPinName(std::string_view name)
		{
			return std::find(ReservedPinNames.begin(), ReservedPinNames.end(), name) != ReservedPinNames.end();
		}

		bool IsValidVisualScriptText(std::string_view text, size_t maxBytes)
		{
			return IsValidText(text, maxBytes);
		}

		bool IsValidVisualScriptPinPath(std::string_view path)
		{
			return IsValidPinPath(path);
		}

		std::string ToStoredVisualScriptPin(VisualScriptAnalyzedNode const* analyzed, VisualScriptPinDirection direction,
		                                    std::string_view name)
		{
			if (analyzed == nullptr)
			{
				return std::string(name);
			}
			VisualScriptPinRename const* field = nullptr;
			for (VisualScriptPinRename const& rename : analyzed->PinRenames)
			{
				if (rename.Direction != direction)
				{
					continue;
				}
				if (rename.To == name)
				{
					return rename.From;
				}
				bool const isField = name.size() > rename.To.size() && name.starts_with(rename.To) && name[rename.To.size()] == '.';
				if (isField && (field == nullptr || rename.To.size() > field->To.size()))
				{
					field = &rename;
				}
			}
			if (field == nullptr)
			{
				return std::string(name);
			}
			return field->From + std::string(name.substr(field->To.size()));
		}

		uint32_t GetLargestVisualScriptID(VisualScriptDocument const& document)
		{
			uint32_t largest = 0;
			auto const takeGraph = [&largest](VisualScriptGraph const& graph)
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					largest = std::max(largest, node.ID);
				}
				for (VisualScriptComment const& comment : graph.Comments)
				{
					largest = std::max(largest, comment.ID);
				}
			};
			for (VisualScriptVariable const& variable : document.Variables)
			{
				largest = std::max(largest, variable.ID);
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				largest = std::max(largest, function.ID);
				for (VisualScriptLocal const& local : function.Locals)
				{
					largest = std::max(largest, local.ID);
				}
				takeGraph(function.Graph);
			}
			takeGraph(document.Graph);
			return largest;
		}

		Result<uint32_t> AddNode(VisualScriptDocument& document, uint32_t function, std::string type, Json settings, glm::vec2 position)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			if (!IsValidText(type, Format::MaxNodeTypeBytes))
			{
				return MakeError("{}: expected a node type of 1 to {} bytes without control characters", Quote(type),
				                 Format::MaxNodeTypeBytes);
			}
			if (settings.is_null())
			{
				settings = Json::object();
			}
			if (!settings.is_object() || settings.size() > Format::MaxSettingsMembers)
			{
				return MakeError("the settings of a node are an object of at most {} members", Format::MaxSettingsMembers);
			}
			for (auto const& [name, value] : settings.items())
			{
				if (!IsValidText(name, Format::MaxPinNameBytes))
				{
					return MakeError("{}: expected a setting name of 1 to {} bytes without control characters", Quote(name),
					                 Format::MaxPinNameBytes);
				}
				if (!IsValidValue(value))
				{
					return MakeValueError(fmt::format("the setting {}", name));
				}
			}
			if (!IsFinite(position))
			{
				return Error{"a node's position must be finite"};
			}
			if (graph.Nodes.size() >= Format::MaxNodesPerGraph)
			{
				return MakeError("a graph holds at most {} nodes", Format::MaxNodesPerGraph);
			}
			Result<uint32_t> id = TakeIDs(document, 1);
			if (!id)
			{
				return id;
			}

			VisualScriptNode node;
			node.ID = id.GetValue();
			node.Type = std::move(type);
			node.Position = ClampPosition(position);
			node.Settings = std::move(settings);
			graph.Nodes.push_back(std::move(node));
			return id;
		}

		Result<void> SpliceNode(VisualScriptDocument& document, uint32_t function, uint32_t node, VisualScriptLink const& link,
		                        std::string const& inputPin, std::string const& outputPin, VisualScriptNodePins const& pins)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			if (!HasLink(graph.Links, link))
			{
				return MakeError("the graph has no link from {} of node {} to {} of node {}", link.From.Pin, link.From.Node, link.To.Pin,
				                 link.To.Node);
			}
			if (FindNode(graph, node) == nullptr)
			{
				return MakeError("the graph has no node {}", node);
			}
			if (node == link.From.Node || node == link.To.Node)
			{
				return MakeError("node {} is an end of the link it would go into", node);
			}
			VisualScriptLink const into{link.From, {node, inputPin}};
			VisualScriptLink const out{{node, outputPin}, link.To};
			Result<bool> const intoExec = CheckNewLink(graph, into, pins);
			if (!intoExec)
			{
				return Error{intoExec.GetError()};
			}
			Result<bool> const outExec = CheckNewLink(graph, out, pins);
			if (!outExec)
			{
				return Error{outExec.GetError()};
			}

			std::vector<VisualScriptLink> links = graph.Links;
			std::erase(links, link);
			AddLink(links, into, intoExec.GetValue());
			AddLink(links, out, outExec.GetValue());
			if (Result<void> count = CheckLinkCount(links.size()); !count)
			{
				return count;
			}
			graph.Links = std::move(links);
			return {};
		}

		Result<VisualScriptRemoval> RemoveElements(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> elements,
		                                           VisualScriptNodePins const* pins, bool bridge)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();

			VisualScriptRemoval removal;
			VisualScriptNode const* const entry = function != 0 ? FindEntry(graph) : nullptr;
			ElementLookup const lookup(graph, elements);
			std::unordered_set<uint32_t> nodes;
			std::unordered_set<uint32_t> comments;
			for (uint32_t const id : elements)
			{
				if (lookup.FindNode(id) != nullptr)
				{
					if (entry != nullptr && id == entry->ID)
					{
						if (removal.Notes.empty())
						{
							VisualScriptFunction const* const owner = FindFunction(document, function);
							removal.Notes.push_back(
								fmt::format("The entry of {} stays: to remove the function, delete it in My Script", owner->Name));
						}
						continue;
					}
					nodes.insert(id);
				}
				else if (lookup.FindComment(id) != nullptr)
				{
					comments.insert(id);
				}
				else
				{
					return MakeError("the graph has no node or comment {}", id);
				}
			}

			std::unordered_map<uint32_t, Bridge> bridges;
			if (bridge)
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					if (!nodes.contains(node.ID))
					{
						continue;
					}
					if (node.Type == RerouteType)
					{
						bridges.emplace(node.ID, Bridge{"In", "Out"});
						continue;
					}
					if (pins == nullptr)
					{
						continue;
					}
					std::vector<VisualScriptNodePin> const row = GetPinRow(*pins, node.ID);
					VisualScriptNodePin const* input = nullptr;
					VisualScriptNodePin const* output = nullptr;
					size_t inputs = 0;
					size_t outputs = 0;
					for (VisualScriptNodePin const& pin : row)
					{
						if (!IsExecPin(pin))
						{
							continue;
						}
						if (pin.Direction == VisualScriptPinDirection::In)
						{
							input = &pin;
							inputs++;
						}
						else
						{
							output = &pin;
							outputs++;
						}
					}
					if (inputs == 1 && outputs == 1)
					{
						bridges.emplace(node.ID, Bridge{input->Name, output->Name});
					}
				}
			}
			std::unordered_map<uint32_t, std::vector<size_t>> outgoing;
			for (size_t index = 0; index < graph.Links.size(); index++)
			{
				if (bridges.contains(graph.Links[index].From.Node))
				{
					outgoing[graph.Links[index].From.Node].push_back(index);
				}
			}

			// Each link into a removed node from a kept one gives way to its bridged links, in its place.
			std::vector<VisualScriptLink> links;
			links.reserve(graph.Links.size());
			for (VisualScriptLink const& link : graph.Links)
			{
				bool const fromRemoved = nodes.contains(link.From.Node);
				bool const toRemoved = nodes.contains(link.To.Node);
				if (!fromRemoved && !toRemoved)
				{
					links.push_back(link);
					continue;
				}
				if (fromRemoved || bridges.empty())
				{
					continue;
				}
				for (VisualScriptPinReference const& target : ResolveBridge(graph.Links, nodes, bridges, outgoing, link.To))
				{
					links.push_back({link.From, target});
				}
			}
			DropRepeatedLinks(links);
			if (Result<void> count = CheckLinkCount(links.size()); !count)
			{
				return Error{count.GetError()};
			}

			graph.Links = std::move(links);
			removal.Nodes = static_cast<uint32_t>(std::erase_if(graph.Nodes,
			                                                    [&nodes](VisualScriptNode const& node)
			                                                    {
																	return nodes.contains(node.ID);
																}));
			removal.Comments = static_cast<uint32_t>(std::erase_if(graph.Comments,
			                                                       [&comments](VisualScriptComment const& comment)
			                                                       {
																	   return comments.contains(comment.ID);
																   }));
			return removal;
		}

		Result<void> MoveElements(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> elements, glm::vec2 offset)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			if (!IsFinite(offset))
			{
				return Error{"an offset must be finite"};
			}
			ElementLookup const lookup(graph, elements);
			std::vector<VisualScriptNode*> nodes;
			std::vector<VisualScriptComment*> comments;
			std::unordered_set<uint32_t> listed;
			for (uint32_t const id : elements)
			{
				if (VisualScriptNode* const node = lookup.FindNode(id))
				{
					if (!node->Position)
					{
						return MakeError("node {} has no position yet", id);
					}
					if (listed.insert(id).second)
					{
						nodes.push_back(node);
					}
				}
				else if (VisualScriptComment* const comment = lookup.FindComment(id))
				{
					if (listed.insert(id).second)
					{
						comments.push_back(comment);
					}
				}
				else
				{
					return MakeError("the graph has no node or comment {}", id);
				}
			}
			for (VisualScriptNode* const node : nodes)
			{
				node->Position = ClampPosition(*node->Position + offset);
			}
			for (VisualScriptComment* const comment : comments)
			{
				comment->Position = ClampPosition(comment->Position + offset);
			}
			return {};
		}

		Result<void> SetPositions(VisualScriptDocument& document, uint32_t function,
		                          std::span<std::pair<uint32_t, glm::vec2> const> positions)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			std::vector<uint32_t> ids;
			ids.reserve(positions.size());
			for (auto const& [id, position] : positions)
			{
				ids.push_back(id);
			}
			ElementLookup const lookup(graph, ids);
			for (auto const& [id, position] : positions)
			{
				if (lookup.FindNode(id) == nullptr && lookup.FindComment(id) == nullptr)
				{
					return MakeError("the graph has no node or comment {}", id);
				}
				if (!IsFinite(position))
				{
					return MakeError("the position of {} must be finite", id);
				}
			}
			for (auto const& [id, position] : positions)
			{
				if (VisualScriptNode* const node = lookup.FindNode(id))
				{
					node->Position = ClampPosition(position);
				}
				else
				{
					lookup.FindComment(id)->Position = ClampPosition(position);
				}
			}
			return {};
		}

		Result<uint32_t> Connect(VisualScriptDocument& document, uint32_t function, VisualScriptLink const& link,
		                         VisualScriptNodePins const& pins)
		{
			return Relink(document, function, {}, std::span<VisualScriptLink const>(&link, 1), pins);
		}

		Result<void> Disconnect(VisualScriptDocument& document, uint32_t function, std::span<VisualScriptLink const> links)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			for (VisualScriptLink const& link : links)
			{
				if (!HasLink(graph.Links, link))
				{
					return MakeError("the graph has no link from {} of node {} to {} of node {}", link.From.Pin, link.From.Node,
					                 link.To.Pin, link.To.Node);
				}
			}
			std::erase_if(graph.Links,
			              [links](VisualScriptLink const& link)
			              {
							  return HasLink(links, link);
						  });
			return {};
		}

		Result<uint32_t> Relink(VisualScriptDocument& document, uint32_t function, std::span<VisualScriptLink const> remove,
		                        std::span<VisualScriptLink const> add, VisualScriptNodePins const& pins)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			for (VisualScriptLink const& link : remove)
			{
				if (!HasLink(graph.Links, link))
				{
					return MakeError("the graph has no link from {} of node {} to {} of node {}", link.From.Pin, link.From.Node,
					                 link.To.Pin, link.To.Node);
				}
			}
			std::vector<bool> exec;
			exec.reserve(add.size());
			for (VisualScriptLink const& link : add)
			{
				Result<bool> const checked = CheckNewLink(graph, link, pins);
				if (!checked)
				{
					return Error{checked.GetError()};
				}
				exec.push_back(checked.GetValue());
			}

			std::vector<VisualScriptLink> links = graph.Links;
			std::erase_if(links,
			              [remove](VisualScriptLink const& link)
			              {
							  return HasLink(remove, link);
						  });
			uint32_t replaced = 0;
			for (size_t index = 0; index < add.size(); index++)
			{
				replaced += AddLink(links, add[index], exec[index]);
			}
			if (Result<void> count = CheckLinkCount(links.size()); !count)
			{
				return Error{count.GetError()};
			}
			graph.Links = std::move(links);
			return replaced;
		}

		Result<uint32_t> InsertReroute(VisualScriptDocument& document, uint32_t function, VisualScriptLink const& link, glm::vec2 position)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			if (!HasLink(graph.Links, link))
			{
				return MakeError("the graph has no link from {} of node {} to {} of node {}", link.From.Pin, link.From.Node, link.To.Pin,
				                 link.To.Node);
			}
			if (Result<void> count = CheckLinkCount(graph.Links.size() + 1); !count)
			{
				return Error{count.GetError()};
			}
			Result<uint32_t> id = AddNode(document, function, std::string(RerouteType), Json::object(), position);
			if (!id)
			{
				return id;
			}
			auto const place = std::find(graph.Links.begin(), graph.Links.end(), link);
			VisualScriptLink const out{{id.GetValue(), "Out"}, link.To};
			place->To = {id.GetValue(), "In"};
			graph.Links.insert(std::next(place), out);
			return id;
		}

		Result<void> SetInput(VisualScriptDocument& document, uint32_t node, std::string pin, Json literal)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptNode& target = *place.GetValue().Node;
			if (Result<void> members = CheckMembers(target); !members)
			{
				return members;
			}
			if (!IsValidText(pin, Format::MaxPinNameBytes))
			{
				return MakeError("{}: expected a pin name of 1 to {} bytes without control characters", Quote(pin),
				                 Format::MaxPinNameBytes);
			}
			if (literal.is_null())
			{
				target.Inputs.erase(pin);
				return {};
			}
			if (AddressesUnsplitField(target, pin, VisualScriptPinDirection::In))
			{
				return MakeError("{} is a field of an input node {} does not split", pin, node);
			}
			if (IsSplit(target, pin, VisualScriptPinDirection::In))
			{
				return MakeError("node {} splits its input {}: its fields take its literals", node, pin);
			}
			if (!IsValidValue(literal))
			{
				return MakeValueError("a literal");
			}
			if (!target.Inputs.contains(pin) && target.Inputs.size() >= Format::MaxSettingsMembers)
			{
				return MakeError("a node holds at most {} literals", Format::MaxSettingsMembers);
			}
			target.Inputs[std::move(pin)] = std::move(literal);
			return {};
		}

		Result<void> SetSetting(VisualScriptDocument& document, uint32_t node, std::string name, Json value)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptNode& target = *place.GetValue().Node;
			if (Result<void> members = CheckMembers(target); !members)
			{
				return members;
			}
			if (!IsValidText(name, Format::MaxPinNameBytes))
			{
				return MakeError("{}: expected a setting name of 1 to {} bytes without control characters", Quote(name),
				                 Format::MaxPinNameBytes);
			}
			bool const changesPins = (target.Type == SequenceType && name == CountSetting) ||
			                         (target.Type == SwitchType && name == CasesSetting) ||
			                         ((target.Type == CustomEventType || target.Type == SendEventType) && name == ParametersSetting) ||
			                         (target.Type == CustomEventType && name == NameSetting);
			if (changesPins)
			{
				return MakeError("{} of node {} ({}) changes its pins or its senders: set it with the operation that updates them too",
				                 name, node, target.Type);
			}
			if (value.is_null())
			{
				target.Settings.erase(name);
				return {};
			}
			if (!IsValidValue(value))
			{
				return MakeValueError(fmt::format("the setting {}", name));
			}
			if (!target.Settings.contains(name) && target.Settings.size() >= Format::MaxSettingsMembers)
			{
				return MakeError("a node holds at most {} settings", Format::MaxSettingsMembers);
			}
			target.Settings[std::move(name)] = std::move(value);
			return {};
		}

		Result<uint32_t> SetSequenceOutputs(VisualScriptDocument& document, uint32_t node, int count, std::optional<int> insertAt,
		                                    std::optional<int> removeAt)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptGraph& graph = *place.GetValue().Graph;
			VisualScriptNode& sequence = *place.GetValue().Node;
			if (sequence.Type != SequenceType)
			{
				return MakeError("node {} is no {}: its type is {}", node, SequenceType, sequence.Type);
			}
			if (Result<void> members = CheckMembers(sequence); !members)
			{
				return Error{members.GetError()};
			}
			if (count < MinSequenceOutputs || count > MaxSequenceOutputs)
			{
				return MakeError("a Sequence has {} to {} outputs", MinSequenceOutputs, MaxSequenceOutputs);
			}
			if (insertAt && removeAt)
			{
				return Error{"outputs are inserted or removed, not both at once"};
			}
			// The outputs the node has: Count, 2 when it is missing, and none known when it is not a count.
			std::optional<int> current;
			auto const stored = sequence.Settings.find(CountSetting);
			if (stored == sequence.Settings.end())
			{
				current = MinSequenceOutputs;
			}
			else if (stored->is_number_integer())
			{
				int64_t const value = stored->is_number_unsigned() ? static_cast<int64_t>(std::min<uint64_t>(stored->get<uint64_t>(), 1000))
				                                                   : stored->get<int64_t>();
				if (value >= MinSequenceOutputs && value <= MaxSequenceOutputs)
				{
					current = static_cast<int>(value);
				}
			}
			if ((insertAt || removeAt) && !current)
			{
				return MakeError("node {} has no Count of {} to {} to insert or remove outputs at", node, MinSequenceOutputs,
				                 MaxSequenceOutputs);
			}
			if (insertAt && (count <= *current || *insertAt < 0 || *insertAt > *current))
			{
				return MakeError("inserting outputs at {} takes a count above {} and an index from 0 to {}", insertAt.value_or(0),
				                 current.value_or(0), current.value_or(0));
			}
			if (removeAt && (count >= *current || *removeAt < 0 || *removeAt > count))
			{
				return MakeError("removing outputs at {} takes a count below {} and an index from 0 to {}", removeAt.value_or(0),
				                 current.value_or(0), count);
			}

			size_t const before = graph.Links.size();
			std::erase_if(graph.Links,
			              [node, count, current, removeAt, insertAt](VisualScriptLink const& link)
			              {
							  std::optional<int> const index = link.From.Node == node ? ParseSequenceOutput(link.From.Pin) : std::nullopt;
							  if (!index || insertAt)
							  {
								  return false;
							  }
							  if (removeAt)
							  {
								  return *index >= *removeAt && *index < *removeAt + (*current - count);
							  }
							  return *index >= count && (!current || *index < *current);
						  });
			auto removed = static_cast<uint32_t>(before - graph.Links.size());
			if (insertAt || removeAt)
			{
				int const first = insertAt ? *insertAt : *removeAt + (*current - count);
				int const shift = count - *current;
				for (VisualScriptLink& link : graph.Links)
				{
					std::optional<int> const index = link.From.Node == node ? ParseSequenceOutput(link.From.Pin) : std::nullopt;
					if (index && *index >= first && *index < *current)
					{
						link.From.Pin = GetSequenceOutput(*index + shift);
					}
				}
				removed += DropRepeatedLinks(graph.Links);
			}
			sequence.Settings[CountSetting] = count;
			return removed;
		}

		Result<uint32_t> SetSwitchCases(VisualScriptDocument& document, uint32_t node, Json cases)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptGraph& graph = *place.GetValue().Graph;
			VisualScriptNode& selector = *place.GetValue().Node;
			if (selector.Type != SwitchType)
			{
				return MakeError("node {} is no {}: its type is {}", node, SwitchType, selector.Type);
			}
			if (Result<void> members = CheckMembers(selector); !members)
			{
				return Error{members.GetError()};
			}
			if (!cases.is_null() && !cases.is_array())
			{
				return Error{"the cases of a Switch are an array"};
			}

			// The new cases' pins; nothing for every enumerator (no Cases on an enum).
			std::optional<std::vector<std::string>> after;
			if (cases.is_array())
			{
				CaseKind const kind = GetCaseKind(selector);
				after.emplace();
				for (Json const& value : cases)
				{
					bool const kindMatches =
						kind == CaseKind::Any || (kind == CaseKind::Integer ? value.is_number_integer() : value.is_string());
					std::optional<std::string> pin = kindMatches ? GetCasePin(value) : std::nullopt;
					if (!pin)
					{
						std::string_view const expected = kind == CaseKind::Integer ? "integers"
						                                  : kind == CaseKind::Text  ? "non-empty strings"
						                                                            : "integers or non-empty strings";
						return MakeError("{} is no case of this Switch: its cases are {}", DescribeValue(value), expected);
					}
					if (!IsValidText(*pin, Format::MaxPinNameBytes))
					{
						return MakeError("the case {} would name a pin of more than {} bytes or with control characters",
						                 Quote(pin->substr(CasePrefix.size())), Format::MaxPinNameBytes);
					}
					if (std::find(after->begin(), after->end(), *pin) != after->end())
					{
						return MakeError("the case {} is listed twice", pin->substr(CasePrefix.size()));
					}
					after->push_back(std::move(*pin));
				}
			}
			else if (!SwitchesOnEnum(selector))
			{
				after.emplace();
			}

			std::optional<std::vector<std::string>> const before = ReadCasePins(selector);
			uint32_t removed = 0;
			if (after)
			{
				ParameterChanges changes;
				if (before)
				{
					changes = CompareParameters(*before, *after);
				}
				else
				{
					// Every enumerator was a case: the case links that are not new cases went.
					for (VisualScriptLink const& link : graph.Links)
					{
						bool const isCase = link.From.Node == node && link.From.Pin.starts_with(CasePrefix);
						if (isCase && std::find(after->begin(), after->end(), link.From.Pin) == after->end() &&
						    std::find(changes.Removed.begin(), changes.Removed.end(), link.From.Pin) == changes.Removed.end())
						{
							changes.Removed.push_back(link.From.Pin);
						}
					}
				}
				// Case pins are names of their own ("Case:a.b" is no field of "Case:a"): only exact names change.
				size_t const linkCount = graph.Links.size();
				std::erase_if(graph.Links,
				              [node, &changes](VisualScriptLink const& link)
				              {
								  return link.From.Node == node &&
					                     std::find(changes.Removed.begin(), changes.Removed.end(), link.From.Pin) != changes.Removed.end();
							  });
				removed = static_cast<uint32_t>(linkCount - graph.Links.size());
				for (VisualScriptLink& link : graph.Links)
				{
					if (link.From.Node != node)
					{
						continue;
					}
					for (auto const& [from, to] : changes.Renamed)
					{
						if (link.From.Pin == from)
						{
							link.From.Pin = to;
							break;
						}
					}
				}
				removed += DropRepeatedLinks(graph.Links);
			}
			if (cases.is_null())
			{
				selector.Settings.erase(CasesSetting);
			}
			else
			{
				selector.Settings[CasesSetting] = std::move(cases);
			}
			return removed;
		}

		Result<uint32_t> SetEventParameters(VisualScriptDocument& document, uint32_t node, std::vector<VisualScriptParameter> parameters)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptGraph& graph = *place.GetValue().Graph;
			VisualScriptNode& event = *place.GetValue().Node;
			if (event.Type != CustomEventType && event.Type != SendEventType)
			{
				return MakeError("node {} is no {} or {}: its type is {}", node, CustomEventType, SendEventType, event.Type);
			}
			if (Result<void> members = CheckMembers(event); !members)
			{
				return Error{members.GetError()};
			}
			if (Result<void> checked = CheckParameters(parameters, "an event's parameters"); !checked)
			{
				return Error{checked.GetError()};
			}

			VisualScriptPinDirection const direction =
				event.Type == CustomEventType ? VisualScriptPinDirection::Out : VisualScriptPinDirection::In;
			ParameterChanges const changes = CompareParameters(ReadParameterNames(event), GetParameterNames(parameters));
			uint32_t removed = ApplyParameterChanges(graph.Links, event, direction, changes);
			removed += DropRepeatedLinks(graph.Links);
			Json list = Json::array();
			for (VisualScriptParameter& parameter : parameters)
			{
				Json entry = Json::object();
				entry[NameSetting] = std::move(parameter.Name);
				entry[TypeSetting] = std::move(parameter.Type);
				list.push_back(std::move(entry));
			}
			event.Settings[ParametersSetting] = std::move(list);
			return removed;
		}

		Result<uint32_t> RenameCustomEvent(VisualScriptDocument& document, uint32_t node, std::string const& name)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptNode& event = *place.GetValue().Node;
			if (event.Type != CustomEventType)
			{
				return MakeError("node {} is no {}: its type is {}", node, CustomEventType, event.Type);
			}
			if (Result<void> members = CheckMembers(event); !members)
			{
				return Error{members.GetError()};
			}
			if (!Format::IsValidName(name))
			{
				return MakeNameError(name);
			}
			std::string const* const current = FindStringSetting(event, NameSetting);
			if (current != nullptr && *current == name)
			{
				return 0u;
			}
			bool const taken =
				std::any_of(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
			                [&event, &name](VisualScriptNode const& other)
			                {
								std::string const* const otherName = FindStringSetting(other, NameSetting);
								return &other != &event && other.Type == CustomEventType && otherName != nullptr && *otherName == name;
							});
			if (taken)
			{
				return MakeError("a custom event is named {} already", name);
			}

			std::optional<std::string> const previous = current != nullptr ? std::optional<std::string>(*current) : std::nullopt;
			event.Settings[NameSetting] = name;
			uint32_t senders = 0;
			if (!previous)
			{
				return senders;
			}
			for (VisualScriptGraph* const graph : GetGraphs(document))
			{
				for (VisualScriptNode& sender : graph->Nodes)
				{
					std::string const* const sent = FindStringSetting(sender, NameSetting);
					if (sender.Type == SendEventType && sent != nullptr && *sent == *previous)
					{
						sender.Settings[NameSetting] = name;
						senders++;
					}
				}
			}
			return senders;
		}

		Result<void> SetNodeComment(VisualScriptDocument& document, std::span<uint32_t const> nodes, std::string const& comment)
		{
			if (!IsValidLongText(comment))
			{
				return MakeLongTextError("a node comment");
			}
			Result<std::vector<VisualScriptNode*>> found = GetNodes(document, nodes);
			if (!found)
			{
				return Error{found.GetError()};
			}
			for (VisualScriptNode* const node : found.GetValue())
			{
				node->Comment = comment;
			}
			return {};
		}

		Result<void> SetNodeCollapsed(VisualScriptDocument& document, std::span<uint32_t const> nodes, bool collapsed)
		{
			Result<std::vector<VisualScriptNode*>> found = GetNodes(document, nodes);
			if (!found)
			{
				return Error{found.GetError()};
			}
			for (VisualScriptNode* const node : found.GetValue())
			{
				node->Collapsed = collapsed;
			}
			return {};
		}

		Result<void> SetNodeEnabled(VisualScriptDocument& document, std::span<uint32_t const> nodes, VisualScriptNodeEnabled enabled)
		{
			if (VisualScriptNodeEnabledToString(enabled).empty())
			{
				return MakeError("{} is no enabled state", static_cast<int>(enabled));
			}
			Result<std::vector<VisualScriptNode*>> found = GetNodes(document, nodes);
			if (!found)
			{
				return Error{found.GetError()};
			}
			for (VisualScriptNode* const node : found.GetValue())
			{
				node->Enabled = enabled;
			}
			return {};
		}

		Result<uint32_t> SplitPin(VisualScriptDocument& document, uint32_t node, VisualScriptPinDirection direction,
		                          std::string const& path, VisualScriptNodePins const& pins, VisualScriptLibrary const& library)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptGraph& graph = *place.GetValue().Graph;
			VisualScriptNode& target = *place.GetValue().Node;
			if (Result<void> members = CheckMembers(target); !members)
			{
				return Error{members.GetError()};
			}
			if (IsSplit(target, path, direction))
			{
				return MakeError("node {} splits its {} {} already", node, DescribeDirection(direction), path);
			}
			std::vector<VisualScriptNodePin> const row = GetPinRow(pins, node);
			VisualScriptNodePin const* const pin = FindPin(row, path, direction);
			if (pin == nullptr || !IsValidPinPath(path) || AddressesUnsplitField(target, path, direction))
			{
				return MakeError("node {} has no {} {}", node, DescribeDirection(direction), path);
			}
			if (pin->Type == VisualScriptWildcardType)
			{
				return MakeError("the type of {} is not known yet", path);
			}
			VisualScriptTypeInfo const* const type = library.FindType(pin->Type);
			bool const splits = type != nullptr && (direction == VisualScriptPinDirection::In ? type->Makeable : type->Breakable);
			if (!splits || type->Fields.size() < 2)
			{
				return MakeError("{} ({}) cannot be split: an input splits when Make builds its type, an output when Break takes it apart, "
				                 "into two fields or more",
				                 path, pin->Type);
			}
			for (VisualScriptStructField const& field : type->Fields)
			{
				if (!IsValidPinPath(path + "." + field.Name))
				{
					return MakeError("the field pins of {} would have names of more than {} bytes", path, Format::MaxPinNameBytes);
				}
			}
			std::vector<std::string>& split = GetSplitList(target, direction);
			if (split.size() >= Format::MaxSplitPins)
			{
				return MakeError("a node splits at most {} pins of each direction", Format::MaxSplitPins);
			}

			// An input's literal becomes its fields' literals.
			std::optional<std::vector<Json>> fieldLiterals;
			bool const hasLiteral = direction == VisualScriptPinDirection::In && target.Inputs.contains(path);
			if (hasLiteral)
			{
				fieldLiterals = SplitLiteral(library, *type, target.Inputs[path]);
				size_t added = 0;
				if (fieldLiterals)
				{
					for (VisualScriptStructField const& field : type->Fields)
					{
						added += target.Inputs.contains(path + "." + field.Name) ? 0 : 1;
					}
				}
				if (target.Inputs.size() - 1 + added > Format::MaxSettingsMembers)
				{
					return MakeError("a node holds at most {} literals", Format::MaxSettingsMembers);
				}
			}

			if (hasLiteral)
			{
				target.Inputs.erase(path);
				if (fieldLiterals)
				{
					for (size_t index = 0; index < type->Fields.size(); index++)
					{
						target.Inputs[path + "." + type->Fields[index].Name] = std::move((*fieldLiterals)[index]);
					}
				}
			}
			size_t const before = graph.Links.size();
			std::erase_if(graph.Links,
			              [node, direction, &path](VisualScriptLink const& link)
			              {
							  VisualScriptPinReference const& end = GetEnd(link, direction);
							  return end.Node == node && end.Pin == path;
						  });
			split.push_back(path);
			return static_cast<uint32_t>(before - graph.Links.size());
		}

		Result<uint32_t> RecombinePin(VisualScriptDocument& document, uint32_t node, VisualScriptPinDirection direction,
		                              std::string const& path, VisualScriptNodePins const& pins, VisualScriptLibrary const& library)
		{
			Result<NodePlace> place = GetNode(document, node);
			if (!place)
			{
				return Error{place.GetError()};
			}
			VisualScriptGraph& graph = *place.GetValue().Graph;
			VisualScriptNode& target = *place.GetValue().Node;
			if (Result<void> members = CheckMembers(target); !members)
			{
				return Error{members.GetError()};
			}
			if (!IsSplit(target, path, direction))
			{
				return MakeError("node {} does not split its {} {}", node, DescribeDirection(direction), path);
			}

			// The fields' literals become the pin's when they make one literal of its struct, whichever the struct is.
			std::optional<Json> combined;
			if (direction == VisualScriptPinDirection::In)
			{
				std::vector<VisualScriptNodePin> const row = GetPinRow(pins, node);
				for (VisualScriptTypeInfo const* const type : FindSplitPinTypes(library, target, row, path, direction))
				{
					std::optional<Json> literal = CombineFieldLiterals(library, graph.Links, target, path, *type, 0);
					if (!literal || (combined && *combined != *literal))
					{
						combined.reset();
						break;
					}
					combined = std::move(literal);
				}
			}

			std::erase_if(GetSplitList(target, direction),
			              [&path](std::string const& entry)
			              {
							  return IsAtOrUnder(entry, path);
						  });
			size_t const before = graph.Links.size();
			std::erase_if(graph.Links,
			              [node, direction, &path](VisualScriptLink const& link)
			              {
							  VisualScriptPinReference const& end = GetEnd(link, direction);
							  return end.Node == node && IsUnder(end.Pin, path);
						  });
			if (direction == VisualScriptPinDirection::In)
			{
				Json inputs = Json::object();
				for (auto const& [name, value] : target.Inputs.items())
				{
					if (!IsUnder(name, path))
					{
						inputs[name] = value;
					}
				}
				if (combined)
				{
					inputs[path] = std::move(*combined);
				}
				target.Inputs = std::move(inputs);
			}
			return static_cast<uint32_t>(before - graph.Links.size());
		}

		Result<uint32_t> AddComment(VisualScriptDocument& document, uint32_t function, std::string text, std::span<uint32_t const> elements,
		                            VisualScriptNodeExtents const& extents, glm::vec2 position, glm::vec4 color)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			if (!IsValidLongText(text))
			{
				return MakeLongTextError("a comment's text");
			}
			if (!IsFinite(color))
			{
				return Error{"a comment's colour must be finite"};
			}
			if (graph.Comments.size() >= Format::MaxCommentsPerGraph)
			{
				return MakeError("a graph holds at most {} comments", Format::MaxCommentsPerGraph);
			}
			VisualScriptComment comment;
			comment.Text = std::move(text);
			comment.Color = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
			if (elements.empty())
			{
				if (!IsFinite(position))
				{
					return Error{"a comment's position must be finite"};
				}
				comment.Position = ClampPosition(position);
				comment.Size = DefaultCommentSize;
			}
			else
			{
				Result<std::vector<ElementRect>> rects = GetElementRects(graph, elements, extents);
				if (!rects)
				{
					return Error{rects.GetError()};
				}
				glm::vec2 minimum(std::numeric_limits<float>::max());
				glm::vec2 maximum(std::numeric_limits<float>::lowest());
				for (ElementRect const& rect : rects.GetValue())
				{
					minimum = glm::min(minimum, rect.Position);
					maximum = glm::max(maximum, rect.Position + rect.Size);
				}
				minimum -= glm::vec2(VisualScriptEditing::CommentFrameMargin,
				                     VisualScriptEditing::CommentFrameMargin + VisualScriptEditing::CommentFrameTitleSpace);
				maximum += glm::vec2(VisualScriptEditing::CommentFrameMargin);
				comment.Position = ClampPosition(minimum);
				comment.Size = glm::clamp(maximum - minimum, glm::vec2(MinCommentSize), glm::vec2(MaxCommentSize));
			}
			Result<uint32_t> id = TakeIDs(document, 1);
			if (!id)
			{
				return id;
			}
			comment.ID = id.GetValue();
			graph.Comments.push_back(std::move(comment));
			return id;
		}

		Result<void> SetCommentText(VisualScriptDocument& document, uint32_t comment, std::string text)
		{
			Result<VisualScriptComment*> found = GetComment(document, comment);
			if (!found)
			{
				return Error{found.GetError()};
			}
			if (!IsValidLongText(text))
			{
				return MakeLongTextError("a comment's text");
			}
			found.GetValue()->Text = std::move(text);
			return {};
		}

		Result<void> SetCommentGeometry(VisualScriptDocument& document, uint32_t comment, glm::vec2 position, glm::vec2 size)
		{
			Result<VisualScriptComment*> found = GetComment(document, comment);
			if (!found)
			{
				return Error{found.GetError()};
			}
			if (!IsFinite(position) || !IsFinite(size))
			{
				return Error{"a comment's position and size must be finite"};
			}
			found.GetValue()->Position = ClampPosition(position);
			found.GetValue()->Size = glm::clamp(size, glm::vec2(MinCommentSize), glm::vec2(MaxCommentSize));
			return {};
		}

		Result<void> SetCommentColor(VisualScriptDocument& document, uint32_t comment, glm::vec4 color)
		{
			Result<VisualScriptComment*> found = GetComment(document, comment);
			if (!found)
			{
				return Error{found.GetError()};
			}
			if (!IsFinite(color))
			{
				return Error{"a comment's colour must be finite"};
			}
			found.GetValue()->Color = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
			return {};
		}

		Result<void> SetDescription(VisualScriptDocument& document, std::string description)
		{
			if (!IsValidLongText(description))
			{
				return MakeLongTextError("a description");
			}
			document.Description = std::move(description);
			return {};
		}

		Result<uint32_t> AddVariable(VisualScriptDocument& document, std::string name, std::string type)
		{
			if (!Format::IsValidName(name))
			{
				return MakeNameError(name);
			}
			VisualScriptVariable variable;
			variable.Name = std::move(name);
			variable.Type = std::move(type);
			if (std::optional<std::string> clash = FindVariableNameClash(document, variable, std::nullopt))
			{
				return Error{std::move(*clash)};
			}
			if (Result<void> checked = CheckValueType(variable.Type, "a variable"); !checked)
			{
				return Error{checked.GetError()};
			}
			if (document.Variables.size() >= Format::MaxVariables)
			{
				return MakeError("a document holds at most {} variables", Format::MaxVariables);
			}
			Result<uint32_t> id = TakeIDs(document, 1);
			if (!id)
			{
				return id;
			}
			variable.ID = id.GetValue();
			document.Variables.push_back(std::move(variable));
			return id;
		}

		Result<bool> SetVariable(VisualScriptDocument& document, uint32_t id, VisualScriptVariable variable,
		                         VisualScriptEnumLookup const& enums)
		{
			auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
			                                [id](VisualScriptVariable const& existing)
			                                {
												return existing.ID == id;
											});
			if (found == document.Variables.end())
			{
				return MakeError("the document has no variable {}", id);
			}
			auto const index = static_cast<size_t>(std::distance(document.Variables.begin(), found));
			VisualScriptVariable const& current = *found;
			variable.ID = id;
			if (!Format::IsValidName(variable.Name))
			{
				return MakeNameError(variable.Name);
			}
			if (std::optional<std::string> clash = FindVariableNameClash(document, variable, index))
			{
				return Error{std::move(*clash)};
			}
			if (Result<void> checked = CheckValueType(variable.Type, "a variable"); !checked)
			{
				return Error{checked.GetError()};
			}
			if (!IsValidValue(variable.Value))
			{
				return MakeValueError("a variable's value");
			}
			if (!IsValidLongText(variable.Tooltip))
			{
				return MakeLongTextError("a tooltip");
			}
			if (variable.Range && (!IsFinite(*variable.Range) || variable.Range->x > variable.Range->y))
			{
				return Error{"a range is two finite numbers, the minimum first"};
			}
			if (Result<void> formerNames = CheckFormerNames(document, variable, index); !formerNames)
			{
				return Error{formerNames.GetError()};
			}
			bool const becomesPublic = variable.Public && (!current.Public || current.Type != variable.Type);
			if (becomesPublic && !CanBePublic(variable.Type, enums))
			{
				return MakeError("{} cannot be public: components cannot store it", variable.Type);
			}

			bool const cleared = variable.Type != current.Type && !KeepsValue(variable.Value, variable.Type, enums);
			if (cleared)
			{
				variable.Value = Json();
			}
			*found = std::move(variable);
			return cleared;
		}

		Result<uint32_t> RemoveVariable(VisualScriptDocument& document, uint32_t id, bool removeNodes)
		{
			size_t const before = document.Variables.size();
			std::erase_if(document.Variables,
			              [id](VisualScriptVariable const& variable)
			              {
							  return variable.ID == id;
						  });
			if (document.Variables.size() == before)
			{
				return MakeError("the document has no variable {}", id);
			}
			uint32_t removed = 0;
			if (removeNodes)
			{
				for (VisualScriptGraph* const graph : GetGraphs(document))
				{
					removed += RemoveNodesIf(*graph,
					                         [id](VisualScriptNode const& node)
					                         {
												 return (node.Type == GetVariableType || node.Type == SetVariableType) &&
						                                NamesID(node.Settings, VariableSetting, id);
											 });
				}
			}
			return removed;
		}

		Result<void> ReorderVariables(VisualScriptDocument& document, std::span<uint32_t const> order)
		{
			if (order.size() != document.Variables.size())
			{
				return MakeError("the order lists {} variables of {}", order.size(), document.Variables.size());
			}
			std::vector<VisualScriptVariable> variables;
			variables.reserve(order.size());
			for (uint32_t const id : order)
			{
				auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
				                                [id](VisualScriptVariable const& variable)
				                                {
													return variable.ID == id;
												});
				if (found == document.Variables.end())
				{
					return MakeError("the document has no variable {}", id);
				}
				bool const listed = std::any_of(variables.begin(), variables.end(),
				                                [id](VisualScriptVariable const& variable)
				                                {
													return variable.ID == id;
												});
				if (listed)
				{
					return MakeError("the order lists variable {} twice", id);
				}
				variables.push_back(*found);
			}
			document.Variables = std::move(variables);
			return {};
		}

		Result<uint32_t> DuplicateVariable(VisualScriptDocument& document, uint32_t id)
		{
			auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
			                                [id](VisualScriptVariable const& variable)
			                                {
												return variable.ID == id;
											});
			if (found == document.Variables.end())
			{
				return MakeError("the document has no variable {}", id);
			}
			if (document.Variables.size() >= Format::MaxVariables)
			{
				return MakeError("a document holds at most {} variables", Format::MaxVariables);
			}
			Result<uint32_t> newID = TakeIDs(document, 1);
			if (!newID)
			{
				return newID;
			}
			VisualScriptVariable copy = *found;
			copy.ID = newID.GetValue();
			copy.Name = MakeUniqueVisualScriptName(found->Name,
			                                       [&document](std::string_view name)
			                                       {
													   return IsVariableNameTaken(document, name);
												   });
			copy.FormerNames.clear();
			document.Variables.insert(std::next(found), std::move(copy));
			return newID;
		}

		Result<uint32_t> AddLocal(VisualScriptDocument& document, uint32_t function, std::string name, std::string type)
		{
			Result<VisualScriptFunction*> owner = GetFunction(document, function);
			if (!owner)
			{
				return Error{owner.GetError()};
			}
			VisualScriptFunction& target = *owner.GetValue();
			if (!Format::IsValidName(name))
			{
				return MakeNameError(name);
			}
			if (std::optional<std::string> clash = FindLocalNameClash(document, target, name, std::nullopt))
			{
				return Error{std::move(*clash)};
			}
			if (Result<void> checked = CheckValueType(type, "a local"); !checked)
			{
				return Error{checked.GetError()};
			}
			if (target.Locals.size() >= Format::MaxLocals)
			{
				return MakeError("a function holds at most {} locals", Format::MaxLocals);
			}
			Result<uint32_t> id = TakeIDs(document, 1);
			if (!id)
			{
				return id;
			}
			VisualScriptLocal local;
			local.ID = id.GetValue();
			local.Name = std::move(name);
			local.Type = std::move(type);
			target.Locals.push_back(std::move(local));
			return id;
		}

		Result<bool> SetLocal(VisualScriptDocument& document, uint32_t function, uint32_t id, VisualScriptLocal local,
		                      VisualScriptEnumLookup const& enums)
		{
			Result<VisualScriptFunction*> owner = GetFunction(document, function);
			if (!owner)
			{
				return Error{owner.GetError()};
			}
			VisualScriptFunction& target = *owner.GetValue();
			auto const found = std::find_if(target.Locals.begin(), target.Locals.end(),
			                                [id](VisualScriptLocal const& existing)
			                                {
												return existing.ID == id;
											});
			if (found == target.Locals.end())
			{
				return MakeError("{} has no local {}", target.Name, id);
			}
			local.ID = id;
			if (!Format::IsValidName(local.Name))
			{
				return MakeNameError(local.Name);
			}
			auto const index = static_cast<size_t>(std::distance(target.Locals.begin(), found));
			if (std::optional<std::string> clash = FindLocalNameClash(document, target, local.Name, index))
			{
				return Error{std::move(*clash)};
			}
			if (Result<void> checked = CheckValueType(local.Type, "a local"); !checked)
			{
				return Error{checked.GetError()};
			}
			if (!IsValidValue(local.Value))
			{
				return MakeValueError("a local's value");
			}
			bool const cleared = local.Type != found->Type && !KeepsValue(local.Value, local.Type, enums);
			if (cleared)
			{
				local.Value = Json();
			}
			*found = std::move(local);
			return cleared;
		}

		Result<uint32_t> RemoveLocal(VisualScriptDocument& document, uint32_t function, uint32_t id, bool removeNodes)
		{
			Result<VisualScriptFunction*> owner = GetFunction(document, function);
			if (!owner)
			{
				return Error{owner.GetError()};
			}
			VisualScriptFunction& target = *owner.GetValue();
			size_t const before = target.Locals.size();
			std::erase_if(target.Locals,
			              [id](VisualScriptLocal const& local)
			              {
							  return local.ID == id;
						  });
			if (target.Locals.size() == before)
			{
				return MakeError("{} has no local {}", target.Name, id);
			}
			if (!removeNodes)
			{
				return 0u;
			}
			return RemoveNodesIf(target.Graph,
			                     [id](VisualScriptNode const& node)
			                     {
									 return (node.Type == GetVariableType || node.Type == SetVariableType) &&
				                            NamesID(node.Settings, VariableSetting, id);
								 });
		}

		Result<uint32_t> AddFunction(VisualScriptDocument& document, std::string name)
		{
			if (!Format::IsValidName(name))
			{
				return MakeNameError(name);
			}
			if (IsFunctionNameTaken(document, name))
			{
				return MakeError("a function is named {} already", name);
			}
			if (document.Functions.size() >= Format::MaxFunctions)
			{
				return MakeError("a document holds at most {} functions", Format::MaxFunctions);
			}
			Result<uint32_t> id = TakeIDs(document, 3);
			if (!id)
			{
				return id;
			}
			VisualScriptFunction function;
			function.ID = id.GetValue();
			function.Name = std::move(name);
			VisualScriptNode entry;
			entry.ID = function.ID + 1;
			entry.Type = std::string(EntryType);
			entry.Position = glm::vec2(0.0f, 0.0f);
			VisualScriptNode exit;
			exit.ID = function.ID + 2;
			exit.Type = std::string(ReturnType);
			exit.Position = glm::vec2(480.0f, 0.0f);
			function.Graph.Links.push_back({{entry.ID, "Then"}, {exit.ID, "Execute"}});
			function.Graph.Nodes.push_back(std::move(entry));
			function.Graph.Nodes.push_back(std::move(exit));
			document.Functions.push_back(std::move(function));
			return id;
		}

		Result<void> RenameFunction(VisualScriptDocument& document, uint32_t function, std::string name)
		{
			Result<VisualScriptFunction*> owner = GetFunction(document, function);
			if (!owner)
			{
				return Error{owner.GetError()};
			}
			if (!Format::IsValidName(name))
			{
				return MakeNameError(name);
			}
			if (owner.GetValue()->Name != name && IsFunctionNameTaken(document, name))
			{
				return MakeError("a function is named {} already", name);
			}
			owner.GetValue()->Name = std::move(name);
			return {};
		}

		Result<uint32_t> SetFunctionSignature(VisualScriptDocument& document, uint32_t function, std::vector<VisualScriptParameter> inputs,
		                                      std::vector<VisualScriptParameter> outputs)
		{
			Result<VisualScriptFunction*> owner = GetFunction(document, function);
			if (!owner)
			{
				return Error{owner.GetError()};
			}
			VisualScriptFunction& target = *owner.GetValue();
			if (Result<void> checked = CheckParameters(inputs, "a function's inputs"); !checked)
			{
				return Error{checked.GetError()};
			}
			if (Result<void> checked = CheckParameters(outputs, "a function's outputs"); !checked)
			{
				return Error{checked.GetError()};
			}

			ParameterChanges const inputChanges = CompareParameters(GetParameterNames(target.Inputs), GetParameterNames(inputs));
			ParameterChanges const outputChanges = CompareParameters(GetParameterNames(target.Outputs), GetParameterNames(outputs));
			uint32_t removed = 0;
			for (VisualScriptNode& node : target.Graph.Nodes)
			{
				if (node.Type == EntryType)
				{
					removed += ApplyParameterChanges(target.Graph.Links, node, VisualScriptPinDirection::Out, inputChanges);
				}
				else if (node.Type == ReturnType)
				{
					removed += ApplyParameterChanges(target.Graph.Links, node, VisualScriptPinDirection::In, outputChanges);
				}
			}
			for (VisualScriptGraph* const graph : GetGraphs(document))
			{
				for (VisualScriptNode& node : graph->Nodes)
				{
					if (node.Type == CallType && NamesID(node.Settings, FunctionSetting, function))
					{
						removed += ApplyParameterChanges(graph->Links, node, VisualScriptPinDirection::In, inputChanges);
						removed += ApplyParameterChanges(graph->Links, node, VisualScriptPinDirection::Out, outputChanges);
					}
				}
				removed += DropRepeatedLinks(graph->Links);
			}
			target.Inputs = std::move(inputs);
			target.Outputs = std::move(outputs);
			return removed;
		}

		Result<uint32_t> SetFunctionPure(VisualScriptDocument& document, uint32_t function, bool pure)
		{
			Result<VisualScriptFunction*> owner = GetFunction(document, function);
			if (!owner)
			{
				return Error{owner.GetError()};
			}
			VisualScriptFunction& target = *owner.GetValue();
			uint32_t removed = 0;
			if (pure && !target.Pure)
			{
				std::unordered_set<uint32_t> flowNodes;
				for (VisualScriptNode const& node : target.Graph.Nodes)
				{
					if (node.Type == EntryType || node.Type == ReturnType)
					{
						flowNodes.insert(node.ID);
					}
				}
				for (VisualScriptGraph* const graph : GetGraphs(document))
				{
					std::unordered_set<uint32_t> calls;
					for (VisualScriptNode const& node : graph->Nodes)
					{
						if (node.Type == CallType && NamesID(node.Settings, FunctionSetting, function))
						{
							calls.insert(node.ID);
						}
					}
					bool const ownGraph = graph == &target.Graph;
					size_t const before = graph->Links.size();
					std::erase_if(graph->Links,
					              [&calls, &flowNodes, ownGraph](VisualScriptLink const& link)
					              {
									  bool const fromFlow =
										  calls.contains(link.From.Node) || (ownGraph && flowNodes.contains(link.From.Node));
									  bool const toFlow = calls.contains(link.To.Node) || (ownGraph && flowNodes.contains(link.To.Node));
									  return (fromFlow && link.From.Pin == "Then") || (toFlow && link.To.Pin == "Execute");
								  });
					removed += static_cast<uint32_t>(before - graph->Links.size());
				}
			}
			target.Pure = pure;
			return removed;
		}

		Result<uint32_t> RemoveFunction(VisualScriptDocument& document, uint32_t function, bool removeCalls)
		{
			if (FindFunction(document, function) == nullptr)
			{
				return MakeError("the document has no function {}", function);
			}
			std::erase_if(document.Functions,
			              [function](VisualScriptFunction const& existing)
			              {
							  return existing.ID == function;
						  });
			uint32_t removed = 0;
			if (removeCalls)
			{
				for (VisualScriptGraph* const graph : GetGraphs(document))
				{
					removed += RemoveNodesIf(*graph,
					                         [function](VisualScriptNode const& node)
					                         {
												 return node.Type == CallType && NamesID(node.Settings, FunctionSetting, function);
											 });
				}
			}
			return removed;
		}

		Result<uint32_t> DuplicateFunction(VisualScriptDocument& document, uint32_t function)
		{
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [function](VisualScriptFunction const& existing)
			                                {
												return existing.ID == function;
											});
			if (found == document.Functions.end())
			{
				return MakeError("the document has no function {}", function);
			}
			if (document.Functions.size() >= Format::MaxFunctions)
			{
				return MakeError("a document holds at most {} functions", Format::MaxFunctions);
			}
			VisualScriptFunction copy = *found;
			Result<uint32_t> first = TakeIDs(document, 1 + copy.Locals.size() + copy.Graph.Nodes.size() + copy.Graph.Comments.size());
			if (!first)
			{
				return first;
			}

			uint32_t next = first.GetValue();
			copy.ID = next++;
			copy.Name = MakeUniqueVisualScriptName(found->Name,
			                                       [&document](std::string_view name)
			                                       {
													   return IsFunctionNameTaken(document, name);
												   });
			std::unordered_map<uint32_t, uint32_t> locals;
			for (VisualScriptLocal& local : copy.Locals)
			{
				locals.emplace(local.ID, next);
				local.ID = next++;
			}
			std::unordered_map<uint32_t, uint32_t> nodes;
			for (VisualScriptNode& node : copy.Graph.Nodes)
			{
				nodes.emplace(node.ID, next);
				node.ID = next++;
				if (node.Type == GetVariableType || node.Type == SetVariableType)
				{
					for (auto const& [from, to] : locals)
					{
						if (NamesID(node.Settings, VariableSetting, from))
						{
							node.Settings[VariableSetting] = to;
							break;
						}
					}
				}
				else if (node.Type == CallType && NamesID(node.Settings, FunctionSetting, function))
				{
					node.Settings[FunctionSetting] = copy.ID;
				}
			}
			// Links between nodes of the graph follow them (the loader keeps no other).
			std::erase_if(copy.Graph.Links,
			              [&nodes](VisualScriptLink const& link)
			              {
							  return !nodes.contains(link.From.Node) || !nodes.contains(link.To.Node);
						  });
			for (VisualScriptLink& link : copy.Graph.Links)
			{
				link.From.Node = nodes.find(link.From.Node)->second;
				link.To.Node = nodes.find(link.To.Node)->second;
			}
			for (VisualScriptComment& comment : copy.Graph.Comments)
			{
				comment.ID = next++;
			}
			uint32_t const id = copy.ID;
			document.Functions.insert(std::next(found), std::move(copy));
			return id;
		}

		Result<uint32_t> PromoteToVariable(VisualScriptDocument& document, uint32_t function, VisualScriptPinReference const& pin,
		                                   VisualScriptPinDirection direction, VisualScriptNodePins const& pins)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			VisualScriptNode* const node = FindNode(graph, pin.Node);
			if (node == nullptr)
			{
				return MakeError("the graph has no node {}", pin.Node);
			}
			if (Result<void> members = CheckMembers(*node); !members)
			{
				return Error{members.GetError()};
			}
			if (!node->Position)
			{
				return MakeError("node {} has no position yet", pin.Node);
			}
			std::vector<VisualScriptNodePin> const row = GetPinRow(pins, pin.Node);
			VisualScriptNodePin const* const promoted = FindPin(row, pin.Pin, direction);
			if (promoted == nullptr || AddressesUnsplitField(*node, pin.Pin, direction) || IsSplit(*node, pin.Pin, direction))
			{
				return MakeError("node {} has no {} {}", pin.Node, DescribeDirection(direction), pin.Pin);
			}
			if (IsExecPin(*promoted))
			{
				return MakeError("{} is a flow pin: variables hold values", pin.Pin);
			}
			if (promoted->Type == VisualScriptWildcardType)
			{
				return MakeError("the type of {} is not known yet", pin.Pin);
			}
			if (Result<void> checked = CheckValueType(promoted->Type, "a variable"); !checked)
			{
				return Error{checked.GetError()};
			}
			// The value an input passed while nothing was linked to it becomes the variable's.
			Json value;
			if (direction == VisualScriptPinDirection::In)
			{
				Result<Json> literal = GetPromotedLiteral(graph.Links, *node, *promoted);
				if (!literal)
				{
					return Error{literal.GetError()};
				}
				value = literal.TakeValue();
			}
			// The node after whose first exec output a Set goes, and the link it goes into.
			VisualScriptNodePin const* flow = nullptr;
			if (direction == VisualScriptPinDirection::Out)
			{
				auto const exit = std::find_if(row.begin(), row.end(),
				                               [](VisualScriptNodePin const& candidate)
				                               {
												   return candidate.Direction == VisualScriptPinDirection::Out && IsExecPin(candidate);
											   });
				flow = exit != row.end() ? &*exit : nullptr;
			}
			if (document.Variables.size() >= Format::MaxVariables)
			{
				return MakeError("a document holds at most {} variables", Format::MaxVariables);
			}
			if (graph.Nodes.size() >= Format::MaxNodesPerGraph)
			{
				return MakeError("a graph holds at most {} nodes", Format::MaxNodesPerGraph);
			}
			if (Result<void> count = CheckLinkCount(graph.Links.size() + 2); !count)
			{
				return Error{count.GetError()};
			}
			Result<uint32_t> first = TakeIDs(document, 2);
			if (!first)
			{
				return first;
			}

			VisualScriptVariable variable;
			variable.ID = first.GetValue();
			variable.Name = MakeUniqueVisualScriptName(pin.Pin,
			                                           [&document](std::string_view name)
			                                           {
														   return IsVariableNameTaken(document, name);
													   });
			variable.Type = promoted->Type;
			VisualScriptNode user;
			user.ID = variable.ID + 1;
			user.Settings[VariableSetting] = variable.ID;
			glm::vec2 const anchor = *node->Position + promoted->Anchor;
			if (direction == VisualScriptPinDirection::In)
			{
				user.Type = std::string(GetVariableType);
				user.Position = RoundPosition(anchor + glm::vec2(PromotedGetOffset, -PromotedNodeRise));
				variable.Value = std::move(value);
				node->Inputs.erase(pin.Pin);
				std::erase_if(graph.Links,
				              [&pin](VisualScriptLink const& link)
				              {
								  return link.To == pin;
							  });
				graph.Links.push_back({{user.ID, "Value"}, pin});
			}
			else
			{
				user.Type = std::string(SetVariableType);
				user.Position = RoundPosition(anchor + glm::vec2(PromotedSetOffset, -PromotedNodeRise));
				graph.Links.push_back({pin, {user.ID, "Value"}});
				if (flow != nullptr)
				{
					VisualScriptPinReference const exit{pin.Node, flow->Name};
					auto const next = std::find_if(graph.Links.begin(), graph.Links.end(),
					                               [&exit](VisualScriptLink const& link)
					                               {
													   return link.From == exit;
												   });
					if (next != graph.Links.end())
					{
						VisualScriptPinReference const target = next->To;
						next->To = {user.ID, "Execute"};
						graph.Links.push_back({{user.ID, "Then"}, target});
					}
					else
					{
						graph.Links.push_back({exit, {user.ID, "Execute"}});
					}
				}
			}
			uint32_t const id = variable.ID;
			document.Variables.push_back(std::move(variable));
			graph.Nodes.push_back(std::move(user));
			return id;
		}

		Result<void> PromoteToFunctionInput(VisualScriptDocument& document, uint32_t function, VisualScriptPinReference const& pin,
		                                    VisualScriptNodePins const& pins)
		{
			if (function == 0)
			{
				return Error{"only an input of a node in a function's graph becomes an input of the function"};
			}
			Result<VisualScriptFunction*> owner = GetFunction(document, function);
			if (!owner)
			{
				return Error{owner.GetError()};
			}
			VisualScriptFunction& target = *owner.GetValue();
			VisualScriptNode* const node = FindNode(target.Graph, pin.Node);
			if (node == nullptr)
			{
				return MakeError("the graph has no node {}", pin.Node);
			}
			if (Result<void> members = CheckMembers(*node); !members)
			{
				return members;
			}
			std::vector<VisualScriptNodePin> const row = GetPinRow(pins, pin.Node);
			VisualScriptNodePin const* const promoted = FindPin(row, pin.Pin, VisualScriptPinDirection::In);
			if (promoted == nullptr || AddressesUnsplitField(*node, pin.Pin, VisualScriptPinDirection::In) ||
			    IsSplit(*node, pin.Pin, VisualScriptPinDirection::In))
			{
				return MakeError("node {} has no input {}", pin.Node, pin.Pin);
			}
			if (IsExecPin(*promoted))
			{
				return MakeError("{} is a flow pin: function inputs are values", pin.Pin);
			}
			if (promoted->Type == VisualScriptWildcardType)
			{
				return MakeError("the type of {} is not known yet", pin.Pin);
			}
			if (Result<void> checked = CheckValueType(promoted->Type, "a parameter"); !checked)
			{
				return checked;
			}
			if (IsLinkedInput(target.Graph.Links, pin.Node, pin.Pin))
			{
				return MakeError("{} is linked: only an unlinked input becomes a function input", pin.Pin);
			}
			VisualScriptNode const* const entry = FindEntry(target.Graph);
			if (entry == nullptr)
			{
				return MakeError("{} has no Function.Entry to take the input from", target.Name);
			}
			if (target.Inputs.size() >= Format::MaxParameters)
			{
				return MakeError("a function's inputs hold at most {} parameters", Format::MaxParameters);
			}
			if (Result<void> count = CheckLinkCount(target.Graph.Links.size() + 1); !count)
			{
				return count;
			}
			std::string const name = MakeUniqueVisualScriptName(pin.Pin,
			                                                    [&target](std::string_view candidate)
			                                                    {
																	return IsParameterNameTaken(target.Inputs, candidate);
																});
			// The calls take over what the input passed (its literal, else its default): each must have room for it.
			Result<Json> literal = GetPromotedLiteral(target.Graph.Links, *node, *promoted);
			if (!literal)
			{
				return Error{literal.GetError()};
			}
			std::optional<Json> const value = literal.GetValue().is_null() ? std::nullopt : std::optional<Json>(literal.TakeValue());
			std::vector<VisualScriptNode*> calls;
			for (VisualScriptGraph* const graph : GetGraphs(document))
			{
				for (VisualScriptNode& call : graph->Nodes)
				{
					if (call.Type == CallType && NamesID(call.Settings, FunctionSetting, function))
					{
						if (Result<void> members = CheckMembers(call); !members)
						{
							return members;
						}
						if (value && !call.Inputs.contains(name) && call.Inputs.size() >= Format::MaxSettingsMembers)
						{
							return MakeError("node {} holds the most literals a node can", call.ID);
						}
						calls.push_back(&call);
					}
				}
			}

			node->Inputs.erase(pin.Pin);
			if (value)
			{
				for (VisualScriptNode* const call : calls)
				{
					call->Inputs[name] = *value;
				}
			}
			target.Graph.Links.push_back({{entry->ID, name}, pin});
			target.Inputs.push_back({name, promoted->Type});
			return {};
		}

		Result<void> AlignElements(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> elements,
		                           VisualScriptAlignment alignment, VisualScriptNodeExtents const& extents)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			Result<std::vector<ElementRect>> rects = GetElementRects(*found.GetValue(), elements, extents);
			if (!rects)
			{
				return Error{rects.GetError()};
			}
			if (rects.GetValue().size() < 2)
			{
				return Error{"aligning takes two elements or more"};
			}
			glm::vec2 minimum(std::numeric_limits<float>::max());
			glm::vec2 maximum(std::numeric_limits<float>::lowest());
			for (ElementRect const& rect : rects.GetValue())
			{
				minimum = glm::min(minimum, rect.Position);
				maximum = glm::max(maximum, rect.Position + rect.Size);
			}
			glm::vec2 const center = (minimum + maximum) * 0.5f;

			std::vector<glm::vec2> positions;
			positions.reserve(rects.GetValue().size());
			for (ElementRect const& rect : rects.GetValue())
			{
				std::optional<glm::vec2> position;
				switch (alignment)
				{
					case VisualScriptAlignment::Left:
						position = glm::vec2(minimum.x, rect.Position.y);
						break;
					case VisualScriptAlignment::Right:
						position = glm::vec2(maximum.x - rect.Size.x, rect.Position.y);
						break;
					case VisualScriptAlignment::Top:
						position = glm::vec2(rect.Position.x, minimum.y);
						break;
					case VisualScriptAlignment::Bottom:
						position = glm::vec2(rect.Position.x, maximum.y - rect.Size.y);
						break;
					case VisualScriptAlignment::Centers:
						position = glm::vec2(center.x - rect.Size.x * 0.5f, rect.Position.y);
						break;
					case VisualScriptAlignment::Middles:
						position = glm::vec2(rect.Position.x, center.y - rect.Size.y * 0.5f);
						break;
				}
				if (!position)
				{
					return MakeError("{} is no alignment", static_cast<int>(alignment));
				}
				positions.push_back(RoundPosition(*position));
			}
			for (size_t index = 0; index < positions.size(); index++)
			{
				SetRectPosition(rects.GetValue()[index], positions[index]);
			}
			return {};
		}

		Result<void> DistributeElements(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> elements,
		                                VisualScriptDistribution distribution, VisualScriptNodeExtents const& extents)
		{
			if (distribution != VisualScriptDistribution::Horizontal && distribution != VisualScriptDistribution::Vertical)
			{
				return MakeError("{} is no distribution", static_cast<int>(distribution));
			}
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			Result<std::vector<ElementRect>> rects = GetElementRects(*found.GetValue(), elements, extents);
			if (!rects)
			{
				return Error{rects.GetError()};
			}
			std::vector<ElementRect>& sorted = rects.GetValue();
			if (sorted.size() < 3)
			{
				return Error{"distributing takes three elements or more"};
			}
			glm::length_t const axis = distribution == VisualScriptDistribution::Horizontal ? 0 : 1;
			std::sort(sorted.begin(), sorted.end(),
			          [axis](ElementRect const& a, ElementRect const& b)
			          {
						  return a.Position[axis] != b.Position[axis] ? a.Position[axis] < b.Position[axis] : a.ID < b.ID;
					  });
			// The gaps between neighbours are equal: what is left between the first's far edge and the last's near edge, once
			// the elements between them take their sizes, shared out.
			double middle = 0.0;
			for (size_t index = 1; index + 1 < sorted.size(); index++)
			{
				middle += sorted[index].Size[axis];
			}
			double const start = static_cast<double>(sorted.front().Position[axis]) + sorted.front().Size[axis];
			double const gap =
				(static_cast<double>(sorted.back().Position[axis]) - start - middle) / static_cast<double>(sorted.size() - 1);
			double edge = start;
			for (size_t index = 1; index + 1 < sorted.size(); index++)
			{
				edge += gap;
				glm::vec2 position = sorted[index].Position;
				position[axis] = static_cast<float>(edge);
				SetRectPosition(sorted[index], RoundPosition(position));
				edge += sorted[index].Size[axis];
			}
			return {};
		}

		Result<uint32_t> StraightenExec(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> nodes,
		                                VisualScriptNodePins const& pins)
		{
			Result<VisualScriptGraph*> found = GetGraph(document, function);
			if (!found)
			{
				return Error{found.GetError()};
			}
			VisualScriptGraph& graph = *found.GetValue();
			// The nodes in the graph's order, with their pins.
			ElementLookup const lookup(graph, nodes);
			std::unordered_map<uint32_t, std::vector<VisualScriptNodePin>> rows;
			for (uint32_t const id : nodes)
			{
				VisualScriptNode const* const node = lookup.FindNode(id);
				if (node == nullptr)
				{
					return MakeError("the graph has no node {}", id);
				}
				if (!node->Position)
				{
					return MakeError("node {} has no position yet", id);
				}
				if (!rows.contains(id))
				{
					rows.emplace(id, GetPinRow(pins, id));
				}
			}
			if (rows.size() < 2)
			{
				return Error{"straightening takes two nodes or more"};
			}
			std::vector<VisualScriptNode*> order;
			for (VisualScriptNode& node : graph.Nodes)
			{
				if (rows.contains(node.ID))
				{
					order.push_back(&node);
				}
			}

			// Each node's first exec link from another of them gives the node it follows, and how far below that node's top its
			// own top goes for the input to be level with the output.
			struct Follower
			{
				VisualScriptNode* Node = nullptr;
				float Offset = 0.0f;
			};
			std::unordered_map<uint32_t, VisualScriptNode*> byID;
			for (VisualScriptNode* const node : order)
			{
				byID.emplace(node->ID, node);
			}
			std::unordered_set<uint32_t> followers;
			std::unordered_map<uint32_t, std::vector<Follower>> children;
			for (VisualScriptLink const& link : graph.Links)
			{
				auto const fromRow = rows.find(link.From.Node);
				auto const toRow = rows.find(link.To.Node);
				if (link.From.Node == link.To.Node || fromRow == rows.end() || toRow == rows.end() || followers.contains(link.To.Node))
				{
					continue;
				}
				VisualScriptNodePin const* const output = FindPin(fromRow->second, link.From.Pin, VisualScriptPinDirection::Out);
				VisualScriptNodePin const* const input = FindPin(toRow->second, link.To.Pin, VisualScriptPinDirection::In);
				if (output == nullptr || input == nullptr || !IsExecPin(*output) || !IsExecPin(*input))
				{
					continue;
				}
				followers.insert(link.To.Node);
				children[link.From.Node].push_back({byID.find(link.To.Node)->second, output->Anchor.y - input->Anchor.y});
			}

			// From the nodes no other one leads to (then, for nodes only cycles lead to, the first of each cycle in the graph's
			// order), each node follows the one before it once that one has moved.
			std::unordered_set<uint32_t> placed;
			uint32_t moved = 0;
			auto const follow = [&children, &placed, &moved](VisualScriptNode* root)
			{
				std::vector<VisualScriptNode*> pending{root};
				placed.insert(root->ID);
				while (!pending.empty())
				{
					VisualScriptNode* const current = pending.back();
					pending.pop_back();
					auto const next = children.find(current->ID);
					if (next == children.end())
					{
						continue;
					}
					for (auto child = next->second.rbegin(); child != next->second.rend(); ++child)
					{
						if (!placed.insert(child->Node->ID).second)
						{
							continue;
						}
						glm::vec2 const position = RoundPosition(glm::vec2(child->Node->Position->x, current->Position->y + child->Offset));
						if (position != *child->Node->Position)
						{
							child->Node->Position = position;
							moved++;
						}
						pending.push_back(child->Node);
					}
				}
			};
			for (VisualScriptNode* const node : order)
			{
				if (!followers.contains(node->ID))
				{
					follow(node);
				}
			}
			for (VisualScriptNode* const node : order)
			{
				if (!placed.contains(node->ID))
				{
					follow(node);
				}
			}
			return moved;
		}
	}
}
