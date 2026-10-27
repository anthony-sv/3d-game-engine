#include "stpch.h"
#include "Strada/Asset/VisualScriptSerializer.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace Strada
{
	namespace
	{
		using Format = VisualScriptSerializer;

		// The keys each element may have, in the order the writer writes them.
		constexpr std::array<std::string_view, 2> FileKeys = {"Strada", "VisualScript"};
		constexpr std::array<std::string_view, 2> HeaderKeys = {"Version", "Type"};
		// Version 3 added Kind, written first; a file of version 1 or 2 knows only the others.
		constexpr std::array<std::string_view, 5> ContentKeys = {"Kind", "Description", "Variables", "Functions", "Graph"};
		constexpr std::array<std::string_view, 8> VariableKeys = {"ID",     "Name",    "Type",  "Value",
		                                                          "Public", "Tooltip", "Range", "FormerNames"};
		constexpr std::array<std::string_view, 7> FunctionKeys = {"ID", "Name", "Pure", "Inputs", "Outputs", "Locals", "Graph"};
		// Version 3 appended FormerNames; a file of version 1 or 2 knows only the others.
		constexpr std::array<std::string_view, 3> ParameterKeys = {"Name", "Type", "FormerNames"};
		constexpr size_t Version2ParameterKeyCount = 2;
		constexpr std::array<std::string_view, 4> LocalKeys = {"ID", "Name", "Type", "Value"};
		constexpr std::array<std::string_view, 3> GraphKeys = {"Nodes", "Links", "Comments"};
		// Version 2 appended the last four; a file of version 1 knows only the others.
		constexpr std::array<std::string_view, 9> NodeKeys = {"ID",    "Type",    "Position",  "Settings", "Inputs",
		                                                      "Split", "Comment", "Collapsed", "Enabled"};
		constexpr size_t Version1NodeKeyCount = 5;
		constexpr std::array<std::string_view, 2> SplitKeys = {"Inputs", "Outputs"};
		constexpr std::array<std::string_view, 2> LinkKeys = {"From", "To"};
		constexpr std::array<std::string_view, 5> CommentKeys = {"ID", "Text", "Position", "Size", "Color"};

		std::string Member(std::string_view path, std::string_view key)
		{
			return path.empty() ? std::string(key) : fmt::format("{}.{}", path, key);
		}

		std::string Element(std::string_view path, size_t index)
		{
			return fmt::format("{}[{}]", path, index);
		}

		// A string as a quoted JSON string, for messages about text that may hold anything; long text is cut short (a cut
		// character is replaced).
		std::string Quote(std::string_view text)
		{
			constexpr size_t MaxQuotedBytes = 64;
			std::string quoted = Json(std::string(text.substr(0, MaxQuotedBytes))).dump(-1, ' ', false, Json::error_handler_t::replace);
			return text.size() > MaxQuotedBytes ? quoted + "..." : quoted;
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

		// The strings of values and their member names, and the texts of documents (descriptions, tooltips, the texts of
		// comments and nodes): at most MaxTextBytes of UTF-8 (lines allowed).
		bool IsValidLongText(std::string_view text)
		{
			return text.size() <= Format::MaxTextBytes && Utf8::IsValid(text);
		}

		// VisualScriptSerializer::IsValidValue of a value at a depth (the value itself is level 1): the depth limit also bounds
		// the recursion.
		bool IsValidValueAt(Json const& value, size_t depth)
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
					if (!IsValidLongText(item.key()) || !IsValidValueAt(item.value(), depth + 1))
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
									   return IsValidValueAt(element, depth + 1);
								   });
			}
			return value.is_null() || value.is_boolean() || value.is_number();
		}

		// Why a value fails IsValidValue, followed by what happens to it.
		std::string DescribeInvalidValue(std::string_view consequence)
		{
			return fmt::format("nested deeper than {} levels, or holds a number that is not finite or a string of more than {} bytes or "
			                   "not of UTF-8; {}",
			                   Format::MaxValueDepth, Format::MaxTextBytes, consequence);
		}

		// IDs are integers from 1 to MaxID, however the JSON stores them (documents built in memory hold signed integers).
		std::optional<uint32_t> ToID(Json const& value)
		{
			if (!value.is_number_integer())
			{
				return std::nullopt;
			}
			if (value.is_number_unsigned())
			{
				uint64_t const id = value.get<uint64_t>();
				return id >= 1 && id <= Format::MaxID ? std::optional<uint32_t>(static_cast<uint32_t>(id)) : std::nullopt;
			}
			int64_t const id = value.get<int64_t>();
			return id >= 1 && id <= Format::MaxID ? std::optional<uint32_t>(static_cast<uint32_t>(id)) : std::nullopt;
		}

		// [nodeID, "Pin"].
		std::optional<VisualScriptPinReference> ToPinReference(Json const& value)
		{
			if (!value.is_array() || value.size() != 2 || !value[1].is_string())
			{
				return std::nullopt;
			}
			std::optional<uint32_t> const node = ToID(value[0]);
			std::string const& pin = value[1].get_ref<std::string const&>();
			if (!node || !Format::IsValidText(pin, Format::MaxPinNameBytes))
			{
				return std::nullopt;
			}
			return VisualScriptPinReference{*node, pin};
		}

		// An array of N finite numbers. Read as doubles, so values beyond the range of float are clamped rather than refused.
		template<size_t N>
		std::optional<std::array<double, N>> ToNumbers(Json const& value)
		{
			if (!value.is_array() || value.size() != N)
			{
				return std::nullopt;
			}
			std::array<double, N> numbers{};
			for (size_t i = 0; i < N; i++)
			{
				if (!value[i].is_number())
				{
					return std::nullopt;
				}
				numbers[i] = value[i].get<double>();
				if (!std::isfinite(numbers[i]))
				{
					return std::nullopt;
				}
			}
			return numbers;
		}

		// Reads the content object of a version of the format (the keys a version does not have are unknown keys). Refuses it
		// only for an unknown key under UnknownFieldPolicy::Error; everything else that is damaged is repaired, and every
		// repair is recorded as an issue.
		class DocumentReader
		{
		public:
			DocumentReader(DeserializationContext const& context, int version)
				: m_Context(context),
				  m_Version(version)
			{
			}

			// Unknown keys of an object: each refuses the document under UnknownFieldPolicy::Error, is an issue under Warn and
			// is skipped under Ignore.
			[[nodiscard]] Result<void> CheckKeys(Json const& object, std::span<std::string_view const> allowed, std::string_view path,
			                                     std::string_view message = "unknown key; it is ignored")
			{
				for (auto const& item : object.items())
				{
					std::string const& key = item.key();
					if (std::find(allowed.begin(), allowed.end(), key) != allowed.end())
					{
						continue;
					}
					switch (m_Context.UnknownFields)
					{
						case UnknownFieldPolicy::Error:
							return MakeError("{}: unknown key", Member(path, key));
						case UnknownFieldPolicy::Warn:
							Report(Member(path, key), std::string(message));
							break;
						case UnknownFieldPolicy::Ignore:
							break;
					}
				}
				return {};
			}

			[[nodiscard]] Result<VisualScriptDocument> ReadContent(Json const& content)
			{
				if (!content.is_object())
				{
					return Error{"the visual script must be a JSON object"};
				}
				// A key of version 3 in a file of version 1 or 2 is an unknown key, read as such: no earlier writer wrote one.
				std::span<std::string_view const> const contentKeys = HasVersion3Keys()
				                                                          ? std::span<std::string_view const>(ContentKeys)
				                                                          : std::span<std::string_view const>(ContentKeys).subspan(1);
				if (Result<void> keys = CheckKeys(content, contentKeys, {}); !keys)
				{
					return Error{keys.GetError()};
				}

				// IDs are claimed in document order (variables; each function with its locals, nodes and comments; the event
				// graph's nodes and comments): of two elements with one ID, the later one is dropped.
				VisualScriptDocument document;
				if (HasVersion3Keys())
				{
					document.Kind = ReadKind(content);
				}
				document.Description = ReadText(content, "Description", {});
				if (Result<void> variables = ReadVariables(content, document); !variables)
				{
					return Error{variables.GetError()};
				}
				if (Result<void> functions = ReadFunctions(content, document); !functions)
				{
					return Error{functions.GetError()};
				}
				if (Result<void> graph = ReadGraph(content, {}, document.Graph); !graph)
				{
					return Error{graph.GetError()};
				}
				return document;
			}

			// Hands the issues to the caller and to the context's warnings.
			void Publish(std::vector<VisualScriptIssue>* issues)
			{
				for (VisualScriptIssue const& issue : m_Issues)
				{
					m_Context.Warn(fmt::format("{}: {}", issue.Path, issue.Message));
				}
				if (issues != nullptr)
				{
					issues->insert(issues->end(), std::make_move_iterator(m_Issues.begin()), std::make_move_iterator(m_Issues.end()));
				}
				m_Issues.clear();
			}

		private:
			void Report(std::string path, std::string message) { m_Issues.push_back({std::move(path), std::move(message)}); }

			// The document's Kind and parameters' FormerNames: files of version 3 and the content form.
			bool HasVersion3Keys() const { return m_Version >= 3; }

			uint32_t Claim(uint32_t id)
			{
				m_IDs.insert(id);
				return id;
			}

			// The array under key; nullptr when the key is missing or holds something else (reported, read as []).
			Json const* FindList(Json const& object, char const* key, std::string_view path)
			{
				auto const found = object.find(key);
				if (found == object.end())
				{
					return nullptr;
				}
				if (!found->is_array())
				{
					Report(Member(path, key), "expected an array; replaced by []");
					return nullptr;
				}
				return &*found;
			}

			// The ID of an element when it is valid and not taken; otherwise reports why the element is dropped. The ID is
			// claimed only once the whole element is known to be kept, so dropped elements claim nothing.
			std::optional<uint32_t> ReadNewID(Json const& object, std::string_view path, std::string_view kind)
			{
				auto const found = object.find("ID");
				if (found == object.end())
				{
					Report(Member(path, "ID"), fmt::format("missing; the {} is dropped", kind));
					return std::nullopt;
				}
				std::optional<uint32_t> const id = ToID(*found);
				if (!id)
				{
					Report(Member(path, "ID"), fmt::format("expected an integer from 1 to {}; the {} is dropped", Format::MaxID, kind));
					return std::nullopt;
				}
				if (m_IDs.contains(*id))
				{
					Report(Member(path, "ID"), fmt::format("{} is the ID of an earlier element; the {} is dropped", *id, kind));
					return std::nullopt;
				}
				return id;
			}

			std::optional<std::string> ReadName(Json const& object, std::string_view path, std::string_view kind)
			{
				auto const found = object.find("Name");
				if (found == object.end())
				{
					Report(Member(path, "Name"), fmt::format("missing; the {} is dropped", kind));
					return std::nullopt;
				}
				if (!found->is_string() || !Format::IsValidName(found->get_ref<std::string const&>()))
				{
					Report(Member(path, "Name"),
					       fmt::format(
							   "expected a name of 1 to {} bytes without control characters, '.' or a leading or trailing space; the {} "
							   "is dropped",
							   Format::MaxNameBytes, kind));
					return std::nullopt;
				}
				return found->get<std::string>();
			}

			std::optional<std::string> ReadType(Json const& object, std::string_view path, size_t maxBytes, std::string_view kind)
			{
				auto const found = object.find("Type");
				if (found == object.end())
				{
					Report(Member(path, "Type"), fmt::format("missing; the {} is dropped", kind));
					return std::nullopt;
				}
				if (!found->is_string() || !Format::IsValidText(found->get_ref<std::string const&>(), maxBytes))
				{
					Report(Member(path, "Type"),
					       fmt::format("expected a type of 1 to {} bytes without control characters; the {} is dropped", maxBytes, kind));
					return std::nullopt;
				}
				return found->get<std::string>();
			}

			std::string ReadText(Json const& object, char const* key, std::string_view path)
			{
				auto const found = object.find(key);
				if (found == object.end())
				{
					return {};
				}
				// Parsed JSON always holds UTF-8; documents built in memory may not, and the writer would replace what is not.
				if (found->is_string() && IsValidLongText(found->get_ref<std::string const&>()))
				{
					return found->get<std::string>();
				}
				Report(Member(path, key),
				       fmt::format("expected a string of UTF-8 of at most {} bytes; replaced by \"\"", Format::MaxTextBytes));
				return {};
			}

			VisualScriptDocumentKind ReadKind(Json const& content)
			{
				auto const found = content.find("Kind");
				if (found == content.end())
				{
					return VisualScriptDocumentKind::Script;
				}
				if (found->is_string())
				{
					if (std::optional<VisualScriptDocumentKind> const kind =
					        VisualScriptDocumentKindFromString(found->get_ref<std::string const&>()))
					{
						return *kind;
					}
				}
				Report("Kind", "expected \"Script\" or \"Library\"; the document is a script");
				return VisualScriptDocumentKind::Script;
			}

			bool ReadBool(Json const& object, char const* key, std::string_view path)
			{
				auto const found = object.find(key);
				if (found == object.end())
				{
					return false;
				}
				if (found->is_boolean())
				{
					return found->get<bool>();
				}
				Report(Member(path, key), "expected true or false; replaced by false");
				return false;
			}

			// A variable's or local's value as stored; null (none) when missing, null or invalid.
			Json ReadValue(Json const& object, std::string_view path)
			{
				auto const found = object.find("Value");
				if (found == object.end() || found->is_null())
				{
					return Json();
				}
				if (Format::IsValidValue(*found))
				{
					return *found;
				}
				Report(Member(path, "Value"), DescribeInvalidValue("removed"));
				return Json();
			}

			std::optional<glm::vec2> ReadRange(Json const& variable, std::string_view path)
			{
				auto const found = variable.find("Range");
				if (found == variable.end())
				{
					return std::nullopt;
				}
				// Compared as floats, as stored, so a range that loads stays valid when saved.
				if (found->is_array() && found->size() == 2)
				{
					std::optional<float> const minimum = Format::ReadFloat((*found)[0]);
					std::optional<float> const maximum = Format::ReadFloat((*found)[1]);
					if (minimum && maximum && *minimum <= *maximum)
					{
						return glm::vec2(*minimum, *maximum);
					}
				}
				Report(Member(path, "Range"),
				       fmt::format("expected two finite numbers of at most {} in magnitude, the minimum first; removed",
				                   Format::MaxFloatMagnitude));
				return std::nullopt;
			}

			// Moves coordinates outside [minimum, maximum] to the nearest bound, reporting when any moved.
			glm::vec2 Clamp(std::array<double, 2> const& values, double minimum, double maximum, std::string_view path)
			{
				glm::vec2 result{};
				bool clamped = false;
				for (glm::length_t i = 0; i < 2; i++)
				{
					double const value = std::clamp(values[static_cast<size_t>(i)], minimum, maximum);
					clamped = clamped || value != values[static_cast<size_t>(i)];
					result[i] = static_cast<float>(value);
				}
				if (clamped)
				{
					Report(std::string(path), fmt::format("outside {} to {}; clamped", minimum, maximum));
				}
				return result;
			}

			std::optional<glm::vec2> ReadNodePosition(Json const& node, std::string_view nodePath)
			{
				auto const found = node.find("Position");
				if (found == node.end())
				{
					return std::nullopt;
				}
				std::string const path = Member(nodePath, "Position");
				std::optional<std::array<double, 2>> const values = ToNumbers<2>(*found);
				if (!values)
				{
					Report(path, "expected two numbers; the node is unplaced");
					return std::nullopt;
				}
				return Clamp(*values, -Format::MaxCoordinate, Format::MaxCoordinate, path);
			}

			glm::vec2 ReadCommentVector(Json const& comment, char const* key, std::string_view commentPath, glm::vec2 fallback,
			                            double minimum, double maximum)
			{
				auto const found = comment.find(key);
				if (found == comment.end())
				{
					return fallback;
				}
				std::string const path = Member(commentPath, key);
				std::optional<std::array<double, 2>> const values = ToNumbers<2>(*found);
				if (!values)
				{
					Report(path, fmt::format("expected two numbers; replaced by [{}, {}]", fallback.x, fallback.y));
					return fallback;
				}
				return Clamp(*values, minimum, maximum, path);
			}

			glm::vec4 ReadCommentColor(Json const& comment, std::string_view commentPath)
			{
				glm::vec4 const fallback = VisualScriptComment().Color;
				auto const found = comment.find("Color");
				if (found == comment.end())
				{
					return fallback;
				}
				std::optional<std::array<double, 4>> const values = ToNumbers<4>(*found);
				if (values && std::all_of(values->begin(), values->end(),
				                          [](double value)
				                          {
											  return value >= 0.0 && value <= 1.0;
										  }))
				{
					return {static_cast<float>((*values)[0]), static_cast<float>((*values)[1]), static_cast<float>((*values)[2]),
					        static_cast<float>((*values)[3])};
				}
				Report(Member(commentPath, "Color"), fmt::format("expected four numbers from 0 to 1; replaced by [{}, {}, {}, {}]",
				                                                 fallback.r, fallback.g, fallback.b, fallback.a));
				return fallback;
			}

			// A node's Settings or Inputs: an object of at most MaxSettingsMembers members with valid names and values.
			Json ReadMembers(Json const& node, char const* key, std::string_view nodePath)
			{
				auto const found = node.find(key);
				if (found == node.end())
				{
					return Json::object();
				}
				std::string const path = Member(nodePath, key);
				if (!found->is_object())
				{
					Report(path, "expected an object; replaced by {}");
					return Json::object();
				}

				bool intact = found->size() <= Format::MaxSettingsMembers;
				for (auto const& item : found->items())
				{
					if (!intact)
					{
						break;
					}
					intact = Format::IsValidText(item.key(), Format::MaxPinNameBytes) && Format::IsValidValue(item.value());
				}
				if (intact)
				{
					return *found;
				}

				Json members = Json::object();
				for (auto const& item : found->items())
				{
					std::string const& name = item.key();
					if (!Format::IsValidText(name, Format::MaxPinNameBytes))
					{
						Report(path, fmt::format("the member {} is dropped: member names are 1 to {} bytes without control characters",
						                         Quote(name), Format::MaxPinNameBytes));
						continue;
					}
					if (members.size() == Format::MaxSettingsMembers)
					{
						Report(Member(path, name), fmt::format("more than {} members; dropped", Format::MaxSettingsMembers));
						continue;
					}
					if (!Format::IsValidValue(item.value()))
					{
						Report(Member(path, name), DescribeInvalidValue("dropped"));
						continue;
					}
					members[name] = item.value();
				}
				return members;
			}

			[[nodiscard]] Result<void> ReadVariables(Json const& content, VisualScriptDocument& document)
			{
				Json const* const list = FindList(content, "Variables", {});
				if (list == nullptr)
				{
					return {};
				}

				// Former names must differ from the names of all variables, later ones included, so they are read once every
				// variable is: the raw list and the variable's path of each kept variable.
				std::vector<std::pair<Json const*, std::string>> pendingFormerNames;
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& value = (*list)[i];
					std::string const path = Element("Variables", i);
					if (!value.is_object())
					{
						Report(path, "expected an object; the variable is dropped");
						continue;
					}
					if (Result<void> keys = CheckKeys(value, VariableKeys, path); !keys)
					{
						return keys;
					}
					if (document.Variables.size() == Format::MaxVariables)
					{
						Report(path, fmt::format("more than {} variables; the variable is dropped", Format::MaxVariables));
						continue;
					}
					std::optional<uint32_t> const id = ReadNewID(value, path, "variable");
					if (!id)
					{
						continue;
					}
					std::optional<std::string> name = ReadName(value, path, "variable");
					if (!name)
					{
						continue;
					}
					if (m_VariableNames.contains(*name))
					{
						Report(Member(path, "Name"),
						       fmt::format("'{}' is the name of an earlier variable; the variable is dropped", *name));
						continue;
					}
					std::optional<std::string> type = ReadType(value, path, Format::MaxTypeBytes, "variable");
					if (!type)
					{
						continue;
					}

					VisualScriptVariable variable;
					variable.ID = Claim(*id);
					m_VariableNames.insert(*name);
					variable.Name = std::move(*name);
					variable.Type = std::move(*type);
					variable.Value = ReadValue(value, path);
					variable.Public = ReadBool(value, "Public", path);
					variable.Tooltip = ReadText(value, "Tooltip", path);
					variable.Range = ReadRange(value, path);
					auto const formerNames = value.find("FormerNames");
					pendingFormerNames.emplace_back(formerNames == value.end() ? nullptr : &*formerNames, path);
					document.Variables.push_back(std::move(variable));
				}

				std::unordered_set<std::string> takenFormerNames;
				for (size_t i = 0; i < document.Variables.size(); i++)
				{
					auto const& [names, variablePath] = pendingFormerNames[i];
					if (names != nullptr)
					{
						ReadFormerNames(*names, Member(variablePath, "FormerNames"), m_VariableNames, "a variable", takenFormerNames,
						                document.Variables[i].FormerNames);
					}
				}
				return {};
			}

			// The former names of a variable or a parameter: valid names, none of them a current name in its scope (names, which
			// owner describes in messages) or a former name taken earlier in it, at most MaxFormerNames.
			void ReadFormerNames(Json const& list, std::string const& path, std::unordered_set<std::string> const& names,
			                     std::string_view owner, std::unordered_set<std::string>& taken, std::vector<std::string>& formerNames)
			{
				if (!list.is_array())
				{
					Report(path, "expected an array; replaced by []");
					return;
				}
				for (size_t i = 0; i < list.size(); i++)
				{
					Json const& entry = list[i];
					std::string const entryPath = Element(path, i);
					if (!entry.is_string())
					{
						Report(entryPath, "expected a string; dropped");
						continue;
					}
					if (formerNames.size() == Format::MaxFormerNames)
					{
						Report(entryPath, fmt::format("more than {} former names; dropped", Format::MaxFormerNames));
						continue;
					}
					std::string const& name = entry.get_ref<std::string const&>();
					if (!Format::IsValidName(name))
					{
						Report(entryPath, fmt::format("expected a name of 1 to {} bytes without control characters, '.' or a leading or "
						                              "trailing space; dropped",
						                              Format::MaxNameBytes));
						continue;
					}
					if (names.contains(name))
					{
						Report(entryPath, fmt::format("'{}' is the name of {}; dropped", name, owner));
						continue;
					}
					if (!taken.insert(name).second)
					{
						Report(entryPath, fmt::format("'{}' is already a former name; dropped", name));
						continue;
					}
					formerNames.push_back(name);
				}
			}

			[[nodiscard]] Result<void> ReadFunctions(Json const& content, VisualScriptDocument& document)
			{
				Json const* const list = FindList(content, "Functions", {});
				if (list == nullptr)
				{
					return {};
				}

				std::unordered_set<std::string> names;
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& value = (*list)[i];
					std::string const path = Element("Functions", i);
					if (!value.is_object())
					{
						Report(path, "expected an object; the function is dropped");
						continue;
					}
					if (Result<void> keys = CheckKeys(value, FunctionKeys, path); !keys)
					{
						return keys;
					}
					if (document.Functions.size() == Format::MaxFunctions)
					{
						Report(path, fmt::format("more than {} functions; the function is dropped", Format::MaxFunctions));
						continue;
					}
					std::optional<uint32_t> const id = ReadNewID(value, path, "function");
					if (!id)
					{
						continue;
					}
					std::optional<std::string> name = ReadName(value, path, "function");
					if (!name)
					{
						continue;
					}
					if (!names.insert(*name).second)
					{
						Report(Member(path, "Name"),
						       fmt::format("'{}' is the name of an earlier function; the function is dropped", *name));
						continue;
					}

					// A dropped function drops its contents, so they are read (and claim their IDs) only once it is kept.
					VisualScriptFunction function;
					function.ID = Claim(*id);
					function.Name = std::move(*name);
					function.Pure = ReadBool(value, "Pure", path);
					if (Result<void> inputs = ReadParameters(value, "Inputs", path, function.Inputs); !inputs)
					{
						return inputs;
					}
					if (Result<void> outputs = ReadParameters(value, "Outputs", path, function.Outputs); !outputs)
					{
						return outputs;
					}
					if (Result<void> locals = ReadLocals(value, path, function.Locals); !locals)
					{
						return locals;
					}
					if (Result<void> graph = ReadGraph(value, path, function.Graph); !graph)
					{
						return graph;
					}
					document.Functions.push_back(std::move(function));
				}
				return {};
			}

			[[nodiscard]] Result<void> ReadParameters(Json const& function, char const* key, std::string_view functionPath,
			                                          std::vector<VisualScriptParameter>& parameters)
			{
				Json const* const list = FindList(function, key, functionPath);
				if (list == nullptr)
				{
					return {};
				}

				std::span<std::string_view const> const parameterKeys =
					HasVersion3Keys() ? std::span<std::string_view const>(ParameterKeys)
									  : std::span<std::string_view const>(ParameterKeys).first(Version2ParameterKeyCount);
				std::string const listPath = Member(functionPath, key);
				std::unordered_set<std::string> names;
				// Former names must differ from the names of all parameters of the list, later ones included, so they are read
				// once every parameter is: the raw list and the parameter's path of each kept parameter.
				std::vector<std::pair<Json const*, std::string>> pendingFormerNames;
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& value = (*list)[i];
					std::string const path = Element(listPath, i);
					if (!value.is_object())
					{
						Report(path, "expected an object; the parameter is dropped");
						continue;
					}
					if (Result<void> keys = CheckKeys(value, parameterKeys, path); !keys)
					{
						return keys;
					}
					if (parameters.size() == Format::MaxParameters)
					{
						Report(path, fmt::format("more than {} parameters; the parameter is dropped", Format::MaxParameters));
						continue;
					}
					std::optional<std::string> name = ReadName(value, path, "parameter");
					if (!name)
					{
						continue;
					}
					if (names.contains(*name))
					{
						Report(Member(path, "Name"),
						       fmt::format("'{}' is the name of an earlier parameter; the parameter is dropped", *name));
						continue;
					}
					std::optional<std::string> type = ReadType(value, path, Format::MaxTypeBytes, "parameter");
					if (!type)
					{
						continue;
					}
					names.insert(*name);
					parameters.push_back({std::move(*name), std::move(*type)});
					auto const formerNames = HasVersion3Keys() ? value.find("FormerNames") : value.end();
					pendingFormerNames.emplace_back(formerNames == value.end() ? nullptr : &*formerNames, path);
				}

				std::unordered_set<std::string> takenFormerNames;
				for (size_t i = 0; i < parameters.size(); i++)
				{
					auto const& [formerNames, parameterPath] = pendingFormerNames[i];
					if (formerNames != nullptr)
					{
						ReadFormerNames(*formerNames, Member(parameterPath, "FormerNames"), names, "a parameter of this list",
						                takenFormerNames, parameters[i].FormerNames);
					}
				}
				return {};
			}

			[[nodiscard]] Result<void> ReadLocals(Json const& function, std::string_view functionPath,
			                                      std::vector<VisualScriptLocal>& locals)
			{
				Json const* const list = FindList(function, "Locals", functionPath);
				if (list == nullptr)
				{
					return {};
				}

				std::string const listPath = Member(functionPath, "Locals");
				std::unordered_set<std::string> names;
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& value = (*list)[i];
					std::string const path = Element(listPath, i);
					if (!value.is_object())
					{
						Report(path, "expected an object; the local is dropped");
						continue;
					}
					if (Result<void> keys = CheckKeys(value, LocalKeys, path); !keys)
					{
						return keys;
					}
					if (locals.size() == Format::MaxLocals)
					{
						Report(path, fmt::format("more than {} locals; the local is dropped", Format::MaxLocals));
						continue;
					}
					std::optional<uint32_t> const id = ReadNewID(value, path, "local");
					if (!id)
					{
						continue;
					}
					std::optional<std::string> name = ReadName(value, path, "local");
					if (!name)
					{
						continue;
					}
					if (names.contains(*name))
					{
						Report(Member(path, "Name"), fmt::format("'{}' is the name of an earlier local; the local is dropped", *name));
						continue;
					}
					if (m_VariableNames.contains(*name))
					{
						Report(Member(path, "Name"), fmt::format("'{}' is the name of a variable; the local is dropped", *name));
						continue;
					}
					std::optional<std::string> type = ReadType(value, path, Format::MaxTypeBytes, "local");
					if (!type)
					{
						continue;
					}

					VisualScriptLocal local;
					local.ID = Claim(*id);
					names.insert(*name);
					local.Name = std::move(*name);
					local.Type = std::move(*type);
					local.Value = ReadValue(value, path);
					locals.push_back(std::move(local));
				}
				return {};
			}

			// The graph under the owner's "Graph" key (the content object's or a function's).
			[[nodiscard]] Result<void> ReadGraph(Json const& owner, std::string_view ownerPath, VisualScriptGraph& graph)
			{
				auto const found = owner.find("Graph");
				if (found == owner.end())
				{
					return {};
				}
				std::string const path = Member(ownerPath, "Graph");
				if (!found->is_object())
				{
					Report(path, "expected an object; replaced by an empty graph");
					return {};
				}
				if (Result<void> keys = CheckKeys(*found, GraphKeys, path); !keys)
				{
					return keys;
				}

				std::unordered_set<uint32_t> nodeIDs;
				if (Result<void> nodes = ReadNodes(*found, path, graph, nodeIDs); !nodes)
				{
					return nodes;
				}
				if (Result<void> links = ReadLinks(*found, path, nodeIDs, graph); !links)
				{
					return links;
				}
				return ReadComments(*found, path, graph);
			}

			// The pins of one direction a node shows by field. Each list keeps a parent ahead of its fields, so an entry is
			// dropped when it is no pin path, when the list already holds MaxSplitPins entries or the same path, or when it is a
			// field of a path the list does not hold before it (a dropped path drops its fields with it).
			std::vector<std::string> ReadSplitPins(Json const& split, char const* key, std::string_view splitPath)
			{
				std::vector<std::string> paths;
				Json const* const list = FindList(split, key, splitPath);
				if (list == nullptr)
				{
					return paths;
				}

				std::string const listPath = Member(splitPath, key);
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& entry = (*list)[i];
					std::string const path = Element(listPath, i);
					if (!entry.is_string() || !Format::IsValidPinPath(entry.get_ref<std::string const&>()))
					{
						Report(path, fmt::format("expected a pin name or the path of a field pin (\"Hit.Point\"): names separated by '.', "
						                         "1 to {} bytes without control characters; dropped",
						                         Format::MaxPinNameBytes));
						continue;
					}
					if (paths.size() == Format::MaxSplitPins)
					{
						Report(path, fmt::format("more than {} split pins; dropped", Format::MaxSplitPins));
						continue;
					}
					std::string const& pin = entry.get_ref<std::string const&>();
					if (std::find(paths.begin(), paths.end(), pin) != paths.end())
					{
						Report(path, fmt::format("'{}' is already split; dropped", pin));
						continue;
					}
					size_t const field = pin.rfind('.');
					if (field != std::string::npos)
					{
						std::string_view const parent = std::string_view(pin).substr(0, field);
						if (std::find(paths.begin(), paths.end(), parent) == paths.end())
						{
							Report(path, fmt::format("'{}' is a field of '{}', which is not split before it; dropped", pin, parent));
							continue;
						}
					}
					paths.push_back(pin);
				}
				return paths;
			}

			[[nodiscard]] Result<VisualScriptSplitPins> ReadSplit(Json const& node, std::string_view nodePath)
			{
				auto const found = node.find("Split");
				if (found == node.end())
				{
					return VisualScriptSplitPins{};
				}
				std::string const path = Member(nodePath, "Split");
				if (!found->is_object())
				{
					Report(path, "expected an object; replaced by {}");
					return VisualScriptSplitPins{};
				}
				if (Result<void> keys = CheckKeys(*found, SplitKeys, path); !keys)
				{
					return Error{keys.GetError()};
				}
				VisualScriptSplitPins split;
				split.Inputs = ReadSplitPins(*found, "Inputs", path);
				split.Outputs = ReadSplitPins(*found, "Outputs", path);
				return split;
			}

			VisualScriptNodeEnabled ReadEnabled(Json const& node, std::string_view nodePath)
			{
				auto const found = node.find("Enabled");
				if (found == node.end())
				{
					return VisualScriptNodeEnabled::Enabled;
				}
				if (found->is_string())
				{
					if (std::optional<VisualScriptNodeEnabled> const enabled =
					        VisualScriptNodeEnabledFromString(found->get_ref<std::string const&>()))
					{
						return *enabled;
					}
				}
				Report(Member(nodePath, "Enabled"), "expected \"Enabled\", \"Disabled\" or \"DevelopmentOnly\"; replaced by \"Enabled\"");
				return VisualScriptNodeEnabled::Enabled;
			}

			[[nodiscard]] Result<void> ReadNodes(Json const& graphObject, std::string_view graphPath, VisualScriptGraph& graph,
			                                     std::unordered_set<uint32_t>& nodeIDs)
			{
				Json const* const list = FindList(graphObject, "Nodes", graphPath);
				if (list == nullptr)
				{
					return {};
				}

				// A version-2 key in a file of version 1 is an unknown key, read as such: no writer of version 1 wrote one.
				bool const hasVersion2Keys = m_Version >= 2;
				std::span<std::string_view const> const nodeKeys =
					hasVersion2Keys ? std::span<std::string_view const>(NodeKeys)
									: std::span<std::string_view const>(NodeKeys).first(Version1NodeKeyCount);
				std::string const listPath = Member(graphPath, "Nodes");
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& value = (*list)[i];
					std::string const path = Element(listPath, i);
					if (!value.is_object())
					{
						Report(path, "expected an object; the node is dropped");
						continue;
					}
					if (Result<void> keys = CheckKeys(value, nodeKeys, path); !keys)
					{
						return keys;
					}
					if (graph.Nodes.size() == Format::MaxNodesPerGraph)
					{
						Report(path, fmt::format("more than {} nodes in a graph; the node is dropped", Format::MaxNodesPerGraph));
						continue;
					}
					std::optional<uint32_t> const id = ReadNewID(value, path, "node");
					if (!id)
					{
						continue;
					}
					std::optional<std::string> type = ReadType(value, path, Format::MaxNodeTypeBytes, "node");
					if (!type)
					{
						continue;
					}

					VisualScriptNode node;
					node.ID = Claim(*id);
					nodeIDs.insert(node.ID);
					node.Type = std::move(*type);
					node.Position = ReadNodePosition(value, path);
					node.Settings = ReadMembers(value, "Settings", path);
					node.Inputs = ReadMembers(value, "Inputs", path);
					if (hasVersion2Keys)
					{
						Result<VisualScriptSplitPins> split = ReadSplit(value, path);
						if (!split)
						{
							return Error{split.GetError()};
						}
						node.Split = split.TakeValue();
						node.Comment = ReadText(value, "Comment", path);
						node.Collapsed = ReadBool(value, "Collapsed", path);
						node.Enabled = ReadEnabled(value, path);
					}
					graph.Nodes.push_back(std::move(node));
				}
				return {};
			}

			std::optional<VisualScriptPinReference> ReadEndpoint(Json const& link, char const* key, std::string_view linkPath,
			                                                     std::unordered_set<uint32_t> const& nodeIDs)
			{
				std::string const path = Member(linkPath, key);
				auto const found = link.find(key);
				if (found == link.end())
				{
					Report(path, "missing; the link is dropped");
					return std::nullopt;
				}
				std::optional<VisualScriptPinReference> endpoint = ToPinReference(*found);
				if (!endpoint)
				{
					Report(path,
					       fmt::format("expected [node ID, pin name] with a pin name of 1 to {} bytes without control characters; the "
					                   "link is dropped",
					                   Format::MaxPinNameBytes));
					return std::nullopt;
				}
				if (!nodeIDs.contains(endpoint->Node))
				{
					Report(path, fmt::format("node {} is not in this graph; the link is dropped", endpoint->Node));
					return std::nullopt;
				}
				return endpoint;
			}

			[[nodiscard]] Result<void> ReadLinks(Json const& graphObject, std::string_view graphPath,
			                                     std::unordered_set<uint32_t> const& nodeIDs, VisualScriptGraph& graph)
			{
				Json const* const list = FindList(graphObject, "Links", graphPath);
				if (list == nullptr)
				{
					return {};
				}

				std::string const listPath = Member(graphPath, "Links");
				std::set<std::tuple<uint32_t, std::string, uint32_t, std::string>> seen;
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& value = (*list)[i];
					std::string const path = Element(listPath, i);
					if (!value.is_object())
					{
						Report(path, "expected an object; the link is dropped");
						continue;
					}
					if (Result<void> keys = CheckKeys(value, LinkKeys, path); !keys)
					{
						return keys;
					}
					if (graph.Links.size() == Format::MaxLinksPerGraph)
					{
						Report(path, fmt::format("more than {} links in a graph; the link is dropped", Format::MaxLinksPerGraph));
						continue;
					}
					std::optional<VisualScriptPinReference> from = ReadEndpoint(value, "From", path, nodeIDs);
					if (!from)
					{
						continue;
					}
					std::optional<VisualScriptPinReference> to = ReadEndpoint(value, "To", path, nodeIDs);
					if (!to)
					{
						continue;
					}
					if (!seen.emplace(from->Node, from->Pin, to->Node, to->Pin).second)
					{
						Report(path, "repeats an earlier link; the link is dropped");
						continue;
					}
					graph.Links.push_back({std::move(*from), std::move(*to)});
				}
				return {};
			}

			[[nodiscard]] Result<void> ReadComments(Json const& graphObject, std::string_view graphPath, VisualScriptGraph& graph)
			{
				Json const* const list = FindList(graphObject, "Comments", graphPath);
				if (list == nullptr)
				{
					return {};
				}

				std::string const listPath = Member(graphPath, "Comments");
				VisualScriptComment const defaults;
				for (size_t i = 0; i < list->size(); i++)
				{
					Json const& value = (*list)[i];
					std::string const path = Element(listPath, i);
					if (!value.is_object())
					{
						Report(path, "expected an object; the comment is dropped");
						continue;
					}
					if (Result<void> keys = CheckKeys(value, CommentKeys, path); !keys)
					{
						return keys;
					}
					if (graph.Comments.size() == Format::MaxCommentsPerGraph)
					{
						Report(path, fmt::format("more than {} comments in a graph; the comment is dropped", Format::MaxCommentsPerGraph));
						continue;
					}
					std::optional<uint32_t> const id = ReadNewID(value, path, "comment");
					if (!id)
					{
						continue;
					}

					VisualScriptComment comment;
					comment.ID = Claim(*id);
					comment.Text = ReadText(value, "Text", path);
					comment.Position =
						ReadCommentVector(value, "Position", path, defaults.Position, -Format::MaxCoordinate, Format::MaxCoordinate);
					comment.Size = ReadCommentVector(value, "Size", path, defaults.Size, Format::MinCommentSize, Format::MaxCommentSize);
					comment.Color = ReadCommentColor(value, path);
					graph.Comments.push_back(std::move(comment));
				}
				return {};
			}

			DeserializationContext const& m_Context;
			int m_Version = Format::FormatVersion;
			std::vector<VisualScriptIssue> m_Issues;
			std::unordered_set<uint32_t> m_IDs;
			std::unordered_set<std::string> m_VariableNames;
		};

		Json SerializePin(VisualScriptPinReference const& pin)
		{
			return Json::array({pin.Node, pin.Pin});
		}

		Json SerializeGraph(VisualScriptGraph const& graph)
		{
			Json nodes = Json::array();
			for (VisualScriptNode const& node : graph.Nodes)
			{
				Json json = Json::object();
				json["ID"] = node.ID;
				json["Type"] = node.Type;
				if (node.Position)
				{
					json["Position"] = JsonTraits<glm::vec2>::ToJson(*node.Position);
				}
				// Empty (or null) Settings and Inputs are the default and are omitted.
				if (!node.Settings.empty())
				{
					json["Settings"] = node.Settings;
				}
				if (!node.Inputs.empty())
				{
					json["Inputs"] = node.Inputs;
				}
				if (!node.Split.IsEmpty())
				{
					Json split = Json::object();
					if (!node.Split.Inputs.empty())
					{
						split["Inputs"] = node.Split.Inputs;
					}
					if (!node.Split.Outputs.empty())
					{
						split["Outputs"] = node.Split.Outputs;
					}
					json["Split"] = std::move(split);
				}
				if (!node.Comment.empty())
				{
					json["Comment"] = node.Comment;
				}
				if (node.Collapsed)
				{
					json["Collapsed"] = true;
				}
				if (node.Enabled != VisualScriptNodeEnabled::Enabled)
				{
					json["Enabled"] = std::string(VisualScriptNodeEnabledToString(node.Enabled));
				}
				nodes.push_back(std::move(json));
			}

			Json links = Json::array();
			for (VisualScriptLink const& link : graph.Links)
			{
				Json json = Json::object();
				json["From"] = SerializePin(link.From);
				json["To"] = SerializePin(link.To);
				links.push_back(std::move(json));
			}

			Json comments = Json::array();
			for (VisualScriptComment const& comment : graph.Comments)
			{
				Json json = Json::object();
				json["ID"] = comment.ID;
				json["Text"] = comment.Text;
				json["Position"] = JsonTraits<glm::vec2>::ToJson(comment.Position);
				json["Size"] = JsonTraits<glm::vec2>::ToJson(comment.Size);
				json["Color"] = JsonTraits<glm::vec4>::ToJson(comment.Color);
				comments.push_back(std::move(json));
			}

			Json json = Json::object();
			json["Nodes"] = std::move(nodes);
			json["Links"] = std::move(links);
			json["Comments"] = std::move(comments);
			return json;
		}

		Json SerializeParameters(std::vector<VisualScriptParameter> const& parameters)
		{
			Json json = Json::array();
			for (VisualScriptParameter const& parameter : parameters)
			{
				Json entry = Json::object();
				entry["Name"] = parameter.Name;
				entry["Type"] = parameter.Type;
				if (!parameter.FormerNames.empty())
				{
					entry["FormerNames"] = parameter.FormerNames;
				}
				json.push_back(std::move(entry));
			}
			return json;
		}
	}

	bool VisualScriptSerializer::IsValidText(std::string_view text, size_t maxBytes)
	{
		return !text.empty() && text.size() <= maxBytes && !HasControlCharacters(text) && Utf8::IsValid(text);
	}

	bool VisualScriptSerializer::IsValidPinPath(std::string_view path)
	{
		return IsValidText(path, MaxPinNameBytes) && path.front() != '.' && path.back() != '.' && path.find("..") == std::string_view::npos;
	}

	bool VisualScriptSerializer::IsValidValue(Json const& value)
	{
		return IsValidValueAt(value, 1);
	}

	bool VisualScriptSerializer::IsValidName(std::string_view name)
	{
		// '.' is reserved for the paths of the fields of split struct pins.
		return IsValidText(name, MaxNameBytes) && name.front() != ' ' && name.back() != ' ' && name.find('.') == std::string_view::npos;
	}

	std::optional<float> VisualScriptSerializer::ReadFloat(Json const& value)
	{
		if (!value.is_number())
		{
			return std::nullopt;
		}
		double const number = value.get<double>();
		if (!std::isfinite(number) || std::abs(number) > MaxFloatMagnitude)
		{
			return std::nullopt;
		}
		// The numbers past float's largest value up to the bound round to it.
		constexpr double Largest = static_cast<double>(std::numeric_limits<float>::max());
		return static_cast<float>(std::clamp(number, -Largest, Largest));
	}

	Json VisualScriptSerializer::SerializeDocument(VisualScriptDocument const& document)
	{
		Json variables = Json::array();
		for (VisualScriptVariable const& variable : document.Variables)
		{
			Json json = Json::object();
			json["ID"] = variable.ID;
			json["Name"] = variable.Name;
			json["Type"] = variable.Type;
			if (!variable.Value.is_null())
			{
				json["Value"] = variable.Value;
			}
			if (variable.Public)
			{
				json["Public"] = true;
			}
			if (!variable.Tooltip.empty())
			{
				json["Tooltip"] = variable.Tooltip;
			}
			if (variable.Range)
			{
				json["Range"] = JsonTraits<glm::vec2>::ToJson(*variable.Range);
			}
			if (!variable.FormerNames.empty())
			{
				json["FormerNames"] = variable.FormerNames;
			}
			variables.push_back(std::move(json));
		}

		Json functions = Json::array();
		for (VisualScriptFunction const& function : document.Functions)
		{
			Json locals = Json::array();
			for (VisualScriptLocal const& local : function.Locals)
			{
				Json json = Json::object();
				json["ID"] = local.ID;
				json["Name"] = local.Name;
				json["Type"] = local.Type;
				if (!local.Value.is_null())
				{
					json["Value"] = local.Value;
				}
				locals.push_back(std::move(json));
			}

			Json json = Json::object();
			json["ID"] = function.ID;
			json["Name"] = function.Name;
			json["Pure"] = function.Pure;
			json["Inputs"] = SerializeParameters(function.Inputs);
			json["Outputs"] = SerializeParameters(function.Outputs);
			json["Locals"] = std::move(locals);
			json["Graph"] = SerializeGraph(function.Graph);
			functions.push_back(std::move(json));
		}

		Json content = Json::object();
		if (document.Kind != VisualScriptDocumentKind::Script)
		{
			content["Kind"] = std::string(VisualScriptDocumentKindToString(document.Kind));
		}
		content["Description"] = document.Description;
		content["Variables"] = std::move(variables);
		content["Functions"] = std::move(functions);
		content["Graph"] = SerializeGraph(document.Graph);
		return content;
	}

	Result<VisualScriptDocument> VisualScriptSerializer::DeserializeDocument(Json const& content, DeserializationContext const& context,
	                                                                         std::vector<VisualScriptIssue>* issues)
	{
		// The content form has no version: it has the keys of the current one.
		DocumentReader reader(context, FormatVersion);
		Result<VisualScriptDocument> document = reader.ReadContent(content);
		if (document)
		{
			reader.Publish(issues);
		}
		return document;
	}

	Json VisualScriptSerializer::Serialize(VisualScriptDocument const& document)
	{
		Json file = Json::object();
		file["Strada"] = MakeFileHeader("VisualScript", FormatVersion);
		file["VisualScript"] = SerializeDocument(document);
		return file;
	}

	Result<VisualScriptDocument> VisualScriptSerializer::Deserialize(Json const& file, DeserializationContext const& context,
	                                                                 std::vector<VisualScriptIssue>* issues)
	{
		Result<int> const header = ReadFileHeader(file, "VisualScript", FormatVersion);
		if (!header)
		{
			return Error{header.GetError()};
		}
		auto const content = file.find("VisualScript");
		if (content == file.end() || !content->is_object())
		{
			return Error{"visual script file has no \"VisualScript\" object"};
		}

		DocumentReader reader(context, header.GetValue());
		if (Result<void> keys = reader.CheckKeys(file, FileKeys, {}, "unknown key in the file; it is ignored"); !keys)
		{
			return Error{keys.GetError()};
		}
		if (Result<void> keys =
		        reader.CheckKeys(*file.find("Strada"), HeaderKeys, "Strada", "unknown key in the file header; it is ignored");
		    !keys)
		{
			return Error{keys.GetError()};
		}
		Result<VisualScriptDocument> document = reader.ReadContent(*content);
		if (document)
		{
			reader.Publish(issues);
		}
		return document;
	}

	Result<void> VisualScriptSerializer::SaveToFile(VisualScriptDocument const& document, std::filesystem::path const& path)
	{
		return FileSystem::WriteTextFile(path, DumpJson(Serialize(document)));
	}

	Result<VisualScriptDocument> VisualScriptSerializer::LoadFromFile(std::filesystem::path const& path,
	                                                                  DeserializationContext const& context,
	                                                                  std::vector<VisualScriptIssue>* issues)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			return Error{text.GetError()};
		}
		Result<Json> json = ParseJson(text.GetValue());
		if (!json)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), json.GetError());
		}
		Result<VisualScriptDocument> document = Deserialize(json.GetValue(), context, issues);
		if (!document)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), document.GetError());
		}
		return document;
	}
}
