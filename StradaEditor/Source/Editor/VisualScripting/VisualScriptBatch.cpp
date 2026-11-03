#include "Editor/VisualScripting/VisualScriptBatch.h"

#include "Editor/VisualScripting/VisualScriptArrange.h"
#include "Editor/VisualScripting/VisualScriptCanvasPins.h"
#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptCrossGraph.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptHeadlessCanvas.h"
#include "Editor/VisualScripting/VisualScriptLanguageEditing.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"

#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		using Format = VisualScriptSerializer;
		namespace Editing = VisualScriptEditing;

		// What an operation reads from the working document's analysis (spec §7.4's Needs).
		enum class BatchNeed : uint8_t
		{
			// Nothing.
			None = 0,
			// Pins: names and which are exec.
			Pins,
			// Pins with their resolved types.
			Types,
			// The whole analysis (its diagnostics).
			Analysis
		};

		// The level an operation changes (spec §7.4's Change): 0 layout and text (what HaveSameVisualScriptProgram ignores), 1
		// values, 2 links and types, 3 pins.
		constexpr uint8_t LayoutLevel = 0;
		constexpr uint8_t ValueLevel = 1;
		constexpr uint8_t LinkLevel = 2;
		constexpr uint8_t PinLevel = 3;
		// A rename of a variable, local or function changes no pin or type, but it retitles the nodes that show the name (Get
		// Speed, a call of AddScore), and so their widths: it is recorded as a link change, so that measured layout, which
		// reads the titles of the analysis, analyzes again rather than sizing those nodes by their former names.
		constexpr uint8_t TitleLevel = LinkLevel;

		// An operation kind, the fields it takes ('*' marks a required one) and the member that applies it.
		class BatchExecutor;
		using BatchHandler = bool (BatchExecutor::*)(Json& result);

		struct BatchOperation
		{
			std::string_view Name;
			std::string_view Fields;
			BatchHandler Handler = nullptr;
		};

		constexpr std::string_view LabelPrefix = "$";
		constexpr size_t MaxLabelCharacters = 63;
		constexpr float Grid = 16.0f;
		constexpr float ColumnGap = 64.0f;
		constexpr float NodeGap = 32.0f;
		constexpr float GroupGap = 112.0f;
		constexpr std::string_view AssetScheme = "asset://";
		constexpr std::string_view BuiltinScheme = "builtin://";

		bool IsLabelText(std::string_view text)
		{
			if (text.size() < 2 || text.size() > MaxLabelCharacters + 1 || !text.starts_with(LabelPrefix))
			{
				return false;
			}
			return std::all_of(text.begin() + 1, text.end(),
			                   [](char character)
			                   {
								   return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				                          (character >= '0' && character <= '9') || character == '_';
							   });
		}

		bool IsAssetReference(std::string_view text)
		{
			return text.starts_with(AssetScheme) || text.starts_with(BuiltinScheme);
		}

		// Whether a value holds an "asset://" or "builtin://" text: itself, or an element of an array.
		bool HoldsAssetReference(Json const& value)
		{
			if (value.is_string())
			{
				return IsAssetReference(value.get_ref<std::string const&>());
			}
			return value.is_array() && std::any_of(value.begin(), value.end(),
			                                       [](Json const& element)
			                                       {
													   return element.is_string() &&
				                                              IsAssetReference(element.get_ref<std::string const&>());
												   });
		}

		// Independent of the C locale, which std::tolower reads.
		char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// The edit distance of two names, ignoring the case of ASCII letters.
		size_t GetNameDistance(std::string_view a, std::string_view b)
		{
			std::vector<size_t> previous(b.size() + 1);
			std::vector<size_t> current(b.size() + 1);
			for (size_t j = 0; j <= b.size(); j++)
			{
				previous[j] = j;
			}
			for (size_t i = 1; i <= a.size(); i++)
			{
				current[0] = i;
				for (size_t j = 1; j <= b.size(); j++)
				{
					bool const same = ToLowerAscii(a[i - 1]) == ToLowerAscii(b[j - 1]);
					current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + (same ? 0 : 1)});
				}
				std::swap(previous, current);
			}
			return previous[b.size()];
		}

		// The three names closest to a name, the closest first (ties by name). The distance costs the product of two lengths:
		// a text far longer than every candidate is close to none of them, so only its start is compared, which keeps the
		// search bounded by the candidates whatever the text given (a 1 MB name would otherwise hold the editor for hours).
		Json FindClosestNames(std::string_view name, std::vector<std::string> names)
		{
			std::sort(names.begin(), names.end());
			names.erase(std::unique(names.begin(), names.end()), names.end());
			size_t longest = 0;
			for (std::string const& candidate : names)
			{
				longest = std::max(longest, candidate.size());
			}
			name = name.substr(0, std::min(name.size(), longest + Format::MaxNameBytes));
			std::vector<std::pair<size_t, std::string>> ranked;
			ranked.reserve(names.size());
			for (std::string& candidate : names)
			{
				size_t const distance = GetNameDistance(name, candidate);
				ranked.emplace_back(distance, std::move(candidate));
			}
			std::stable_sort(ranked.begin(), ranked.end(),
			                 [](auto const& a, auto const& b)
			                 {
								 return a.first < b.first;
							 });
			Json closest = Json::array();
			for (size_t index = 0; index < ranked.size() && index < 3; index++)
			{
				closest.push_back(ranked[index].second);
			}
			return closest;
		}

		char const* GetDirectionName(VisualScriptPinDirection direction)
		{
			return direction == VisualScriptPinDirection::In ? "In" : "Out";
		}

		Json DescribeEnd(VisualScriptPinReference const& end)
		{
			return Json::array({end.Node, end.Pin});
		}

		Json DescribeLink(VisualScriptLink const& link)
		{
			return Json::object({{"from", DescribeEnd(link.From)}, {"to", DescribeEnd(link.To)}});
		}

		// The links of a list that another list does not have (nor excluded), described as results describe them.
		Json DescribeLinksGone(std::vector<VisualScriptLink> const& before, std::vector<VisualScriptLink> const& after,
		                       std::span<VisualScriptLink const> excluded = {})
		{
			Json gone = Json::array();
			for (VisualScriptLink const& link : FindVisualScriptLinksGone(FindVisualScriptLinksGone(before, after), excluded))
			{
				gone.push_back(DescribeLink(link));
			}
			return gone;
		}

		// Links hashed by their ends, for the sets that compare link lists.
		struct LinkHash
		{
			size_t operator()(VisualScriptLink const* link) const
			{
				size_t hash = std::hash<uint32_t>()(link->From.Node);
				auto const combine = [&hash](size_t value)
				{
					hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
				};
				combine(std::hash<std::string_view>()(link->From.Pin));
				combine(std::hash<uint32_t>()(link->To.Node));
				combine(std::hash<std::string_view>()(link->To.Pin));
				return hash;
			}
		};

		struct LinkEqual
		{
			bool operator()(VisualScriptLink const* a, VisualScriptLink const* b) const { return *a == *b; }
		};

		// The graph of a sub-graph (0: the event graph, else a function's ID) of a document or a const one; null when none.
		template<typename TDocument>
		auto FindGraph(TDocument& document, uint32_t function) -> decltype(&document.Graph)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [function](VisualScriptFunction const& candidate)
			                                {
												return candidate.ID == function;
											});
			return found != document.Functions.end() ? &found->Graph : nullptr;
		}

		VisualScriptFunction const* FindFunction(VisualScriptDocument const& document, uint32_t function)
		{
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [function](VisualScriptFunction const& candidate)
			                                {
												return candidate.ID == function;
											});
			return found != document.Functions.end() ? &*found : nullptr;
		}

		// Where an element of a graph is: the sub-graph and whether it is a node.
		struct ElementPlace
		{
			uint32_t Function = 0;
			bool IsNode = false;
			VisualScriptNode const* Node = nullptr;
		};

		std::optional<ElementPlace> FindElement(VisualScriptDocument const& document, uint32_t id)
		{
			auto const search = [id](VisualScriptGraph const& graph, uint32_t function) -> std::optional<ElementPlace>
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					if (node.ID == id)
					{
						return ElementPlace{function, true, &node};
					}
				}
				for (VisualScriptComment const& comment : graph.Comments)
				{
					if (comment.ID == id)
					{
						return ElementPlace{function, false, nullptr};
					}
				}
				return std::nullopt;
			};
			if (std::optional<ElementPlace> found = search(document.Graph, 0))
			{
				return found;
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				if (std::optional<ElementPlace> found = search(function.Graph, function.ID))
				{
					return found;
				}
			}
			return std::nullopt;
		}

		// Every ID of a document: variables, locals, functions, nodes and comment boxes.
		std::vector<uint32_t> CollectIDs(VisualScriptDocument const& document)
		{
			std::vector<uint32_t> ids;
			auto const addGraph = [&ids](VisualScriptGraph const& graph)
			{
				for (VisualScriptNode const& node : graph.Nodes)
				{
					ids.push_back(node.ID);
				}
				for (VisualScriptComment const& comment : graph.Comments)
				{
					ids.push_back(comment.ID);
				}
			};
			for (VisualScriptVariable const& variable : document.Variables)
			{
				ids.push_back(variable.ID);
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				ids.push_back(function.ID);
				for (VisualScriptLocal const& local : function.Locals)
				{
					ids.push_back(local.ID);
				}
				addGraph(function.Graph);
			}
			addGraph(document.Graph);
			std::sort(ids.begin(), ids.end());
			return ids;
		}

		float FloorToGrid(float value)
		{
			return std::floor(value / Grid) * Grid;
		}

		float CeilToGrid(float value)
		{
			return std::ceil(value / Grid) * Grid;
		}

		struct Rect
		{
			glm::vec2 Min{0.0f};
			glm::vec2 Max{0.0f};

			bool Overlaps(Rect const& other) const
			{
				return Min.x < other.Max.x && other.Min.x < Max.x && Min.y < other.Max.y && other.Min.y < Max.y;
			}
		};

		std::optional<VisualScriptPinDirection> ReadDirection(std::string_view text)
		{
			if (text == "In")
			{
				return VisualScriptPinDirection::In;
			}
			if (text == "Out")
			{
				return VisualScriptPinDirection::Out;
			}
			return std::nullopt;
		}

		// A headless canvas of a sub-graph and what it was synced with.
		struct BatchCanvas
		{
			Scope<VisualScriptHeadlessCanvas> Canvas = CreateScope<VisualScriptHeadlessCanvas>();
			uint64_t Version = std::numeric_limits<uint64_t>::max();
			// What the canvas points at since its last Sync, held so that none of it goes before the next one even when the
			// batch has moved on to newer analyses and snapshots.
			Ref<VisualScriptAnalysis const> Analysis;
			Ref<VisualScriptDocument const> AnalyzedDocument;
		};

		// What a label names.
		enum class LabelKind : uint8_t
		{
			// A node or a comment box.
			Element = 0,
			Variable,
			Local,
			Function
		};

		struct Label
		{
			LabelKind Kind = LabelKind::Element;
			uint32_t ID = 0;
			// The operation whose change removed what the label names.
			std::optional<size_t> DeletedBy;
		};

		class BatchExecutor
		{
		public:
			BatchExecutor(VisualScriptDocument const& stored, VisualScriptBatchContext const& context)
				: m_Context(context),
				  m_Working(stored),
				  m_Library(context.Semantics.GetLibrary())
			{
				std::vector<uint32_t> const ids = CollectIDs(stored);
				m_StartIDs.insert(ids.begin(), ids.end());
				m_LiveIDs = ids;
				bool const cached = context.StoredAnalysis != nullptr && context.StoredAnalysisDocument != nullptr &&
				                    (context.StoredAnalysisDocument.get() == &stored || *context.StoredAnalysisDocument == stored);
				if (m_Library != nullptr && cached)
				{
					m_Analysis = context.StoredAnalysis;
					m_AnalyzedDocument = context.StoredAnalysisDocument;
				}
			}

			std::variant<VisualScriptBatchResult, VisualScriptBatchError> Run(Json const& operations);

			// The operations (BatchOperations names each): true with the result filled, or false with m_Error set.
			bool AddNode(Json& result);
			bool Delete(Json& result);
			bool Duplicate(Json& result);
			bool Paste(Json& result);
			bool Move(Json& result);
			bool SetPosition(Json& result);
			bool Connect(Json& result);
			bool Disconnect(Json& result);
			bool Relink(Json& result);
			bool Splice(Json& result);
			bool InsertReroute(Json& result);
			bool SetInput(Json& result);
			bool SetSetting(Json& result);
			bool SetSequenceOutputs(Json& result);
			bool SetSwitchCases(Json& result);
			bool SetEventParameters(Json& result);
			bool RenameCustomEvent(Json& result);
			bool SetArrayElements(Json& result);
			bool SetFormula(Json& result);
			bool SetTimeline(Json& result);
			bool SetKind(Json& result);
			bool Retarget(Json& result);
			bool SetNodeComment(Json& result);
			bool SetCollapsed(Json& result);
			bool SetEnabled(Json& result);
			bool SplitPin(Json& result);
			bool RecombinePin(Json& result);
			bool AddComment(Json& result);
			bool SetComment(Json& result);
			bool SetDescription(Json& result);
			bool AddVariable(Json& result);
			bool SetVariable(Json& result);
			bool DeleteVariable(Json& result);
			bool ReorderVariables(Json& result);
			bool DuplicateVariable(Json& result);
			bool AddLocal(Json& result);
			bool SetLocal(Json& result);
			bool DeleteLocal(Json& result);
			bool AddFunction(Json& result);
			bool RenameFunction(Json& result);
			bool SetFunctionSignature(Json& result);
			bool SetFunctionPure(Json& result);
			bool DeleteFunction(Json& result);
			bool DuplicateFunction(Json& result);
			bool PromoteToVariable(Json& result);
			bool PromoteToFunctionInput(Json& result);
			bool Arrange(Json& result);
			bool Align(Json& result);
			bool Distribute(Json& result);
			bool Straighten(Json& result);
			bool Fix(Json& result);

		private:
			// --- Errors ---

			// Fails the operation: the path is "operations[i]" and the suffix (".from[1]"), the message names the operation.
			bool Fail(std::string_view suffix, std::string const& message, Json data = Json(),
			          VisualScriptBatchFailure failure = VisualScriptBatchFailure::Invalid)
			{
				VisualScriptBatchError error;
				error.Failure = failure;
				error.Operation = m_Index;
				error.Kind = m_Kind == "?" ? std::string() : std::string(m_Kind);
				error.Path = fmt::format("operations[{}]{}", m_Index, suffix);
				error.Message = fmt::format("operations[{}] ({}): {}", m_Index, m_Kind, message);
				error.Data = std::move(data);
				m_Error = std::move(error);
				return false;
			}

			// Fails with a field's path and the field named first in the message ("to[0]: ...").
			bool FailField(std::string_view field, std::string const& message, Json data = Json(),
			               VisualScriptBatchFailure failure = VisualScriptBatchFailure::Invalid)
			{
				std::string const suffix = field.starts_with("[") ? std::string(field) : "." + std::string(field);
				return Fail(suffix, fmt::format("{}: {}", field, message), std::move(data), failure);
			}

			// --- Fields ---

			Json const* Find(std::string_view field) const
			{
				auto const found = m_Operation->find(std::string(field));
				return found != m_Operation->end() ? &*found : nullptr;
			}

			bool Has(std::string_view field) const { return Find(field) != nullptr; }

			// Each Read leaves the value as it is when the field is absent and fails when it has another type.
			bool Read(std::string_view field, std::string& value)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (!found->is_string())
				{
					return FailField(field, "expected a string");
				}
				value = found->get<std::string>();
				return true;
			}

			bool Read(std::string_view field, std::optional<std::string>& value)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (!found->is_string())
				{
					return FailField(field, "expected a string");
				}
				value = found->get<std::string>();
				return true;
			}

			bool Read(std::string_view field, std::optional<bool>& value)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (!found->is_boolean())
				{
					return FailField(field, "expected true or false");
				}
				value = found->get<bool>();
				return true;
			}

			bool Read(std::string_view field, std::optional<int64_t>& value)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (found->is_number_integer())
				{
					value = found->get<int64_t>();
					return true;
				}
				if (found->is_number_float())
				{
					double const number = found->get<double>();
					if (std::isfinite(number) && std::trunc(number) == number && std::abs(number) < 9.0e15)
					{
						value = static_cast<int64_t>(number);
						return true;
					}
				}
				return FailField(field, "expected an integer");
			}

			// [x, y]: finite, within the coordinates documents allow (sizes: positive).
			bool ReadPoint(std::string_view field, std::optional<glm::vec2>& value)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (!found->is_array() || found->size() != 2 || !(*found)[0].is_number() || !(*found)[1].is_number())
				{
					return FailField(field, "expected two numbers, [x, y]");
				}
				double const x = (*found)[0].get<double>();
				double const y = (*found)[1].get<double>();
				if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > Format::MaxCoordinate || std::abs(y) > Format::MaxCoordinate)
				{
					return FailField(field, fmt::format("expected finite numbers within +-{}", Format::MaxCoordinate));
				}
				value = glm::vec2(static_cast<float>(x), static_cast<float>(y));
				return true;
			}

			bool ReadColor(std::string_view field, std::optional<glm::vec4>& value)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (!found->is_array() || found->size() != 4 ||
				    !std::all_of(found->begin(), found->end(),
				                 [](Json const& component)
				                 {
									 return component.is_number();
								 }))
				{
					return FailField(field, "expected four numbers, linear [r, g, b, a] within 0 to 1");
				}
				glm::vec4 color{0.0f};
				for (glm::length_t index = 0; index < 4; index++)
				{
					double const component = (*found)[static_cast<size_t>(index)].get<double>();
					if (!std::isfinite(component) || component < 0.0 || component > 1.0)
					{
						return FailField(field, "expected four numbers, linear [r, g, b, a] within 0 to 1");
					}
					color[index] = static_cast<float>(component);
				}
				value = color;
				return true;
			}

			bool ReadDirectionField(std::string_view field, VisualScriptPinDirection& value)
			{
				std::string text;
				if (!Read(field, text))
				{
					return false;
				}
				std::optional<VisualScriptPinDirection> const direction = ReadDirection(text);
				if (!direction)
				{
					return FailField(field, "expected In or Out");
				}
				value = *direction;
				return true;
			}

			// A list of {name, type} (inputs, outputs, parameters).
			bool ReadParameters(std::string_view field, std::vector<VisualScriptParameter>& parameters)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (!found->is_array())
				{
					return FailField(field, "expected an array of { name, type }");
				}
				for (size_t index = 0; index < found->size(); index++)
				{
					Json const& entry = (*found)[index];
					auto const name = entry.is_object() ? entry.find("name") : Json::const_iterator();
					auto const type = entry.is_object() ? entry.find("type") : Json::const_iterator();
					if (!entry.is_object() || entry.size() != 2 || name == entry.end() || type == entry.end() || !name->is_string() ||
					    !type->is_string())
					{
						return FailField(fmt::format("{}[{}]", field, index), "expected { name, type }, both strings");
					}
					parameters.emplace_back(name->get<std::string>(), type->get<std::string>());
				}
				return true;
			}

			// --- Labels ---

			bool CheckNewLabel(std::string_view field, std::string const& label)
			{
				if (!IsLabelText(label))
				{
					return FailField(field, fmt::format("{} is no label: a label is $ and 1 to {} ASCII letters, digits or _", label,
					                                    MaxLabelCharacters));
				}
				if (m_Labels.contains(label))
				{
					return FailField(field, fmt::format("{} is defined already, by an earlier operation", label));
				}
				return true;
			}

			// The label of a field ("as"), checked before the operation runs, defined once it succeeded.
			bool ReadNewLabel(std::string_view field, std::optional<std::string>& label)
			{
				if (!Read(field, label))
				{
					return false;
				}
				return !label || CheckNewLabel(field, *label);
			}

			bool ReadNewLabels(std::string_view field, std::vector<std::string>& labels)
			{
				Json const* const found = Find(field);
				if (found == nullptr)
				{
					return true;
				}
				if (!found->is_array())
				{
					return FailField(field, "expected an array of labels");
				}
				for (size_t index = 0; index < found->size(); index++)
				{
					Json const& entry = (*found)[index];
					std::string const path = fmt::format("{}[{}]", field, index);
					if (!entry.is_string())
					{
						return FailField(path, "expected a label");
					}
					std::string label = entry.get<std::string>();
					if (!CheckNewLabel(path, label))
					{
						return false;
					}
					if (std::find(labels.begin(), labels.end(), label) != labels.end())
					{
						return FailField(path, fmt::format("{} is given twice", label));
					}
					labels.push_back(std::move(label));
				}
				return true;
			}

			void DefineLabel(std::string const& label, LabelKind kind, uint32_t id)
			{
				m_Labels[label] = Label{kind, id, std::nullopt};
				m_LabelOrder.emplace_back(label, id);
			}

			Json DescribeLabels() const
			{
				Json labels = Json::array();
				for (auto const& [label, id] : m_LabelOrder)
				{
					labels.push_back(label);
				}
				return labels;
			}

			std::string ListLabels() const
			{
				if (m_LabelOrder.empty())
				{
					return "none";
				}
				std::string list;
				for (auto const& [label, id] : m_LabelOrder)
				{
					list += list.empty() ? label : ", " + label;
				}
				return list;
			}

			// --- References ---

			// The element (a node, a comment box, or either) an integer or a label names.
			enum class ElementWanted : uint8_t
			{
				Node = 0,
				Comment,
				Either
			};

			std::optional<ElementPlace> ResolveElement(Json const& value, std::string_view field, ElementWanted wanted,
			                                           uint32_t* id = nullptr)
			{
				char const* const noun = wanted == ElementWanted::Node      ? "node"
				                         : wanted == ElementWanted::Comment ? "comment box"
				                                                            : "node or comment box";
				uint32_t element = 0;
				if (value.is_string())
				{
					std::string const& text = value.get_ref<std::string const&>();
					auto const label = m_Labels.find(text);
					if (label == m_Labels.end())
					{
						std::vector<std::string> labels;
						for (auto const& [name, labelled] : m_LabelOrder)
						{
							labels.push_back(name);
						}
						FailField(field, fmt::format("{} is not defined by an earlier operation; defined: {}", text, ListLabels()),
						          Json::object({{"labels", DescribeLabels()}, {"names", FindClosestNames(text, std::move(labels))}}));
						return std::nullopt;
					}
					if (label->second.Kind != LabelKind::Element)
					{
						char const* const kind = label->second.Kind == LabelKind::Variable ? "a variable"
						                         : label->second.Kind == LabelKind::Local  ? "a local"
						                                                                   : "a function";
						FailField(field, fmt::format("{} names {}, not a {}", text, kind, noun));
						return std::nullopt;
					}
					if (label->second.DeletedBy)
					{
						FailField(field, fmt::format("{} was deleted by operations[{}]", text, *label->second.DeletedBy));
						return std::nullopt;
					}
					element = label->second.ID;
				}
				else if (value.is_number())
				{
					std::optional<uint32_t> const start = ReadStartID(value, field, noun);
					if (!start)
					{
						return std::nullopt;
					}
					element = *start;
				}
				else
				{
					FailField(field, fmt::format("expected the ID of a {} or a label", noun));
					return std::nullopt;
				}
				std::optional<ElementPlace> const place = FindElement(m_Working, element);
				if (!place || (wanted == ElementWanted::Node && !place->IsNode) || (wanted == ElementWanted::Comment && place->IsNode))
				{
					FailField(field, fmt::format("{} is not a {}", element, noun));
					return std::nullopt;
				}
				if (id != nullptr)
				{
					*id = element;
				}
				return place;
			}

			// An integer reference: an ID the document had when the batch started, not deleted since.
			std::optional<uint32_t> ReadStartID(Json const& value, std::string_view field, std::string_view noun)
			{
				double const number = value.get<double>();
				if (!std::isfinite(number) || std::trunc(number) != number || number < 1.0 || number > Format::MaxID)
				{
					FailField(field, fmt::format("expected the ID of a {} (1 to {}) or a label", noun, Format::MaxID));
					return std::nullopt;
				}
				uint32_t const id = static_cast<uint32_t>(number);
				if (!m_StartIDs.contains(id))
				{
					FailField(field,
					          fmt::format("the visual script had no {} {} when the batch started: elements an operation creates "
					                      "are named by the label it defines with as",
					                      noun, id),
					          Json::object({{"labels", DescribeLabels()}}));
					return std::nullopt;
				}
				if (auto const deleted = m_Deleted.find(id); deleted != m_Deleted.end())
				{
					FailField(field, fmt::format("{} {} was deleted by operations[{}]", noun, id, deleted->second));
					return std::nullopt;
				}
				return id;
			}

			std::optional<ElementPlace> ResolveNodeField(std::string_view field, uint32_t& id)
			{
				Json const* const value = Find(field);
				return value != nullptr ? ResolveElement(*value, field, ElementWanted::Node, &id) : std::nullopt;
			}

			// A list of elements (nodes, comment boxes or either), each once, in order.
			bool ResolveElements(std::string_view field, ElementWanted wanted, std::vector<std::pair<uint32_t, ElementPlace>>& elements)
			{
				Json const* const list = Find(field);
				if (list == nullptr)
				{
					return true;
				}
				if (!list->is_array())
				{
					return FailField(field, "expected an array of IDs and labels");
				}
				for (size_t index = 0; index < list->size(); index++)
				{
					uint32_t id = 0;
					std::optional<ElementPlace> const place =
						ResolveElement((*list)[index], fmt::format("{}[{}]", field, index), wanted, &id);
					if (!place)
					{
						return false;
					}
					if (std::none_of(elements.begin(), elements.end(),
					                 [id](auto const& element)
					                 {
										 return element.first == id;
									 }))
					{
						elements.emplace_back(id, *place);
					}
				}
				return true;
			}

			// Elements of one sub-graph (the operations that measure or align them across one canvas).
			bool ResolveElementsOfOneGraph(std::string_view field, ElementWanted wanted, std::vector<uint32_t>& ids, uint32_t& function)
			{
				std::vector<std::pair<uint32_t, ElementPlace>> elements;
				if (!ResolveElements(field, wanted, elements))
				{
					return false;
				}
				for (size_t index = 0; index < elements.size(); index++)
				{
					if (index > 0 && elements[index].second.Function != function)
					{
						return FailField(fmt::format("{}[{}]", field, index),
						                 fmt::format("{} is in another graph than {}: the elements must be of one graph",
						                             elements[index].first, elements[0].first));
					}
					function = elements[index].second.Function;
					ids.push_back(elements[index].first);
				}
				return true;
			}

			// A sub-graph: 0 (the event graph) or a function's ID, a function's name or a label of one. Absent: fallback.
			std::optional<uint32_t> ResolveFunction(std::string_view field, std::optional<uint32_t> fallback)
			{
				Json const* const value = Find(field);
				if (value == nullptr)
				{
					if (fallback && (*fallback == 0 || FindFunction(m_Working, *fallback) != nullptr))
					{
						return fallback;
					}
					FailField(field, fallback ? fmt::format("the visual script has no function {}", *fallback)
					                          : std::string("a function is required"));
					return std::nullopt;
				}
				return ResolveFunctionValue(*value, field, true);
			}

			std::optional<uint32_t> ResolveFunctionValue(Json const& value, std::string_view field, bool eventGraph)
			{
				if (value.is_number())
				{
					double const number = value.get<double>();
					if (eventGraph && number == 0.0)
					{
						return 0u;
					}
					std::optional<uint32_t> const id = ReadStartID(value, field, "function");
					if (!id)
					{
						return std::nullopt;
					}
					if (FindFunction(m_Working, *id) == nullptr)
					{
						FailField(field, fmt::format("{} is not a function{}", *id, eventGraph ? " (0 is the event graph)" : ""));
						return std::nullopt;
					}
					return id;
				}
				if (!value.is_string())
				{
					FailField(field, "expected a function's ID or name, or a label");
					return std::nullopt;
				}
				std::string const& text = value.get_ref<std::string const&>();
				if (std::optional<uint32_t> const labelled = ResolveDeclarationLabel(text, field, LabelKind::Function))
				{
					return labelled;
				}
				if (m_Error)
				{
					return std::nullopt;
				}
				for (VisualScriptFunction const& function : m_Working.Functions)
				{
					if (function.Name == text)
					{
						return function.ID;
					}
				}
				std::vector<std::string> names;
				for (VisualScriptFunction const& function : m_Working.Functions)
				{
					names.push_back(function.Name);
				}
				FailField(field, fmt::format("the visual script has no function named {}", text),
				          Json::object({{"names", FindClosestNames(text, std::move(names))}, {"labels", DescribeLabels()}}));
				return std::nullopt;
			}

			// A label of a variable, local or function (label syntax and defined): its ID; nothing without an error for a text
			// that is no defined label, which is then a name; an error for a label of another kind or of what was deleted.
			std::optional<uint32_t> ResolveDeclarationLabel(std::string const& text, std::string_view field, LabelKind kind,
			                                                LabelKind alternative = LabelKind::Element)
			{
				if (!IsLabelText(text))
				{
					return std::nullopt;
				}
				auto const label = m_Labels.find(text);
				if (label == m_Labels.end())
				{
					return std::nullopt;
				}
				char const* const noun = kind == LabelKind::Variable ? "variable" : kind == LabelKind::Local ? "local" : "function";
				if (label->second.Kind != kind && (alternative == LabelKind::Element || label->second.Kind != alternative))
				{
					FailField(field, fmt::format("{} names {}, not a {}", text,
					                             label->second.Kind == LabelKind::Element    ? "a node or comment box"
					                             : label->second.Kind == LabelKind::Variable ? "a variable"
					                             : label->second.Kind == LabelKind::Local    ? "a local"
					                                                                         : "a function",
					                             noun));
					return std::nullopt;
				}
				if (label->second.DeletedBy)
				{
					FailField(field, fmt::format("{} was deleted by operations[{}]", text, *label->second.DeletedBy));
					return std::nullopt;
				}
				return label->second.ID;
			}

			// A variable of the document: its ID, its name or a label of one.
			std::optional<uint32_t> ResolveVariable(std::string_view field)
			{
				Json const* const value = Find(field);
				if (value == nullptr)
				{
					FailField(field, "a variable is required");
					return std::nullopt;
				}
				return ResolveVariableValue(*value, field);
			}

			std::optional<uint32_t> ResolveVariableValue(Json const& value, std::string_view field)
			{
				auto const findVariable = [this](uint32_t id)
				{
					return std::any_of(m_Working.Variables.begin(), m_Working.Variables.end(),
					                   [id](VisualScriptVariable const& variable)
					                   {
										   return variable.ID == id;
									   });
				};
				if (value.is_number())
				{
					std::optional<uint32_t> const id = ReadStartID(value, field, "variable");
					if (id && !findVariable(*id))
					{
						FailField(field, fmt::format("{} is not a variable", *id));
						return std::nullopt;
					}
					return id;
				}
				if (!value.is_string())
				{
					FailField(field, "expected a variable's ID or name, or a label");
					return std::nullopt;
				}
				std::string const& text = value.get_ref<std::string const&>();
				if (std::optional<uint32_t> const labelled = ResolveDeclarationLabel(text, field, LabelKind::Variable))
				{
					return labelled;
				}
				if (m_Error)
				{
					return std::nullopt;
				}
				std::vector<std::string> names;
				for (VisualScriptVariable const& variable : m_Working.Variables)
				{
					if (variable.Name == text)
					{
						return variable.ID;
					}
					names.push_back(variable.Name);
				}
				FailField(field, fmt::format("the visual script has no variable named {}", text),
				          Json::object({{"names", FindClosestNames(text, std::move(names))}, {"labels", DescribeLabels()}}));
				return std::nullopt;
			}

			// A local of a function: its ID, its name in that function or a label of one.
			std::optional<uint32_t> ResolveLocal(std::string_view field, uint32_t function)
			{
				Json const* const value = Find(field);
				VisualScriptFunction const* const owner = FindFunction(m_Working, function);
				if (value == nullptr || owner == nullptr)
				{
					FailField(field, "a local of a function is required");
					return std::nullopt;
				}
				auto const hasLocal = [owner](uint32_t id)
				{
					return std::any_of(owner->Locals.begin(), owner->Locals.end(),
					                   [id](VisualScriptLocal const& local)
					                   {
										   return local.ID == id;
									   });
				};
				if (value->is_number())
				{
					std::optional<uint32_t> const id = ReadStartID(*value, field, "local");
					if (id && !hasLocal(*id))
					{
						FailField(field, fmt::format("{} is not a local of function {}", *id, owner->Name));
						return std::nullopt;
					}
					return id;
				}
				if (!value->is_string())
				{
					FailField(field, "expected a local's ID or name, or a label");
					return std::nullopt;
				}
				std::string const& text = value->get_ref<std::string const&>();
				if (std::optional<uint32_t> const labelled = ResolveDeclarationLabel(text, field, LabelKind::Local))
				{
					if (!hasLocal(*labelled))
					{
						FailField(field, fmt::format("{} is not a local of function {}", text, owner->Name));
						return std::nullopt;
					}
					return labelled;
				}
				if (m_Error)
				{
					return std::nullopt;
				}
				std::vector<std::string> names;
				for (VisualScriptLocal const& local : owner->Locals)
				{
					if (local.Name == text)
					{
						return local.ID;
					}
					names.push_back(local.Name);
				}
				FailField(field, fmt::format("function {} has no local named {}", owner->Name, text),
				          Json::object({{"names", FindClosestNames(text, std::move(names))}, {"labels", DescribeLabels()}}));
				return std::nullopt;
			}

			// --- Pins ---

			// The analysis' entry of a node while it still describes the node's type; null when none does.
			VisualScriptAnalyzedNode const* FindAnalyzedNode(uint32_t node) const
			{
				if (m_Analysis == nullptr)
				{
					return nullptr;
				}
				VisualScriptAnalyzedNode const* const analyzed = m_Analysis->FindNode(node);
				std::optional<ElementPlace> const place = FindElement(m_Working, node);
				if (analyzed == nullptr || !place || !place->IsNode ||
				    (place->Node->Type != analyzed->Type && place->Node->Type != analyzed->CanonicalType))
				{
					return nullptr;
				}
				return analyzed;
			}

			std::string ToStoredPin(uint32_t node, VisualScriptPinDirection direction, std::string_view pin) const
			{
				return Editing::ToStoredVisualScriptPin(FindAnalyzedNode(node), direction, pin);
			}

			// One end of a link, [node, "Pin"]: the node, its sub-graph, the pin as the document stores it and as it was given
			// (an operation that analyzes before it uses the end stores the given name again with the new analysis).
			struct LinkEnd
			{
				VisualScriptPinReference End;
				uint32_t Function = 0;
				std::string Given;
			};

			std::optional<LinkEnd> ResolveEnd(Json const& value, std::string_view field, VisualScriptPinDirection direction)
			{
				if (!value.is_array() || value.size() != 2 || !value[1].is_string())
				{
					FailField(field, fmt::format("expected [node, \"{}\"]: a node's ID or label and a pin's name",
					                             direction == VisualScriptPinDirection::Out ? "Output" : "Input"));
					return std::nullopt;
				}
				uint32_t id = 0;
				std::optional<ElementPlace> const place = ResolveElement(value[0], fmt::format("{}[0]", field), ElementWanted::Node, &id);
				if (!place)
				{
					return std::nullopt;
				}
				std::string const& pin = value[1].get_ref<std::string const&>();
				if (!Format::IsValidPinPath(pin))
				{
					FailField(fmt::format("{}[1]", field), fmt::format("{} is no pin name", pin));
					return std::nullopt;
				}
				return LinkEnd{{id, ToStoredPin(id, direction, pin)}, place->Function, pin};
			}

			// The end's pin as the document stores the name given, by the analysis the operation now has.
			void StoreAgain(LinkEnd& end, VisualScriptPinDirection direction) const
			{
				end.End.Pin = ToStoredPin(end.End.Node, direction, end.Given);
			}

			// A link { from, to } of one sub-graph.
			struct LinkReference
			{
				LinkEnd From;
				LinkEnd To;

				VisualScriptLink GetLink() const { return VisualScriptLink{From.End, To.End}; }
			};

			std::optional<LinkReference> ResolveLink(Json const& value, std::string_view field)
			{
				auto const from = value.is_object() ? value.find("from") : Json::const_iterator();
				auto const to = value.is_object() ? value.find("to") : Json::const_iterator();
				if (!value.is_object() || value.size() != 2 || from == value.end() || to == value.end())
				{
					FailField(field, "expected { from: [node, \"Output\"], to: [node, \"Input\"] }");
					return std::nullopt;
				}
				std::optional<LinkEnd> out = ResolveEnd(*from, fmt::format("{}.from", field), VisualScriptPinDirection::Out);
				if (!out)
				{
					return std::nullopt;
				}
				std::optional<LinkEnd> in = ResolveEnd(*to, fmt::format("{}.to", field), VisualScriptPinDirection::In);
				if (!in)
				{
					return std::nullopt;
				}
				if (out->Function != in->Function)
				{
					FailField(field, "the nodes are in different graphs: a link joins nodes of one graph");
					return std::nullopt;
				}
				return LinkReference{std::move(*out), std::move(*in)};
			}

			void StoreAgain(LinkReference& link) const
			{
				StoreAgain(link.From, VisualScriptPinDirection::Out);
				StoreAgain(link.To, VisualScriptPinDirection::In);
			}

			bool ResolveLinks(std::string_view field, std::vector<LinkReference>& links, std::optional<uint32_t>& function)
			{
				Json const* const list = Find(field);
				if (list == nullptr)
				{
					return true;
				}
				if (!list->is_array())
				{
					return FailField(field, "expected an array of { from, to }");
				}
				for (size_t index = 0; index < list->size(); index++)
				{
					std::string const path = fmt::format("{}[{}]", field, index);
					std::optional<LinkReference> link = ResolveLink((*list)[index], path);
					if (!link)
					{
						return false;
					}
					if (function && *function != link->From.Function)
					{
						return FailField(path, "the link is in another graph than the others: the links must be of one graph");
					}
					function = link->From.Function;
					links.push_back(std::move(*link));
				}
				return true;
			}

			// The rows of a node's pins, as an error's data describes them.
			static Json DescribePins(std::vector<VisualScriptNodePin> const& rows)
			{
				Json pins = Json::array();
				for (VisualScriptNodePin const& row : rows)
				{
					pins.push_back(Json::object({{"name", row.Name}, {"direction", GetDirectionName(row.Direction)}, {"type", row.Type}}));
				}
				return pins;
			}

			// Fails, with the node's pins, when the node has no such pin.
			bool CheckPin(VisualScriptNodePins const& pins, VisualScriptPinReference const& end, VisualScriptPinDirection direction,
			              std::string_view field)
			{
				std::vector<VisualScriptNodePin> const rows = pins(end.Node);
				bool const found = std::any_of(rows.begin(), rows.end(),
				                               [&end, direction](VisualScriptNodePin const& row)
				                               {
												   return row.Direction == direction && row.Name == end.Pin;
											   });
				if (found)
				{
					return true;
				}
				return FailField(
					field,
					fmt::format("node {} has no {} {}", end.Node, direction == VisualScriptPinDirection::In ? "input" : "output", end.Pin),
					Json::object({{"pins", DescribePins(rows)}}));
			}

			// --- Analysis and canvases ---

			// Whether the working document has what the operation reads; analyzes it again when the changes since the last
			// analysis may have changed that (spec §7.5). degraded: an operation that runs without the scripting runtime, on the
			// canvas it infers from links and literals, instead of failing.
			bool Require(BatchNeed need, bool degraded = false)
			{
				if (need == BatchNeed::None)
				{
					return true;
				}
				if (m_Library == nullptr)
				{
					if (degraded)
					{
						return true;
					}
					char const* const knows = need == BatchNeed::Pins    ? "it knows the pins of nodes"
					                          : need == BatchNeed::Types ? "it knows the types of pins"
					                                                     : "it finds the problems a Fix repairs";
					return Fail({}, fmt::format("needs the scripting runtime: {}; without it, graph.set writes links as given", knows),
					            Json::object({{"operation", m_Index}}), VisualScriptBatchFailure::NeedsRuntime);
				}
				uint8_t const threshold = need == BatchNeed::Pins ? PinLevel : need == BatchNeed::Types ? LinkLevel : ValueLevel;
				if (m_Analysis != nullptr && m_Level < threshold)
				{
					return true;
				}
				m_Analyses++;
				Result<VisualScriptAnalysis> analysis = m_Context.Semantics.Analyze(m_Working, m_Context.Graph);
				if (!analysis)
				{
					return Fail({},
					            fmt::format("needs the analysis of the visual script, and the scripting runtime failed to analyze it: "
					                        "see the log ({})",
					                        analysis.GetError()),
					            Json::object({{"operation", m_Index}}), VisualScriptBatchFailure::AnalysisFailed);
				}
				m_Analysis = CreateRef<VisualScriptAnalysis const>(analysis.TakeValue());
				// The display stands for the document analyzed: canvases match nodes against it by their types, settings,
				// literal keys and splits (MatchesVisualScriptAnalysis), never by positions, so the copy canvases show serves.
				m_AnalyzedDocument = GetDisplay();
				m_Level = LayoutLevel;
				return true;
			}

			// The working document as the canvases show it (unplaced nodes placed), copied once per version and shared by the
			// canvases of every sub-graph.
			Ref<VisualScriptDocument const> const& GetDisplay()
			{
				if (m_DisplayVersion != m_Version || m_Display == nullptr)
				{
					m_Display = VisualScriptHeadlessCanvas::MakeDisplay(m_Working);
					m_DisplayVersion = m_Version;
					m_Displays++;
				}
				return m_Display;
			}

			// The canvas of a sub-graph, synced with the working document as it is now. Below pin level since the analysis,
			// the analysis describes every node's pins, so the canvas takes them for the document as it is (a literal added
			// since would otherwise make a node fall back to its template): the display then stands for the document analyzed.
			VisualScriptHeadlessCanvas const& GetCanvas(uint32_t function)
			{
				BatchCanvas& canvas = m_Canvases[function];
				if (canvas.Version != m_Version || canvas.Analysis != m_Analysis)
				{
					Ref<VisualScriptDocument const> const& display = GetDisplay();
					canvas.Analysis = m_Analysis;
					canvas.AnalyzedDocument = m_Analysis == nullptr ? nullptr : (m_Level < PinLevel ? display : m_AnalyzedDocument);
					canvas.Canvas->Sync(display, function, canvas.Analysis.get(), canvas.AnalyzedDocument.get(), m_Library.get());
					canvas.Version = m_Version;
				}
				return *canvas.Canvas;
			}

			// The pins of the nodes of every graph of the document (operations that remove nodes of several graphs). Valid
			// until the document next changes.
			VisualScriptNodePins GetDocumentPins()
			{
				auto graphs = CreateRef<std::unordered_map<uint32_t, VisualScriptNodePins>>();
				auto owners = CreateRef<std::unordered_map<uint32_t, uint32_t>>();
				auto const addGraph = [&](VisualScriptGraph const& graph, uint32_t function)
				{
					(*graphs)[function] = GetCanvas(function).GetPins();
					for (VisualScriptNode const& node : graph.Nodes)
					{
						(*owners)[node.ID] = function;
					}
				};
				addGraph(m_Working.Graph, 0);
				for (VisualScriptFunction const& function : m_Working.Functions)
				{
					addGraph(function.Graph, function.ID);
				}
				return [graphs, owners](uint32_t node) -> std::vector<VisualScriptNodePin>
				{
					auto const owner = owners->find(node);
					if (owner == owners->end())
					{
						return {};
					}
					auto const pins = graphs->find(owner->second);
					return pins != graphs->end() ? pins->second(node) : std::vector<VisualScriptNodePin>();
				};
			}

			bool ReadExtentsMode(VisualScriptExtentsMode& mode)
			{
				std::string text = "Measured";
				if (!Read("extents", text))
				{
					return false;
				}
				if (text != "Measured" && text != "Uniform")
				{
					return FailField("extents", "expected Measured or Uniform");
				}
				mode = text == "Uniform" ? VisualScriptExtentsMode::Uniform : VisualScriptExtentsMode::Measured;
				return true;
			}

			// The sizes of a sub-graph's nodes: measured by its canvas (the analysis made again when links may have changed
			// types; without the runtime, the degraded canvas), or uniform. Valid until the document next changes.
			std::optional<VisualScriptNodeExtents> GetExtents(uint32_t function, VisualScriptExtentsMode mode)
			{
				if (mode == VisualScriptExtentsMode::Uniform)
				{
					return VisualScriptNodeExtents(&Editing::GetDefaultVisualScriptNodeExtents);
				}
				if (!Require(BatchNeed::Types, true))
				{
					return std::nullopt;
				}
				return GetCanvas(function).GetExtents();
			}

			// --- Changes ---

			// After an operation that succeeded: the level it changed, and what it removed when it may remove elements.
			void RecordChange(uint8_t level)
			{
				m_Level = std::max(m_Level, level);
				m_Version++;
			}

			// Marks the IDs the last operation removed as deleted by it.
			void RecordRemovals()
			{
				std::vector<uint32_t> ids = CollectIDs(m_Working);
				std::vector<uint32_t> removed;
				std::set_difference(m_LiveIDs.begin(), m_LiveIDs.end(), ids.begin(), ids.end(), std::back_inserter(removed));
				for (uint32_t const id : removed)
				{
					if (m_StartIDs.contains(id))
					{
						m_Deleted.try_emplace(id, m_Index);
					}
					for (auto& [name, label] : m_Labels)
					{
						if (label.ID == id && !label.DeletedBy)
						{
							label.DeletedBy = m_Index;
						}
					}
				}
				m_LiveIDs = std::move(ids);
			}

			void RecordCreated(uint32_t id)
			{
				m_Created.push_back(id);
				m_LiveIDs.insert(std::upper_bound(m_LiveIDs.begin(), m_LiveIDs.end(), id), id);
			}

			// Records every ID above the largest the document had before the operation, in order: what it created.
			void RecordCreatedAbove(uint32_t largest)
			{
				std::vector<uint32_t> const ids = CollectIDs(m_Working);
				for (auto id = std::upper_bound(ids.begin(), ids.end(), largest); id != ids.end(); ++id)
				{
					m_Created.push_back(*id);
				}
				m_LiveIDs = ids;
			}

			// --- Values ---

			bool IsAssetType(std::string_view type) const
			{
				if (type == "Strada.AssetHandle")
				{
					return true;
				}
				VisualScriptTypeInfo const* const info = m_Library != nullptr ? m_Library->FindType(type) : nullptr;
				return info != nullptr && info->Kind == VisualScriptTypeKind::Asset;
			}

			// The element type of an array type ("Strada.Prefab[]"); empty for other types.
			std::string GetElementType(std::string_view type) const
			{
				VisualScriptTypeInfo const* const info = m_Library != nullptr ? m_Library->FindType(type) : nullptr;
				if (info != nullptr && !info->Element.empty())
				{
					return info->Element;
				}
				return type.ends_with("[]") ? std::string(type.substr(0, type.size() - 2)) : std::string();
			}

			// Stores "asset://" and "builtin://" texts of a value of an asset type (or an array of one) as the asset's decimal
			// handle; other values, and values of other types, stay as they are.
			bool ConvertAssetValue(Json& value, std::string_view type, std::string_view field)
			{
				if (!m_Context.FindAsset || !HoldsAssetReference(value))
				{
					return true;
				}
				auto const convert = [this, field](Json& text) -> bool
				{
					if (!text.is_string() || !IsAssetReference(text.get_ref<std::string const&>()))
					{
						return true;
					}
					std::string const reference = text.get<std::string>();
					std::optional<AssetHandle> const handle = m_Context.FindAsset(reference);
					if (!handle)
					{
						return FailField(field, fmt::format("no asset '{}' is registered", reference),
						                 Json::object({{"path", fmt::format("operations[{}].{}", m_Index, field)}}),
						                 VisualScriptBatchFailure::AssetNotFound);
					}
					text = handle->ToString();
					return true;
				};
				if (value.is_string())
				{
					return !IsAssetType(type) || convert(value);
				}
				std::string const element = GetElementType(type);
				if (element.empty() || !IsAssetType(element))
				{
					return true;
				}
				for (Json& entry : value)
				{
					if (!convert(entry))
					{
						return false;
					}
				}
				return true;
			}

			// The type of a node's input as the analysis has it (by the name given or the name stored); empty when unknown.
			std::string FindInputType(uint32_t node, std::string_view given, std::string_view stored) const
			{
				VisualScriptAnalyzedNode const* const analyzed = FindAnalyzedNode(node);
				if (analyzed == nullptr)
				{
					return {};
				}
				VisualScriptPin const* pin = analyzed->FindPin(given, VisualScriptPinDirection::In);
				if (pin == nullptr)
				{
					pin = analyzed->FindPin(stored, VisualScriptPinDirection::In);
				}
				return pin != nullptr ? pin->Type : std::string();
			}

			// Stores an input's literal (rule 4 for asset texts: the pin's type from the analysis, made again when links may
			// have changed it).
			bool StoreLiteral(uint32_t node, std::string const& given, Json value, std::string_view field)
			{
				if (HoldsAssetReference(value) && m_Context.FindAsset)
				{
					if (!Require(BatchNeed::Types))
					{
						return false;
					}
					std::string const stored = ToStoredPin(node, VisualScriptPinDirection::In, given);
					if (!ConvertAssetValue(value, FindInputType(node, given, stored), field))
					{
						return false;
					}
				}
				std::string stored = ToStoredPin(node, VisualScriptPinDirection::In, given);
				if (Result<void> const set = Editing::SetInput(m_Working, node, std::move(stored), std::move(value)); !set)
				{
					return FailField(field, set.GetError());
				}
				return true;
			}

			// A variable's or local's value: asset texts stored as handles when the declared type is an asset's (which the
			// library knows, so the runtime is needed then; no analysis is).
			bool ConvertDeclaredValue(Json& value, std::string_view type, std::string_view field)
			{
				if (!HoldsAssetReference(value) || !m_Context.FindAsset)
				{
					return true;
				}
				if (m_Library == nullptr)
				{
					return Fail({},
					            "needs the scripting runtime: it knows which types are assets; without it, graph.set writes "
					            "values as given",
					            Json::object({{"operation", m_Index}}), VisualScriptBatchFailure::NeedsRuntime);
				}
				return ConvertAssetValue(value, type, field);
			}

			// What a setting of a node kind is: the library's kind ("Variable", "Function", "Graph", "Value", ...); without the
			// library, the built-in settings that name elements and graphs.
			std::string GetSettingKind(std::string_view type, std::string_view setting) const
			{
				if (m_Library != nullptr)
				{
					if (VisualScriptNodeTemplate const* const node = m_Library->FindNode(type))
					{
						for (VisualScriptSetting const& candidate : node->Settings)
						{
							if (candidate.Name == setting)
							{
								return candidate.Kind;
							}
						}
					}
				}
				if (setting == "Graph")
				{
					return "Graph";
				}
				if (setting == "Variable" && (type == "Variable.Get" || type == "Variable.Set"))
				{
					return "Variable";
				}
				if (setting == "Function" && type == "Function.Call")
				{
					return "Function";
				}
				return {};
			}

			// A variable's, local's or function's ID for a name in a node's settings (spec §7.3 rule 3): with locals, the node's
			// function's locals first; then target's variables (or functions); with labels (target is the working document), a
			// label of an earlier operation first.
			bool ResolveSettingName(Json& value, std::string_view kind, uint32_t function, VisualScriptDocument const& target, bool labels,
			                        bool locals, std::string_view field)
			{
				std::string const text = value.get<std::string>();
				bool const local = labels;
				if (labels && IsLabelText(text))
				{
					std::optional<uint32_t> const labelled =
						kind == "Function" ? ResolveDeclarationLabel(text, field, LabelKind::Function)
										   : ResolveDeclarationLabel(text, field, LabelKind::Variable, LabelKind::Local);
					if (labelled)
					{
						value = *labelled;
						return true;
					}
					if (m_Error)
					{
						return false;
					}
				}
				std::vector<std::string> names;
				if (kind == "Function")
				{
					for (VisualScriptFunction const& candidate : target.Functions)
					{
						if (candidate.Name == text)
						{
							value = candidate.ID;
							return true;
						}
						names.push_back(candidate.Name);
					}
					return FailField(field, fmt::format("{} has no function named {}", local ? "the visual script" : "the graph", text),
					                 Json::object({{"names", FindClosestNames(text, std::move(names))}}));
				}
				if (locals)
				{
					if (VisualScriptFunction const* const owner = FindFunction(target, function))
					{
						for (VisualScriptLocal const& candidate : owner->Locals)
						{
							if (candidate.Name == text)
							{
								value = candidate.ID;
								return true;
							}
							names.push_back(candidate.Name);
						}
					}
				}
				for (VisualScriptVariable const& candidate : target.Variables)
				{
					if (candidate.Name == text)
					{
						value = candidate.ID;
						return true;
					}
					names.push_back(candidate.Name);
				}
				return FailField(field, fmt::format("{} has no variable named {}", local ? "the visual script" : "the graph", text),
				                 Json::object({{"names", FindClosestNames(text, std::move(names))}}));
			}

			// A node's settings as the document stores them (spec §7.3 rules 3 and 4): a Graph's asset text as its handle, names
			// of variables, locals and functions as their IDs (looked up in the graph the node names, the working document for
			// this script), and asset texts of a Value setting of an asset type as handles. A failure is reported at
			// "<field>.<setting>", or at field itself for SetSetting's one value (single).
			bool ConvertSettings(std::string_view type, Json& settings, uint32_t function, std::string_view field,
			                     Json const* existing = nullptr, bool single = false)
			{
				if (!settings.is_object())
				{
					return true;
				}
				auto const getPath = [field, single](std::string const& name)
				{
					return single ? std::string(field) : fmt::format("{}.{}", field, name);
				};
				for (auto& [name, value] : settings.items())
				{
					if (GetSettingKind(type, name) == "Graph" && value.is_string() &&
					    IsAssetReference(value.get_ref<std::string const&>()) && m_Context.FindAsset)
					{
						std::string const reference = value.get<std::string>();
						std::optional<AssetHandle> const handle = m_Context.FindAsset(reference);
						if (!handle)
						{
							std::string const path = getPath(name);
							return FailField(path, fmt::format("no asset '{}' is registered", reference),
							                 Json::object({{"path", fmt::format("operations[{}].{}", m_Index, path)}}),
							                 VisualScriptBatchFailure::AssetNotFound);
						}
						value = handle->ToString();
					}
				}
				// The graph the node names, from the settings given (a null one removed), else those it has.
				VisualScriptNode probe;
				probe.Settings = settings;
				if (existing != nullptr && !settings.contains("Graph") && existing->is_object() && existing->contains("Graph"))
				{
					probe.Settings["Graph"] = (*existing)["Graph"];
				}
				if (probe.Settings.contains("Graph") && probe.Settings["Graph"].is_null())
				{
					probe.Settings.erase("Graph");
				}
				bool const local = IsLocalVisualScriptReference(probe);
				std::optional<AssetHandle> const graph = ReadVisualScriptGraphSetting(probe);
				for (auto& [name, value] : settings.items())
				{
					std::string const kind = GetSettingKind(type, name);
					std::string const path = getPath(name);
					if ((kind == "Variable" || kind == "Function") && value.is_string())
					{
						// The edited script itself, named by its Graph, is the working document, as the earlier operations left it.
						VisualScriptDocument const* target = &m_Working;
						bool const self = !local && graph && m_Context.Graph.IsValid() && *graph == m_Context.Graph;
						if (!local && !self)
						{
							if (graph && m_Context.FindDocument)
							{
								target = m_Context.FindDocument(*graph);
							}
							else
							{
								target = nullptr;
							}
							if (target == nullptr)
							{
								return FailField(path, fmt::format("{} names the {} of another graph, whose document is not at hand",
								                                   value.get<std::string>(), kind == "Function" ? "function" : "variable"));
							}
						}
						if (!ResolveSettingName(value, kind, function, *target, local || self, local, path))
						{
							return false;
						}
					}
					else if (kind == "Value" && HoldsAssetReference(value))
					{
						auto const typeSetting = settings.find("Type");
						Json const* typed = typeSetting != settings.end() ? &*typeSetting : nullptr;
						if (typed == nullptr && existing != nullptr && existing->is_object() && existing->contains("Type"))
						{
							typed = &(*existing)["Type"];
						}
						if (typed != nullptr && typed->is_string() && !ConvertAssetValue(value, typed->get_ref<std::string const&>(), path))
						{
							return false;
						}
					}
				}
				return true;
			}

			// The document of a graph a node names: the working document for the edited script itself (spec §7.3 rule 3), else
			// the context's; null when none is at hand.
			VisualScriptDocument const* ResolveGraphDocument(AssetHandle graph) const
			{
				if (m_Context.Graph.IsValid() && graph == m_Context.Graph)
				{
					return &m_Working;
				}
				return m_Context.FindDocument ? m_Context.FindDocument(graph) : nullptr;
			}

			VisualScriptGraphResolver MakeGraphResolver() const
			{
				return [this](AssetHandle graph)
				{
					return ResolveGraphDocument(graph);
				};
			}

			// The other graphs for the quick fixes: their documents, this script's being the working document, and their names.
			VisualScriptQuickFixGraphs MakeFixGraphs() const
			{
				return VisualScriptQuickFixGraphs{MakeGraphResolver(), m_Context.FindGraphName};
			}

			// A typed Event.Send or Event.Broadcast given a Graph and a Name but no Parameters takes those of that graph's custom
			// event (spec §4.3): settings (the members given, a null one removed) over existing (the node's, when it has some).
			// A graph not at hand leaves them out (the analysis then says UncheckedGraph); a graph without the event fails at
			// "<field>.Name" with its custom events' names.
			bool FillEventParameters(std::string_view type, Json& settings, std::string_view field, Json const* existing = nullptr)
			{
				if ((type != "Event.Send" && type != "Event.Broadcast") || !settings.is_object())
				{
					return true;
				}
				VisualScriptNode patched;
				patched.Settings = existing != nullptr && existing->is_object() ? *existing : Json::object();
				for (auto const& [name, value] : settings.items())
				{
					if (value.is_null())
					{
						patched.Settings.erase(name);
					}
					else
					{
						patched.Settings[name] = value;
					}
				}
				std::optional<AssetHandle> const graph = ReadVisualScriptGraphSetting(patched);
				auto const name = patched.Settings.find("Name");
				if (!graph || patched.Settings.contains("Parameters") || name == patched.Settings.end() || !name->is_string())
				{
					return true;
				}
				VisualScriptDocument const* const target = ResolveGraphDocument(*graph);
				if (target == nullptr)
				{
					return true;
				}
				Result<Json> parameters = Editing::FindVisualScriptEventParameters(*target, name->get_ref<std::string const&>());
				if (!parameters)
				{
					std::vector<std::string> names;
					for (VisualScriptNode const& node : target->Graph.Nodes)
					{
						auto const eventName =
							node.Type == "Event.Custom" && node.Settings.is_object() ? node.Settings.find("Name") : Json::const_iterator();
						if (node.Type == "Event.Custom" && node.Settings.is_object() && eventName != node.Settings.end() &&
						    eventName->is_string() &&
						    std::find(names.begin(), names.end(), eventName->get_ref<std::string const&>()) == names.end())
						{
							names.push_back(eventName->get<std::string>());
						}
					}
					std::sort(names.begin(), names.end());
					return FailField(fmt::format("{}.Name", field), parameters.GetError(), Json::object({{"names", std::move(names)}}));
				}
				settings["Parameters"] = std::move(parameters.GetValue());
				return true;
			}

			// Retargets a node with settings already converted (Retarget, and SetSetting's route).
			bool ApplyRetarget(uint32_t node, Json settings, Json& result)
			{
				Result<uint32_t> const removed =
					Editing::RetargetVisualScriptNode(m_Working, node, std::move(settings), MakeGraphResolver());
				if (!removed)
				{
					return FailEdit(removed.GetError());
				}
				result["removedLinks"] = removed.GetValue();
				RecordChange(PinLevel);
				RecordRemovals();
				return true;
			}

			// --- Placement ---

			struct PlacedNode
			{
				uint32_t ID = 0;
				Rect Bounds;
			};

			// The nodes of a sub-graph with a position, not among excluded, with their bounds.
			std::vector<PlacedNode> GetPlacedNodes(uint32_t function, VisualScriptNodeExtents const& extents,
			                                       std::unordered_set<uint32_t> const& excluded) const
			{
				std::vector<PlacedNode> placed;
				if (VisualScriptGraph const* const graph = FindGraph(m_Working, function))
				{
					for (VisualScriptNode const& node : graph->Nodes)
					{
						if (node.Position && !excluded.contains(node.ID))
						{
							placed.push_back({node.ID, Rect{*node.Position, *node.Position + extents(node.ID)}});
						}
					}
				}
				return placed;
			}

			// Below the sub-graph's placed nodes: at the left-most one's left, 112 below the lowest bottom (on the 16 grid); (0, 0)
			// in a graph without them.
			glm::vec2 GetCornerBelow(std::span<PlacedNode const> placed) const
			{
				if (placed.empty())
				{
					return glm::vec2(0.0f);
				}
				float left = std::numeric_limits<float>::max();
				float bottom = std::numeric_limits<float>::lowest();
				for (PlacedNode const& node : placed)
				{
					left = std::min(left, node.Bounds.Min.x);
					bottom = std::max(bottom, node.Bounds.Max.y);
				}
				float const maximum = static_cast<float>(Format::MaxCoordinate);
				return glm::clamp(glm::vec2(FloorToGrid(left), CeilToGrid(bottom + GroupGap)), glm::vec2(-maximum), glm::vec2(maximum));
			}

			// The bounds of nodes of the working document.
			std::optional<Rect> GetBounds(std::span<uint32_t const> nodes, VisualScriptNodeExtents const& extents) const
			{
				std::optional<Rect> bounds;
				for (uint32_t const id : nodes)
				{
					std::optional<ElementPlace> const place = FindElement(m_Working, id);
					if (!place || !place->IsNode || !place->Node->Position)
					{
						continue;
					}
					Rect const rect{*place->Node->Position, *place->Node->Position + extents(id)};
					bounds = bounds ? Rect{glm::min(bounds->Min, rect.Min), glm::max(bounds->Max, rect.Max)} : rect;
				}
				return bounds;
			}

			// Arrange's Created scope (spec §8.2): the nodes this batch created in the sub-graph and gave no position, placed
			// beside the nodes they continue, group by group, then moved down past what they would overlap.
			bool ArrangeCreated(uint32_t function, VisualScriptNodeExtents const& extents, size_t& arranged)
			{
				VisualScriptGraph const* const graph = FindGraph(m_Working, function);
				if (graph == nullptr)
				{
					return true;
				}
				std::unordered_set<uint32_t> created;
				std::vector<uint32_t> order;
				for (VisualScriptNode const& node : graph->Nodes)
				{
					if (!node.Position && m_Unplaced.contains(node.ID))
					{
						created.insert(node.ID);
						order.push_back(node.ID);
					}
				}
				std::sort(order.begin(), order.end());
				arranged = order.size();
				if (order.empty())
				{
					return true;
				}

				// The groups: the created nodes connected by links among themselves.
				std::unordered_map<uint32_t, uint32_t> parents;
				for (uint32_t const id : order)
				{
					parents[id] = id;
				}
				auto const findRoot = [&parents](uint32_t id)
				{
					while (parents[id] != id)
					{
						parents[id] = parents[parents[id]];
						id = parents[id];
					}
					return id;
				};
				for (VisualScriptLink const& link : graph->Links)
				{
					if (created.contains(link.From.Node) && created.contains(link.To.Node))
					{
						uint32_t const a = findRoot(link.From.Node);
						uint32_t const b = findRoot(link.To.Node);
						parents[std::max(a, b)] = std::min(a, b);
					}
				}
				std::map<uint32_t, std::vector<uint32_t>> groups;
				for (uint32_t const id : order)
				{
					groups[findRoot(id)].push_back(id);
				}

				std::vector<PlacedNode> const placed = GetPlacedNodes(function, extents, created);
				auto const findPlaced = [&placed](uint32_t id) -> PlacedNode const*
				{
					auto const found = std::find_if(placed.begin(), placed.end(),
					                                [id](PlacedNode const& node)
					                                {
														return node.ID == id;
													});
					return found != placed.end() ? &*found : nullptr;
				};
				std::vector<std::vector<uint32_t>> blocks;
				for (auto const& [root, members] : groups)
				{
					std::unordered_set<uint32_t> const inGroup(members.begin(), members.end());
					std::vector<uint32_t> sources;
					PlacedNode const* consumer = nullptr;
					for (VisualScriptLink const& link : graph->Links)
					{
						if (inGroup.contains(link.To.Node) && findPlaced(link.From.Node) != nullptr &&
						    std::find(sources.begin(), sources.end(), link.From.Node) == sources.end())
						{
							sources.push_back(link.From.Node);
						}
						if (inGroup.contains(link.From.Node))
						{
							PlacedNode const* const target = findPlaced(link.To.Node);
							if (target != nullptr &&
							    (consumer == nullptr || std::tie(target->Bounds.Min.x, target->Bounds.Min.y, target->ID) <
							                                std::tie(consumer->Bounds.Min.x, consumer->Bounds.Min.y, consumer->ID)))
							{
								consumer = target;
							}
						}
					}
					Result<void> done;
					if (!sources.empty())
					{
						std::sort(sources.begin(), sources.end());
						done = Editing::ArrangeNodes(m_Working, function, members, extents, std::nullopt, sources);
					}
					else if (consumer != nullptr)
					{
						done = Editing::ArrangeNodes(m_Working, function, members, extents, glm::vec2(0.0f));
						std::optional<Rect> const bounds = done ? GetBounds(members, extents) : std::nullopt;
						if (bounds)
						{
							glm::vec2 const size = bounds->Max - bounds->Min;
							float const maximum = static_cast<float>(Format::MaxCoordinate);
							glm::vec2 const corner = glm::clamp(
								glm::vec2(FloorToGrid(consumer->Bounds.Min.x - ColumnGap - size.x), FloorToGrid(consumer->Bounds.Min.y)),
								glm::vec2(-maximum), glm::vec2(maximum));
							done = Editing::ArrangeNodes(m_Working, function, members, extents, corner);
						}
					}
					else
					{
						done = Editing::ArrangeNodes(m_Working, function, members, extents, GetCornerBelow(placed));
					}
					if (!done)
					{
						return Fail({}, done.GetError());
					}
					blocks.push_back(members);
				}

				// Each block in turn moves down until it overlaps, with the gap between nodes around it, no other node, comment box
				// or block placed before it.
				std::vector<Rect> obstacles;
				for (PlacedNode const& node : placed)
				{
					obstacles.push_back(node.Bounds);
				}
				for (VisualScriptComment const& comment : graph->Comments)
				{
					obstacles.push_back(Rect{comment.Position, comment.Position + comment.Size});
				}
				for (std::vector<uint32_t> const& block : blocks)
				{
					std::optional<Rect> bounds = GetBounds(block, extents);
					if (!bounds)
					{
						continue;
					}
					float offset = 0.0f;
					for (size_t guard = 0; guard <= obstacles.size(); guard++)
					{
						Rect const grown{bounds->Min - glm::vec2(NodeGap) + glm::vec2(0.0f, offset),
						                 bounds->Max + glm::vec2(NodeGap) + glm::vec2(0.0f, offset)};
						float needed = 0.0f;
						for (Rect const& obstacle : obstacles)
						{
							if (grown.Overlaps(obstacle))
							{
								needed = std::max(needed, obstacle.Max.y - grown.Min.y);
							}
						}
						if (needed <= 0.0f)
						{
							break;
						}
						offset += std::ceil(needed / Grid) * Grid;
					}
					if (offset > 0.0f)
					{
						if (Result<void> const moved = Editing::MoveElements(m_Working, function, block, glm::vec2(0.0f, offset)); !moved)
						{
							return Fail({}, moved.GetError());
						}
						bounds = GetBounds(block, extents);
					}
					for (uint32_t const id : block)
					{
						std::optional<Rect> const node = GetBounds(std::span(&id, 1), extents);
						if (node)
						{
							obstacles.push_back(*node);
						}
					}
				}
				return true;
			}

			// --- Shared parts of operations ---

			bool ApplySplit(bool split, Json& result);
			bool RenameVariableTo(uint32_t id, std::string const& name);
			VisualScriptEnumLookup GetEnumLookup() const
			{
				Ref<VisualScriptLibrary const> const library = m_Library;
				if (library == nullptr)
				{
					return {};
				}
				return [library](std::string_view type)
				{
					return library->FindEnum(type);
				};
			}

			// Fails with the editing operation's own message.
			bool FailEdit(std::string const& message) { return Fail({}, message); }

			// Sets an Expression's formula (SetFormula, and SetSetting's route) and fills the result with what it renamed and
			// removed.
			bool ApplyFormula(uint32_t node, std::string formula, Json& result);
			// Sets a Flow.Timeline's timeline (SetTimeline, and SetSetting's route, which renames no track) and fills the result
			// with the links it removed.
			bool ApplyTimeline(uint32_t node, Json const& setting, std::string_view field,
			                   std::span<Editing::VisualScriptTrackRename const> renames, Json& result);

			// Fails when a node of the elements (of one sub-graph) has no position yet: moving, aligning, distributing,
			// straightening, framing and promoting start from where nodes stand, which for a node added without a position only
			// Arrange decides (the window shows it where the commit will place it, a place no operation can know before then).
			bool RequirePlaced(uint32_t function, std::span<uint32_t const> elements, std::string_view field)
			{
				VisualScriptGraph const* const graph = FindGraph(m_Working, function);
				if (graph == nullptr)
				{
					return true;
				}
				std::unordered_set<uint32_t> const wanted(elements.begin(), elements.end());
				for (VisualScriptNode const& node : graph->Nodes)
				{
					if (!node.Position && wanted.contains(node.ID))
					{
						return FailField(field,
						                 fmt::format("node {} has no position yet: Arrange the nodes added without one (scope Created) "
						                             "or give it a position before moving, aligning, framing or promoting it",
						                             node.ID));
					}
				}
				return true;
			}

			VisualScriptBatchContext const& m_Context;
			VisualScriptDocument m_Working;
			Ref<VisualScriptLibrary const> m_Library;

			// The analysis the operations read, of m_AnalyzedDocument, and the highest level changed since it was made.
			Ref<VisualScriptAnalysis const> m_Analysis;
			Ref<VisualScriptDocument const> m_AnalyzedDocument;
			uint8_t m_Level = LayoutLevel;
			uint32_t m_Analyses = 0;
			// Counts the changes of the working document; canvases sync again when it moved.
			uint64_t m_Version = 0;
			std::map<uint32_t, BatchCanvas> m_Canvases;
			Ref<VisualScriptDocument const> m_Display;
			uint64_t m_DisplayVersion = std::numeric_limits<uint64_t>::max();
			uint32_t m_Displays = 0;

			// References.
			std::unordered_set<uint32_t> m_StartIDs;
			std::vector<uint32_t> m_LiveIDs;
			std::unordered_map<uint32_t, size_t> m_Deleted;
			std::unordered_map<std::string, Label> m_Labels;
			std::vector<std::pair<std::string, uint32_t>> m_LabelOrder;
			std::vector<uint32_t> m_Created;
			// Nodes AddNode created without a position (Arrange's Created scope).
			std::unordered_set<uint32_t> m_Unplaced;

			// The operation being applied.
			Json const* m_Operation = nullptr;
			size_t m_Index = 0;
			std::string_view m_Kind;
			std::optional<VisualScriptBatchError> m_Error;
		};

		constexpr std::array<BatchOperation, 51> BatchOperations = {{
			{"AddNode", "type* function position settings literals collapsed enabled as", &BatchExecutor::AddNode},
			{"Delete", "elements* bridge", &BatchExecutor::Delete},
			{"Duplicate", "elements* labels", &BatchExecutor::Duplicate},
			{"Paste", "clipboard* function at labels", &BatchExecutor::Paste},
			{"Move", "elements* offset*", &BatchExecutor::Move},
			{"SetPosition", "element* position*", &BatchExecutor::SetPosition},
			{"Connect", "from* to*", &BatchExecutor::Connect},
			{"Disconnect", "from to node", &BatchExecutor::Disconnect},
			{"Relink", "remove* add*", &BatchExecutor::Relink},
			{"Splice", "node* link* input output", &BatchExecutor::Splice},
			{"InsertReroute", "link* position* as", &BatchExecutor::InsertReroute},
			{"SetInput", "node* pin* value", &BatchExecutor::SetInput},
			{"SetSetting", "node* name* value", &BatchExecutor::SetSetting},
			{"SetSequenceOutputs", "node* count* insertAt removeAt", &BatchExecutor::SetSequenceOutputs},
			{"SetSwitchCases", "node* cases*", &BatchExecutor::SetSwitchCases},
			{"SetEventParameters", "node* parameters*", &BatchExecutor::SetEventParameters},
			{"RenameCustomEvent", "node* name*", &BatchExecutor::RenameCustomEvent},
			{"SetArrayElements", "node* count* insertAt removeAt", &BatchExecutor::SetArrayElements},
			{"SetFormula", "node* formula*", &BatchExecutor::SetFormula},
			{"SetTimeline", "node* timeline* renames", &BatchExecutor::SetTimeline},
			{"SetNodeComment", "nodes* text*", &BatchExecutor::SetNodeComment},
			{"SetCollapsed", "nodes* collapsed*", &BatchExecutor::SetCollapsed},
			{"SetEnabled", "nodes* enabled*", &BatchExecutor::SetEnabled},
			{"SplitPin", "node* pin* direction*", &BatchExecutor::SplitPin},
			{"RecombinePin", "node* pin* direction*", &BatchExecutor::RecombinePin},
			{"AddComment", "text* function elements position size color as", &BatchExecutor::AddComment},
			{"SetComment", "comment* text position size color", &BatchExecutor::SetComment},
			{"SetDescription", "description*", &BatchExecutor::SetDescription},
			{"SetKind", "kind*", &BatchExecutor::SetKind},
			{"AddVariable", "name* type* value public tooltip range as", &BatchExecutor::AddVariable},
			{"SetVariable", "variable* name type value public tooltip range formerNames", &BatchExecutor::SetVariable},
			{"DeleteVariable", "variable* removeNodes", &BatchExecutor::DeleteVariable},
			{"ReorderVariables", "variables*", &BatchExecutor::ReorderVariables},
			{"DuplicateVariable", "variable* name", &BatchExecutor::DuplicateVariable},
			{"AddLocal", "function* name* type* value as", &BatchExecutor::AddLocal},
			{"SetLocal", "function* local* name type value", &BatchExecutor::SetLocal},
			{"DeleteLocal", "function* local* removeNodes", &BatchExecutor::DeleteLocal},
			{"AddFunction", "name* pure inputs outputs as entryAs returnAs", &BatchExecutor::AddFunction},
			{"RenameFunction", "function* name*", &BatchExecutor::RenameFunction},
			{"SetFunctionSignature", "function* inputs* outputs*", &BatchExecutor::SetFunctionSignature},
			{"SetFunctionPure", "function* pure*", &BatchExecutor::SetFunctionPure},
			{"DeleteFunction", "function* removeCalls", &BatchExecutor::DeleteFunction},
			{"DuplicateFunction", "function* name", &BatchExecutor::DuplicateFunction},
			{"PromoteToVariable", "node* pin* direction* name as", &BatchExecutor::PromoteToVariable},
			{"PromoteToFunctionInput", "node* pin* name", &BatchExecutor::PromoteToFunctionInput},
			{"Retarget", "node* settings*", &BatchExecutor::Retarget},
			{"Arrange", "function scope nodes extents", &BatchExecutor::Arrange},
			{"Align", "elements* alignment* extents", &BatchExecutor::Align},
			{"Distribute", "elements* axis* extents", &BatchExecutor::Distribute},
			{"Straighten", "nodes*", &BatchExecutor::Straighten},
			{"Fix", "code* fix* node pin direction link variable function", &BatchExecutor::Fix},
		}};

		constexpr std::array<std::string_view, BatchOperations.size()> MakeBatchOperationNames()
		{
			std::array<std::string_view, BatchOperations.size()> names{};
			for (size_t index = 0; index < BatchOperations.size(); index++)
			{
				names[index] = BatchOperations[index].Name;
			}
			return names;
		}

		constexpr std::array<std::string_view, BatchOperations.size()> BatchOperationNames = MakeBatchOperationNames();

		// Whether a field list ("type* function as") has the field; required when it is marked '*'.
		bool TakesField(std::string_view fields, std::string_view field, bool* required = nullptr)
		{
			size_t start = 0;
			while (start < fields.size())
			{
				size_t end = fields.find(' ', start);
				end = end == std::string_view::npos ? fields.size() : end;
				std::string_view entry = fields.substr(start, end - start);
				bool const isRequired = entry.ends_with('*');
				if (isRequired)
				{
					entry.remove_suffix(1);
				}
				if (entry == field)
				{
					if (required != nullptr)
					{
						*required = isRequired;
					}
					return true;
				}
				start = end + 1;
			}
			return false;
		}

		std::variant<VisualScriptBatchResult, VisualScriptBatchError> BatchExecutor::Run(Json const& operations)
		{
			if (!operations.is_array())
			{
				VisualScriptBatchError error;
				error.Path = "operations";
				error.Message = "operations: expected an array of operations";
				return error;
			}
			std::vector<Json> results;
			results.reserve(operations.size());
			for (size_t index = 0; index < operations.size(); index++)
			{
				m_Index = index;
				m_Operation = &operations[index];
				m_Kind = "?";
				if (!m_Operation->is_object())
				{
					Fail({}, "expected an object { \"op\": \"<Kind>\", ... }");
					return *m_Error;
				}
				auto const kind = m_Operation->find("op");
				if (kind == m_Operation->end() || !kind->is_string())
				{
					Fail(".op", "op: expected the operation's kind (AddNode, Connect, ...)");
					return *m_Error;
				}
				std::string const& name = kind->get_ref<std::string const&>();
				auto const operation = std::find_if(BatchOperations.begin(), BatchOperations.end(),
				                                    [&name](BatchOperation const& candidate)
				                                    {
														return candidate.Name == name;
													});
				if (operation == BatchOperations.end())
				{
					std::vector<std::string> names(BatchOperationNames.begin(), BatchOperationNames.end());
					Fail(".op", fmt::format("op: {} is no operation", name),
					     Json::object({{"names", FindClosestNames(name, std::move(names))}}));
					return *m_Error;
				}
				m_Kind = operation->Name;

				// Fields the operation does not take are refused, even when another operation takes them; required ones present.
				for (auto const& [field, value] : m_Operation->items())
				{
					if (field != "op" && !TakesField(operation->Fields, field))
					{
						FailField(field, fmt::format("{} takes no {}", m_Kind, field));
						return *m_Error;
					}
				}
				std::string_view fields = operation->Fields;
				while (!fields.empty())
				{
					size_t const end = std::min(fields.find(' '), fields.size());
					std::string_view const entry = fields.substr(0, end);
					if (entry.ends_with('*') && !Has(entry.substr(0, entry.size() - 1)))
					{
						std::string_view const field = entry.substr(0, entry.size() - 1);
						FailField(field, fmt::format("{} requires {}", m_Kind, field));
						return *m_Error;
					}
					fields.remove_prefix(std::min(end + 1, fields.size()));
				}

				Json result = Json::object();
				if (!(this->*(operation->Handler))(result))
				{
					if (!m_Error)
					{
						Fail({}, "the operation failed");
					}
					return *m_Error;
				}
				results.push_back(std::move(result));
			}

			VisualScriptBatchResult batch;
			batch.Analyses = m_Analyses;
			batch.Displays = m_Displays;
			if (m_Analysis != nullptr && m_Level == LayoutLevel)
			{
				batch.Analysis = m_Analysis;
			}
			batch.Document = std::move(m_Working);
			batch.Labels = std::move(m_LabelOrder);
			batch.Results = std::move(results);
			batch.Created = std::move(m_Created);
			return batch;
		}

		// --- Nodes and links ---

		bool BatchExecutor::AddNode(Json& result)
		{
			std::string type;
			std::optional<glm::vec2> position;
			std::optional<bool> collapsed;
			std::optional<std::string> enabled;
			std::optional<std::string> label;
			if (!Read("type", type) || !ReadPoint("position", position) || !Read("collapsed", collapsed) || !Read("enabled", enabled) ||
			    !ReadNewLabel("as", label))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunction("function", m_Context.Function);
			if (!function)
			{
				return false;
			}
			std::optional<VisualScriptNodeEnabled> state;
			if (enabled)
			{
				state = VisualScriptNodeEnabledFromString(*enabled);
				if (!state)
				{
					return FailField("enabled", "expected Enabled, Disabled or DevelopmentOnly");
				}
			}
			Json settings = Json::object();
			if (Json const* const given = Find("settings"))
			{
				if (!given->is_object())
				{
					return FailField("settings", "expected an object of settings");
				}
				settings = *given;
				if (!ConvertSettings(type, settings, *function, "settings") || !FillEventParameters(type, settings, "settings"))
				{
					return false;
				}
			}
			Json const* const literals = Find("literals");
			if (literals != nullptr && !literals->is_object())
			{
				return FailField("literals", "expected an object: input pin -> value");
			}

			Result<uint32_t> const added = Editing::AddNode(m_Working, *function, type, std::move(settings), position);
			if (!added)
			{
				return FailEdit(added.GetError());
			}
			uint32_t const id = added.GetValue();
			RecordChange(PinLevel);
			RecordCreated(id);
			if (!position)
			{
				m_Unplaced.insert(id);
			}
			if (literals != nullptr)
			{
				for (auto const& [pin, value] : literals->items())
				{
					if (!StoreLiteral(id, pin, value, fmt::format("literals.{}", pin)))
					{
						return false;
					}
				}
			}
			if (collapsed && *collapsed)
			{
				if (Result<void> const set = Editing::SetNodeCollapsed(m_Working, std::span(&id, 1), true); !set)
				{
					return FailField("collapsed", set.GetError());
				}
			}
			if (state)
			{
				if (Result<void> const set = Editing::SetNodeEnabled(m_Working, std::span(&id, 1), *state); !set)
				{
					return FailField("enabled", set.GetError());
				}
			}
			// The node was recorded as added before its literals (an asset literal analyzes it); what came after changed values
			// and its state.
			RecordChange(state ? LinkLevel : ValueLevel);
			if (label)
			{
				DefineLabel(*label, LabelKind::Element, id);
			}
			result["id"] = id;
			return true;
		}

		bool BatchExecutor::Delete(Json& result)
		{
			std::optional<bool> bridge;
			if (!Read("bridge", bridge))
			{
				return false;
			}
			bool const bridging = bridge.value_or(true);
			std::vector<std::pair<uint32_t, ElementPlace>> elements;
			if (!ResolveElements("elements", ElementWanted::Either, elements))
			{
				return false;
			}
			if (bridging && !Require(BatchNeed::Pins, true))
			{
				return false;
			}
			// Every graph's pins taken before anything is removed: the canvases show the document before the operation, as a
			// window's show it before a delete.
			std::map<uint32_t, std::vector<uint32_t>> byGraph;
			for (auto const& [id, place] : elements)
			{
				byGraph[place.Function].push_back(id);
			}
			std::map<uint32_t, VisualScriptNodePins> pins;
			if (bridging && m_Library != nullptr)
			{
				for (auto const& [function, ids] : byGraph)
				{
					pins[function] = GetCanvas(function).GetPins();
				}
			}
			uint32_t nodes = 0;
			uint32_t comments = 0;
			Json notes = Json::array();
			for (auto const& [function, ids] : byGraph)
			{
				auto const graphPins = pins.find(function);
				Result<VisualScriptRemoval> removed =
					Editing::RemoveElements(m_Working, function, ids, graphPins != pins.end() ? &graphPins->second : nullptr, bridging);
				if (!removed)
				{
					return FailEdit(removed.GetError());
				}
				nodes += removed.GetValue().Nodes;
				comments += removed.GetValue().Comments;
				for (std::string& note : removed.GetValue().Notes)
				{
					notes.push_back(std::move(note));
				}
			}
			RecordChange(PinLevel);
			RecordRemovals();
			result["nodes"] = nodes;
			result["comments"] = comments;
			result["notes"] = std::move(notes);
			return true;
		}

		bool BatchExecutor::Duplicate(Json& result)
		{
			std::vector<std::string> labels;
			if (!ReadNewLabels("labels", labels))
			{
				return false;
			}
			std::vector<uint32_t> ids;
			uint32_t function = 0;
			if (!ResolveElementsOfOneGraph("elements", ElementWanted::Either, ids, function))
			{
				return false;
			}
			uint32_t const largest = Editing::GetLargestVisualScriptID(m_Working);
			Result<VisualScriptPasteResult> duplicated =
				DuplicateVisualScriptElements(m_Working, m_Context.Graph, function, ids, m_Library.get());
			if (!duplicated)
			{
				return FailEdit(duplicated.GetError());
			}
			VisualScriptPasteResult const pasted = duplicated.TakeValue();
			if (labels.size() > pasted.Pasted.size())
			{
				return FailField("labels",
				                 fmt::format("{} labels for the {} elements the duplicate made", labels.size(), pasted.Pasted.size()));
			}
			RecordChange(PinLevel);
			RecordCreatedAbove(largest);
			for (size_t index = 0; index < labels.size(); index++)
			{
				DefineLabel(labels[index], LabelKind::Element, pasted.Pasted[index]);
			}
			result["ids"] = pasted.Pasted;
			result["notes"] = pasted.Notes;
			return true;
		}

		bool BatchExecutor::Paste(Json& result)
		{
			std::string clipboard;
			std::optional<glm::vec2> at;
			std::vector<std::string> labels;
			if (!Read("clipboard", clipboard) || !ReadPoint("at", at) || !ReadNewLabels("labels", labels))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunction("function", m_Context.Function);
			if (!function)
			{
				return false;
			}
			Result<VisualScriptClipboardContent> const content = ReadVisualScriptClipboard(clipboard);
			if (!content)
			{
				return FailField("clipboard", content.GetError());
			}
			if (!at)
			{
				// Below the sub-graph's nodes, as Arrange places what a batch adds apart from them.
				VisualScriptNodeExtents const extents = GetCanvas(*function).GetExtents();
				at = GetCornerBelow(GetPlacedNodes(*function, extents, {}));
			}
			uint32_t const largest = Editing::GetLargestVisualScriptID(m_Working);
			Result<VisualScriptPasteResult> pasted =
				PasteVisualScriptClipboard(m_Working, m_Context.Graph, *function, content.GetValue(), *at, m_Library.get());
			if (!pasted)
			{
				return FailEdit(pasted.GetError());
			}
			VisualScriptPasteResult const done = pasted.TakeValue();
			if (labels.size() > done.Pasted.size())
			{
				return FailField("labels", fmt::format("{} labels for the {} elements the paste made", labels.size(), done.Pasted.size()));
			}
			RecordChange(PinLevel);
			RecordCreatedAbove(largest);
			for (size_t index = 0; index < labels.size(); index++)
			{
				DefineLabel(labels[index], LabelKind::Element, done.Pasted[index]);
			}
			result["ids"] = done.Pasted;
			result["notes"] = done.Notes;
			return true;
		}

		bool BatchExecutor::Move(Json& result)
		{
			std::optional<glm::vec2> offset;
			if (!ReadPoint("offset", offset))
			{
				return false;
			}
			std::vector<std::pair<uint32_t, ElementPlace>> elements;
			if (!ResolveElements("elements", ElementWanted::Either, elements))
			{
				return false;
			}
			std::map<uint32_t, std::vector<uint32_t>> byGraph;
			for (auto const& [id, place] : elements)
			{
				byGraph[place.Function].push_back(id);
			}
			for (auto const& [function, ids] : byGraph)
			{
				if (!RequirePlaced(function, ids, "elements"))
				{
					return false;
				}
			}
			for (auto const& [function, ids] : byGraph)
			{
				if (Result<void> const moved = Editing::MoveElements(m_Working, function, ids, *offset); !moved)
				{
					return FailEdit(moved.GetError());
				}
			}
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::SetPosition(Json& result)
		{
			std::optional<glm::vec2> position;
			if (!ReadPoint("position", position))
			{
				return false;
			}
			uint32_t id = 0;
			Json const* const element = Find("element");
			std::optional<ElementPlace> const place =
				element != nullptr ? ResolveElement(*element, "element", ElementWanted::Either, &id) : std::nullopt;
			if (!place)
			{
				return false;
			}
			std::pair<uint32_t, glm::vec2> const entry{id, *position};
			if (Result<void> const set = Editing::SetPositions(m_Working, place->Function, std::span(&entry, 1)); !set)
			{
				return FailEdit(set.GetError());
			}
			m_Unplaced.erase(id);
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::Connect(Json& result)
		{
			std::optional<LinkEnd> from = ResolveEnd(*Find("from"), "from", VisualScriptPinDirection::Out);
			if (!from)
			{
				return false;
			}
			std::optional<LinkEnd> to = ResolveEnd(*Find("to"), "to", VisualScriptPinDirection::In);
			if (!to)
			{
				return false;
			}
			if (from->Function != to->Function)
			{
				return FailField("to", "the nodes are in different graphs: a link joins nodes of one graph");
			}
			if (!Require(BatchNeed::Pins))
			{
				return false;
			}
			StoreAgain(*from, VisualScriptPinDirection::Out);
			StoreAgain(*to, VisualScriptPinDirection::In);
			VisualScriptNodePins const pins = GetCanvas(from->Function).GetPins();
			if (!CheckPin(pins, from->End, VisualScriptPinDirection::Out, "from[1]") ||
			    !CheckPin(pins, to->End, VisualScriptPinDirection::In, "to[1]"))
			{
				return false;
			}
			std::vector<VisualScriptLink> const before = FindGraph(m_Working, from->Function)->Links;
			VisualScriptLink const link{from->End, to->End};
			if (Result<uint32_t> const connected = Editing::Connect(m_Working, from->Function, link, pins); !connected)
			{
				return FailEdit(connected.GetError());
			}
			result["replaced"] = DescribeLinksGone(before, FindGraph(m_Working, from->Function)->Links);
			RecordChange(LinkLevel);
			return true;
		}

		bool BatchExecutor::Disconnect(Json& result)
		{
			Json const* const fromField = Find("from");
			Json const* const toField = Find("to");
			Json const* const nodeField = Find("node");
			if (nodeField != nullptr && (fromField != nullptr || toField != nullptr))
			{
				return FailField("node", "Disconnect takes node alone, or from and/or to");
			}
			if (nodeField == nullptr && fromField == nullptr && toField == nullptr)
			{
				return Fail({}, "Disconnect takes from and/or to, or node");
			}
			std::vector<VisualScriptLink> links;
			uint32_t function = 0;
			if (nodeField != nullptr)
			{
				uint32_t id = 0;
				std::optional<ElementPlace> const place = ResolveElement(*nodeField, "node", ElementWanted::Node, &id);
				if (!place)
				{
					return false;
				}
				function = place->Function;
				for (VisualScriptLink const& link : FindGraph(m_Working, function)->Links)
				{
					if (link.From.Node == id || link.To.Node == id)
					{
						links.push_back(link);
					}
				}
			}
			else
			{
				std::optional<LinkEnd> from;
				std::optional<LinkEnd> to;
				if (fromField != nullptr && !(from = ResolveEnd(*fromField, "from", VisualScriptPinDirection::Out)))
				{
					return false;
				}
				if (toField != nullptr && !(to = ResolveEnd(*toField, "to", VisualScriptPinDirection::In)))
				{
					return false;
				}
				if (from && to && from->Function != to->Function)
				{
					return FailField("to", "the nodes are in different graphs: a link joins nodes of one graph");
				}
				function = from ? from->Function : to->Function;
				for (VisualScriptLink const& link : FindGraph(m_Working, function)->Links)
				{
					if ((!from || link.From == from->End) && (!to || link.To == to->End))
					{
						links.push_back(link);
					}
				}
				if (from && to && links.empty())
				{
					return Fail({}, fmt::format("the graph has no link from {}.{} to {}.{}", from->End.Node, from->End.Pin, to->End.Node,
					                            to->End.Pin));
				}
			}
			if (Result<void> const removed = Editing::Disconnect(m_Working, function, links); !removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removed"] = links.size();
			RecordChange(LinkLevel);
			return true;
		}

		bool BatchExecutor::Relink(Json& result)
		{
			std::vector<LinkReference> removed;
			std::vector<LinkReference> added;
			std::optional<uint32_t> function;
			if (!ResolveLinks("remove", removed, function) || !ResolveLinks("add", added, function))
			{
				return false;
			}
			if (!function)
			{
				return Fail({}, "Relink takes links to remove or to add");
			}
			if (!Require(BatchNeed::Pins))
			{
				return false;
			}
			std::vector<VisualScriptLink> remove;
			std::vector<VisualScriptLink> add;
			for (LinkReference& link : removed)
			{
				StoreAgain(link);
				remove.push_back(link.GetLink());
			}
			for (LinkReference& link : added)
			{
				StoreAgain(link);
				add.push_back(link.GetLink());
			}
			VisualScriptNodePins const pins = GetCanvas(*function).GetPins();
			for (size_t index = 0; index < add.size(); index++)
			{
				if (!CheckPin(pins, add[index].From, VisualScriptPinDirection::Out, fmt::format("add[{}].from[1]", index)) ||
				    !CheckPin(pins, add[index].To, VisualScriptPinDirection::In, fmt::format("add[{}].to[1]", index)))
				{
					return false;
				}
			}
			std::vector<VisualScriptLink> const before = FindGraph(m_Working, *function)->Links;
			if (Result<uint32_t> const relinked = Editing::Relink(m_Working, *function, remove, add, pins); !relinked)
			{
				return FailEdit(relinked.GetError());
			}
			result["replaced"] = DescribeLinksGone(before, FindGraph(m_Working, *function)->Links, remove);
			RecordChange(LinkLevel);
			return true;
		}

		bool BatchExecutor::Splice(Json& result)
		{
			uint32_t node = 0;
			std::optional<ElementPlace> const place = ResolveNodeField("node", node);
			if (!place)
			{
				return false;
			}
			std::optional<LinkReference> link = ResolveLink(*Find("link"), "link");
			if (!link)
			{
				return false;
			}
			if (link->From.Function != place->Function)
			{
				return FailField("link", fmt::format("the link is in another graph than node {}", node));
			}
			std::string input = "Execute";
			std::optional<std::string> output;
			if (!Read("input", input) || !Read("output", output))
			{
				return false;
			}
			if (!Require(BatchNeed::Pins))
			{
				return false;
			}
			StoreAgain(*link);
			VisualScriptLink const spliced = link->GetLink();
			VisualScriptNodePins const pins = GetCanvas(place->Function).GetPins();
			std::string const storedInput = ToStoredPin(node, VisualScriptPinDirection::In, input);
			std::string storedOutput;
			if (output)
			{
				storedOutput = ToStoredPin(node, VisualScriptPinDirection::Out, *output);
			}
			else
			{
				std::vector<VisualScriptNodePin> const rows = pins(node);
				auto const exec =
					std::find_if(rows.begin(), rows.end(),
				                 [](VisualScriptNodePin const& row)
				                 {
									 return row.Direction == VisualScriptPinDirection::Out && row.Type == VisualScriptExecType;
								 });
				if (exec == rows.end())
				{
					return FailField("node", fmt::format("node {} has no exec output: name the output to splice with", node),
					                 Json::object({{"pins", DescribePins(rows)}}));
				}
				storedOutput = exec->Name;
			}
			if (!CheckPin(pins, {node, storedInput}, VisualScriptPinDirection::In, "input") ||
			    !CheckPin(pins, {node, storedOutput}, VisualScriptPinDirection::Out, "output"))
			{
				return false;
			}
			std::vector<VisualScriptLink> const before = FindGraph(m_Working, place->Function)->Links;
			if (Result<void> const done = Editing::SpliceNode(m_Working, place->Function, node, spliced, storedInput, storedOutput, pins);
			    !done)
			{
				return FailEdit(done.GetError());
			}
			result["replaced"] = DescribeLinksGone(before, FindGraph(m_Working, place->Function)->Links, std::span(&spliced, 1));
			RecordChange(LinkLevel);
			return true;
		}

		bool BatchExecutor::InsertReroute(Json& result)
		{
			std::optional<glm::vec2> position;
			std::optional<std::string> label;
			if (!ReadPoint("position", position) || !ReadNewLabel("as", label))
			{
				return false;
			}
			std::optional<LinkReference> const link = ResolveLink(*Find("link"), "link");
			if (!link)
			{
				return false;
			}
			Result<uint32_t> const reroute = Editing::InsertReroute(m_Working, link->From.Function, link->GetLink(), *position);
			if (!reroute)
			{
				return FailEdit(reroute.GetError());
			}
			RecordChange(PinLevel);
			RecordCreated(reroute.GetValue());
			if (label)
			{
				DefineLabel(*label, LabelKind::Element, reroute.GetValue());
			}
			result["id"] = reroute.GetValue();
			return true;
		}

		bool BatchExecutor::SetInput(Json& result)
		{
			uint32_t node = 0;
			std::string pin;
			if (!ResolveNodeField("node", node) || !Read("pin", pin))
			{
				return false;
			}
			Json const* const value = Find("value");
			if (value == nullptr || value->is_null())
			{
				if (Result<void> const set =
				        Editing::SetInput(m_Working, node, ToStoredPin(node, VisualScriptPinDirection::In, pin), Json());
				    !set)
				{
					return FailField("pin", set.GetError());
				}
			}
			else if (!StoreLiteral(node, pin, *value, "value"))
			{
				return false;
			}
			RecordChange(ValueLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::SetSetting(Json& result)
		{
			uint32_t id = 0;
			std::string name;
			std::optional<ElementPlace> const place = ResolveNodeField("node", id);
			if (!place || !Read("name", name))
			{
				return false;
			}
			Json const* const given = Find("value");
			Json value = given != nullptr ? *given : Json();
			std::string_view const route = Editing::FindVisualScriptSettingOperation(*place->Node, name);
			if (route == "SetSequenceOutputs")
			{
				if (!value.is_number_integer())
				{
					return FailField("value", "the Count of a Sequence is an integer from 2 to 64");
				}
				int64_t const count = value.get<int64_t>();
				Result<uint32_t> const removed = Editing::SetSequenceOutputs(
					m_Working, id, static_cast<int>(std::clamp<int64_t>(count, -1, 1000)), std::nullopt, std::nullopt);
				if (!removed)
				{
					return FailEdit(removed.GetError());
				}
				result["removedLinks"] = removed.GetValue();
			}
			else if (route == "SetSwitchCases")
			{
				Result<uint32_t> const removed = Editing::SetSwitchCases(m_Working, id, std::move(value));
				if (!removed)
				{
					return FailEdit(removed.GetError());
				}
				result["removedLinks"] = removed.GetValue();
			}
			else if (route == "SetEventParameters")
			{
				std::vector<VisualScriptParameter> parameters;
				if (!value.is_null() && !value.is_array())
				{
					return FailField("value", "Parameters is an array of { \"Name\", \"Type\" }");
				}
				for (size_t index = 0; value.is_array() && index < value.size(); index++)
				{
					Json const& entry = value[index];
					auto const entryName = entry.is_object() ? entry.find("Name") : Json::const_iterator();
					auto const entryType = entry.is_object() ? entry.find("Type") : Json::const_iterator();
					if (!entry.is_object() || entry.size() != 2 || entryName == entry.end() || entryType == entry.end() ||
					    !entryName->is_string() || !entryType->is_string())
					{
						return FailField(fmt::format("value[{}]", index), "expected { \"Name\", \"Type\" }, both strings");
					}
					parameters.emplace_back(entryName->get<std::string>(), entryType->get<std::string>());
				}
				Result<uint32_t> const removed = Editing::SetEventParameters(m_Working, id, std::move(parameters));
				if (!removed)
				{
					return FailEdit(removed.GetError());
				}
				result["removedLinks"] = removed.GetValue();
			}
			else if (route == "RenameCustomEvent")
			{
				if (!value.is_string())
				{
					return FailField("value", "the Name of a custom event is a string");
				}
				Result<uint32_t> const renamed = Editing::RenameCustomEvent(m_Working, id, value.get<std::string>(), m_Context.Graph);
				if (!renamed)
				{
					return FailEdit(renamed.GetError());
				}
				result["renamedSenders"] = renamed.GetValue();
			}
			else if (route == "SetArrayElements")
			{
				if (!value.is_number_integer())
				{
					return FailField("value", "the Count of a Make Array is an integer from 0 to 64");
				}
				int64_t const count = value.get<int64_t>();
				Result<uint32_t> const removed = Editing::SetArrayElements(
					m_Working, id, static_cast<int>(std::clamp<int64_t>(count, -1, 1000)), std::nullopt, std::nullopt);
				if (!removed)
				{
					return FailEdit(removed.GetError());
				}
				result["removedLinks"] = removed.GetValue();
			}
			else if (route == "SetFormula")
			{
				if (!value.is_string())
				{
					return FailField("value", "the Formula of an Expression is a string");
				}
				if (!ApplyFormula(id, value.get<std::string>(), result))
				{
					return false;
				}
			}
			else if (route == "SetTimeline")
			{
				// Null gives the timeline a setting holds without one: 1 s, no loop, no tracks.
				Json const setting = value.is_null() ? WriteVisualScriptTimeline(VisualScriptTimeline{}) : value;
				if (!ApplyTimeline(id, setting, "value", {}, result))
				{
					return false;
				}
			}
			else if (route == "RetargetNode")
			{
				// The setting names the other graph's elements, whose declarations give the pins (a Send's Parameters copied
				// from the target's event as AddNode copies them).
				Json settings = Json::object({{name, std::move(value)}});
				Json const existing = place->Node->Settings;
				if (!ConvertSettings(place->Node->Type, settings, place->Function, "value", &existing, true) ||
				    !FillEventParameters(place->Node->Type, settings, "value", &existing))
				{
					return false;
				}
				return ApplyRetarget(id, std::move(settings), result);
			}
			else if (!route.empty())
			{
				return FailField("name", fmt::format("{} of node {} ({}) changes its pins or its senders: set it with {}", name, id,
				                                     place->Node->Type, route));
			}
			else
			{
				if (!value.is_null())
				{
					Json settings = Json::object({{name, std::move(value)}});
					if (!ConvertSettings(place->Node->Type, settings, place->Function, "value", &place->Node->Settings, true))
					{
						return false;
					}
					value = std::move(settings[name]);
				}
				if (Result<void> const set = Editing::SetSetting(m_Working, id, name, std::move(value)); !set)
				{
					return FailEdit(set.GetError());
				}
			}
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SetSequenceOutputs(Json& result)
		{
			uint32_t node = 0;
			std::optional<int64_t> count;
			std::optional<int64_t> insertAt;
			std::optional<int64_t> removeAt;
			if (!ResolveNodeField("node", node) || !Read("count", count) || !Read("insertAt", insertAt) || !Read("removeAt", removeAt))
			{
				return false;
			}
			if (insertAt && removeAt)
			{
				return FailField("removeAt", "SetSequenceOutputs takes insertAt or removeAt, not both");
			}
			auto const toInt = [](std::optional<int64_t> value) -> std::optional<int>
			{
				return value ? std::optional<int>(static_cast<int>(std::clamp<int64_t>(*value, -1, 1000))) : std::nullopt;
			};
			Result<uint32_t> const removed =
				Editing::SetSequenceOutputs(m_Working, node, toInt(count).value_or(0), toInt(insertAt), toInt(removeAt));
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SetSwitchCases(Json& result)
		{
			uint32_t node = 0;
			if (!ResolveNodeField("node", node))
			{
				return false;
			}
			Result<uint32_t> const removed = Editing::SetSwitchCases(m_Working, node, *Find("cases"));
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SetEventParameters(Json& result)
		{
			uint32_t node = 0;
			std::vector<VisualScriptParameter> parameters;
			if (!ResolveNodeField("node", node) || !ReadParameters("parameters", parameters))
			{
				return false;
			}
			Result<uint32_t> const removed = Editing::SetEventParameters(m_Working, node, std::move(parameters));
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::RenameCustomEvent(Json& result)
		{
			uint32_t node = 0;
			std::string name;
			if (!ResolveNodeField("node", node) || !Read("name", name))
			{
				return false;
			}
			Result<uint32_t> const renamed = Editing::RenameCustomEvent(m_Working, node, name, m_Context.Graph);
			if (!renamed)
			{
				return FailEdit(renamed.GetError());
			}
			result["renamedSenders"] = renamed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SetArrayElements(Json& result)
		{
			uint32_t node = 0;
			std::optional<int64_t> count;
			std::optional<int64_t> insertAt;
			std::optional<int64_t> removeAt;
			if (!ResolveNodeField("node", node) || !Read("count", count) || !Read("insertAt", insertAt) || !Read("removeAt", removeAt))
			{
				return false;
			}
			if (insertAt && removeAt)
			{
				return FailField("removeAt", "SetArrayElements takes insertAt or removeAt, not both");
			}
			auto const toInt = [](std::optional<int64_t> value) -> std::optional<int>
			{
				return value ? std::optional<int>(static_cast<int>(std::clamp<int64_t>(*value, -1, 1000))) : std::nullopt;
			};
			Result<uint32_t> const removed =
				Editing::SetArrayElements(m_Working, node, toInt(count).value_or(0), toInt(insertAt), toInt(removeAt));
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SetFormula(Json& result)
		{
			uint32_t node = 0;
			std::string formula;
			if (!ResolveNodeField("node", node) || !Read("formula", formula))
			{
				return false;
			}
			if (!ApplyFormula(node, std::move(formula), result))
			{
				return false;
			}
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SetTimeline(Json& result)
		{
			uint32_t node = 0;
			if (!ResolveNodeField("node", node))
			{
				return false;
			}
			std::vector<Editing::VisualScriptTrackRename> renames;
			if (Json const* const list = Find("renames"))
			{
				if (!list->is_array())
				{
					return FailField("renames", "expected an array of { \"old\", \"new\" } track names");
				}
				for (size_t index = 0; index < list->size(); index++)
				{
					Json const& entry = (*list)[index];
					auto const from = entry.is_object() ? entry.find("old") : Json::const_iterator();
					auto const to = entry.is_object() ? entry.find("new") : Json::const_iterator();
					if (!entry.is_object() || entry.size() != 2 || from == entry.end() || to == entry.end() || !from->is_string() ||
					    !to->is_string())
					{
						return FailField(fmt::format("renames[{}]", index), "expected { \"old\", \"new\" }, both track names");
					}
					renames.push_back({from->get<std::string>(), to->get<std::string>()});
				}
			}
			if (!ApplyTimeline(node, *Find("timeline"), "timeline", renames, result))
			{
				return false;
			}
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::ApplyTimeline(uint32_t node, Json const& setting, std::string_view field,
		                                  std::span<Editing::VisualScriptTrackRename const> renames, Json& result)
		{
			Result<VisualScriptTimeline> const timeline = ParseVisualScriptTimeline(setting);
			if (!timeline)
			{
				return FailField(field, timeline.GetError());
			}
			Result<uint32_t> const removed = Editing::SetTimeline(m_Working, node, timeline.GetValue(), renames);
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			return true;
		}

		bool BatchExecutor::ApplyFormula(uint32_t node, std::string formula, Json& result)
		{
			Result<Editing::VisualScriptFormulaChange> const change = Editing::SetFormula(m_Working, node, std::move(formula));
			if (!change)
			{
				return FailEdit(change.GetError());
			}
			Json renamed = Json::array();
			for (auto const& [from, to] : change.GetValue().RenamedInputs)
			{
				renamed.push_back({{"old", from}, {"new", to}});
			}
			result["renamedInputs"] = std::move(renamed);
			result["removedLinks"] = change.GetValue().RemovedLinks;
			return true;
		}

		bool BatchExecutor::SetNodeComment(Json& result)
		{
			std::string text;
			std::vector<std::pair<uint32_t, ElementPlace>> nodes;
			if (!Read("text", text) || !ResolveElements("nodes", ElementWanted::Node, nodes))
			{
				return false;
			}
			std::vector<uint32_t> ids;
			for (auto const& [id, place] : nodes)
			{
				ids.push_back(id);
			}
			if (Result<void> const set = Editing::SetNodeComment(m_Working, ids, text); !set)
			{
				return FailEdit(set.GetError());
			}
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::SetCollapsed(Json& result)
		{
			std::optional<bool> collapsed;
			std::vector<std::pair<uint32_t, ElementPlace>> nodes;
			if (!Read("collapsed", collapsed) || !ResolveElements("nodes", ElementWanted::Node, nodes))
			{
				return false;
			}
			std::vector<uint32_t> ids;
			for (auto const& [id, place] : nodes)
			{
				ids.push_back(id);
			}
			if (Result<void> const set = Editing::SetNodeCollapsed(m_Working, ids, collapsed.value_or(false)); !set)
			{
				return FailEdit(set.GetError());
			}
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::SetEnabled(Json& result)
		{
			std::string enabled;
			std::vector<std::pair<uint32_t, ElementPlace>> nodes;
			if (!Read("enabled", enabled) || !ResolveElements("nodes", ElementWanted::Node, nodes))
			{
				return false;
			}
			std::optional<VisualScriptNodeEnabled> const state = VisualScriptNodeEnabledFromString(enabled);
			if (!state)
			{
				return FailField("enabled", "expected Enabled, Disabled or DevelopmentOnly");
			}
			std::vector<uint32_t> ids;
			for (auto const& [id, place] : nodes)
			{
				ids.push_back(id);
			}
			if (Result<void> const set = Editing::SetNodeEnabled(m_Working, ids, *state); !set)
			{
				return FailEdit(set.GetError());
			}
			RecordChange(LinkLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::ApplySplit(bool split, Json& result)
		{
			uint32_t node = 0;
			std::string pin;
			VisualScriptPinDirection direction = VisualScriptPinDirection::In;
			if (!ResolveNodeField("node", node) || !Read("pin", pin) || !ReadDirectionField("direction", direction))
			{
				return false;
			}
			if (!Require(BatchNeed::Types))
			{
				return false;
			}
			std::optional<ElementPlace> const place = FindElement(m_Working, node);
			std::string path = ToStoredPin(node, direction, pin);
			if (!split)
			{
				// A field pin of a split pin ("Force.X") recombines the split pin it is a field of, the innermost one, as the
				// canvas's Recombine does.
				std::vector<std::string> const& entries =
					direction == VisualScriptPinDirection::In ? place->Node->Split.Inputs : place->Node->Split.Outputs;
				if (std::find(entries.begin(), entries.end(), path) == entries.end())
				{
					std::optional<std::string> found;
					for (std::string const& entry : entries)
					{
						bool const under = path.size() > entry.size() && path.starts_with(entry) && path[entry.size()] == '.';
						if (under && (!found || entry.size() > found->size()))
						{
							found = entry;
						}
					}
					if (found)
					{
						path = *found;
					}
				}
			}
			VisualScriptNodePins const pins = GetCanvas(place->Function).GetPins();
			Result<uint32_t> const removed = split ? Editing::SplitPin(m_Working, node, direction, path, pins, *m_Library)
			                                       : Editing::RecombinePin(m_Working, node, direction, path, pins, *m_Library);
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SplitPin(Json& result)
		{
			return ApplySplit(true, result);
		}

		bool BatchExecutor::RecombinePin(Json& result)
		{
			return ApplySplit(false, result);
		}

		// --- Comment boxes and the document ---

		bool BatchExecutor::AddComment(Json& result)
		{
			std::string text;
			std::optional<glm::vec2> position;
			std::optional<glm::vec2> size;
			std::optional<glm::vec4> color;
			std::optional<std::string> label;
			if (!Read("text", text) || !ReadPoint("position", position) || !ReadPoint("size", size) || !ReadColor("color", color) ||
			    !ReadNewLabel("as", label))
			{
				return false;
			}
			std::optional<uint32_t> function = ResolveFunction("function", m_Context.Function);
			if (!function)
			{
				return false;
			}
			std::vector<uint32_t> elements;
			if (Has("elements"))
			{
				uint32_t owner = *function;
				if (!ResolveElementsOfOneGraph("elements", ElementWanted::Either, elements, owner))
				{
					return false;
				}
				if (!elements.empty() && owner != *function)
				{
					return FailField("elements",
					                 fmt::format("the elements are not of {}",
					                             *function == 0 ? std::string("the event graph") : fmt::format("function {}", *function)));
				}
				if (!RequirePlaced(*function, elements, "elements"))
				{
					return false;
				}
			}
			// Framing measures the elements as a window does; a box placed alone needs no analysis.
			std::optional<VisualScriptNodeExtents> const extents =
				elements.empty() ? std::optional<VisualScriptNodeExtents>(GetCanvas(*function).GetExtents())
								 : GetExtents(*function, VisualScriptExtentsMode::Measured);
			if (!extents)
			{
				return false;
			}
			if (!position)
			{
				position = GetCornerBelow(GetPlacedNodes(*function, *extents, {}));
			}
			Result<uint32_t> const added =
				Editing::AddComment(m_Working, *function, text, elements, *extents, *position, color.value_or(VisualScriptComment().Color));
			if (!added)
			{
				return FailEdit(added.GetError());
			}
			uint32_t const id = added.GetValue();
			if (size)
			{
				VisualScriptGraph const* const graph = FindGraph(m_Working, *function);
				auto const comment = std::find_if(graph->Comments.begin(), graph->Comments.end(),
				                                  [id](VisualScriptComment const& candidate)
				                                  {
													  return candidate.ID == id;
												  });
				if (Result<void> const set = Editing::SetCommentGeometry(m_Working, id, comment->Position, *size); !set)
				{
					return FailField("size", set.GetError());
				}
			}
			RecordChange(LayoutLevel);
			RecordCreated(id);
			if (label)
			{
				DefineLabel(*label, LabelKind::Element, id);
			}
			result["id"] = id;
			return true;
		}

		bool BatchExecutor::SetComment(Json& result)
		{
			std::optional<std::string> text;
			std::optional<glm::vec2> position;
			std::optional<glm::vec2> size;
			std::optional<glm::vec4> color;
			uint32_t id = 0;
			Json const* const field = Find("comment");
			if (!Read("text", text) || !ReadPoint("position", position) || !ReadPoint("size", size) || !ReadColor("color", color) ||
			    !ResolveElement(*field, "comment", ElementWanted::Comment, &id))
			{
				return false;
			}
			if (text)
			{
				if (Result<void> const set = Editing::SetCommentText(m_Working, id, *text); !set)
				{
					return FailField("text", set.GetError());
				}
			}
			if (position || size)
			{
				std::optional<ElementPlace> const place = FindElement(m_Working, id);
				VisualScriptGraph const* const graph = FindGraph(m_Working, place->Function);
				auto const comment = std::find_if(graph->Comments.begin(), graph->Comments.end(),
				                                  [id](VisualScriptComment const& candidate)
				                                  {
													  return candidate.ID == id;
												  });
				if (Result<void> const set =
				        Editing::SetCommentGeometry(m_Working, id, position.value_or(comment->Position), size.value_or(comment->Size));
				    !set)
				{
					return FailField(size ? "size" : "position", set.GetError());
				}
			}
			if (color)
			{
				if (Result<void> const set = Editing::SetCommentColor(m_Working, id, *color); !set)
				{
					return FailField("color", set.GetError());
				}
			}
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::SetDescription(Json& result)
		{
			std::string description;
			if (!Read("description", description))
			{
				return false;
			}
			if (Result<void> const set = Editing::SetDescription(m_Working, std::move(description)); !set)
			{
				return FailEdit(set.GetError());
			}
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::SetKind(Json& result)
		{
			std::string kind;
			if (!Read("kind", kind))
			{
				return false;
			}
			std::optional<VisualScriptDocumentKind> const parsed = VisualScriptDocumentKindFromString(kind);
			if (!parsed)
			{
				return FailField("kind", fmt::format("{} is no kind of visual script: Script or Library", kind));
			}
			if (Result<void> const set = Editing::SetDocumentKind(m_Working, *parsed); !set)
			{
				return FailEdit(set.GetError());
			}
			// What the analysis finds of every element changes with the kind.
			RecordChange(PinLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::Retarget(Json& result)
		{
			uint32_t id = 0;
			std::optional<ElementPlace> const place = ResolveNodeField("node", id);
			if (!place)
			{
				return false;
			}
			Json const* const given = Find("settings");
			if (given == nullptr || !given->is_object())
			{
				return FailField("settings",
				                 "expected an object of settings: Graph, Function, Variable, Name or Parameters (null removes one)");
			}
			Json settings = *given;
			Json const existing = place->Node->Settings;
			if (!ConvertSettings(place->Node->Type, settings, place->Function, "settings", &existing) ||
			    !FillEventParameters(place->Node->Type, settings, "settings", &existing))
			{
				return false;
			}
			return ApplyRetarget(id, std::move(settings), result);
		}

		// --- Variables and locals ---

		// The range field: [min, max], or null for none.
		bool ReadRangeValue(Json const& value, std::optional<glm::vec2>& range)
		{
			if (value.is_null())
			{
				range.reset();
				return true;
			}
			if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
			{
				return false;
			}
			double const minimum = value[0].get<double>();
			double const maximum = value[1].get<double>();
			if (!std::isfinite(minimum) || !std::isfinite(maximum) || std::abs(minimum) > Format::MaxFloatMagnitude ||
			    std::abs(maximum) > Format::MaxFloatMagnitude)
			{
				return false;
			}
			range = glm::vec2(static_cast<float>(minimum), static_cast<float>(maximum));
			return true;
		}

		bool BatchExecutor::AddVariable(Json& result)
		{
			std::string name;
			std::string type;
			std::optional<bool> isPublic;
			std::optional<std::string> tooltip;
			std::optional<std::string> label;
			if (!Read("name", name) || !Read("type", type) || !Read("public", isPublic) || !Read("tooltip", tooltip) ||
			    !ReadNewLabel("as", label))
			{
				return false;
			}
			Result<uint32_t> const added = Editing::AddVariable(m_Working, name, type);
			if (!added)
			{
				return FailEdit(added.GetError());
			}
			uint32_t const id = added.GetValue();
			if (Has("value") || isPublic || tooltip || Has("range"))
			{
				auto const found = std::find_if(m_Working.Variables.begin(), m_Working.Variables.end(),
				                                [id](VisualScriptVariable const& variable)
				                                {
													return variable.ID == id;
												});
				VisualScriptVariable variable = *found;
				if (Json const* const value = Find("value"))
				{
					variable.Value = *value;
					if (!ConvertDeclaredValue(variable.Value, type, "value"))
					{
						return false;
					}
				}
				variable.Public = isPublic.value_or(false);
				variable.Tooltip = tooltip.value_or(std::string());
				if (Json const* const range = Find("range"); range != nullptr && !ReadRangeValue(*range, variable.Range))
				{
					return FailField("range", "expected [min, max] (finite floats) or null");
				}
				if (Result<bool> const set = Editing::SetVariable(m_Working, id, std::move(variable), GetEnumLookup()); !set)
				{
					return FailEdit(set.GetError());
				}
			}
			RecordChange(LinkLevel);
			RecordCreated(id);
			if (label)
			{
				DefineLabel(*label, LabelKind::Variable, id);
			}
			result["id"] = id;
			return true;
		}

		bool BatchExecutor::SetVariable(Json& result)
		{
			std::optional<uint32_t> const id = ResolveVariable("variable");
			if (!id)
			{
				return false;
			}
			auto const found = std::find_if(m_Working.Variables.begin(), m_Working.Variables.end(),
			                                [&id](VisualScriptVariable const& variable)
			                                {
												return variable.ID == *id;
											});
			VisualScriptVariable variable = *found;
			std::optional<std::string> name;
			std::optional<std::string> type;
			std::optional<bool> isPublic;
			std::optional<std::string> tooltip;
			if (!Read("name", name) || !Read("type", type) || !Read("public", isPublic) || !Read("tooltip", tooltip))
			{
				return false;
			}
			uint8_t level = LayoutLevel;
			if (name)
			{
				level = std::max(level, *name != variable.Name ? TitleLevel : ValueLevel);
				variable.Name = *name;
			}
			if (type)
			{
				level = std::max(level, *type != variable.Type ? LinkLevel : ValueLevel);
				variable.Type = *type;
			}
			if (isPublic)
			{
				variable.Public = *isPublic;
				level = std::max(level, ValueLevel);
			}
			if (tooltip)
			{
				variable.Tooltip = *tooltip;
			}
			if (Json const* const range = Find("range"); range != nullptr && !ReadRangeValue(*range, variable.Range))
			{
				return FailField("range", "expected [min, max] (finite floats) or null");
			}
			if (Json const* const value = Find("value"))
			{
				variable.Value = *value;
				if (!ConvertDeclaredValue(variable.Value, variable.Type, "value"))
				{
					return false;
				}
				level = std::max(level, ValueLevel);
			}
			if (Json const* const formerNames = Find("formerNames"))
			{
				if (!formerNames->is_array() || !std::all_of(formerNames->begin(), formerNames->end(),
				                                             [](Json const& entry)
				                                             {
																 return entry.is_string();
															 }))
				{
					return FailField("formerNames", "expected an array of names");
				}
				variable.FormerNames = formerNames->get<std::vector<std::string>>();
				level = std::max(level, ValueLevel);
			}
			Result<bool> const set = Editing::SetVariable(m_Working, *id, std::move(variable), GetEnumLookup());
			if (!set)
			{
				return FailEdit(set.GetError());
			}
			result["valueCleared"] = set.GetValue();
			RecordChange(level);
			return true;
		}

		bool BatchExecutor::DeleteVariable(Json& result)
		{
			std::optional<bool> removeNodes;
			if (!Read("removeNodes", removeNodes))
			{
				return false;
			}
			std::optional<uint32_t> const id = ResolveVariable("variable");
			if (!id)
			{
				return false;
			}
			bool const removing = removeNodes.value_or(true);
			if (removing && !Require(BatchNeed::Pins, true))
			{
				return false;
			}
			std::optional<VisualScriptNodePins> const pins =
				removing && m_Library != nullptr ? std::optional<VisualScriptNodePins>(GetDocumentPins()) : std::nullopt;
			Result<uint32_t> const removed = Editing::RemoveVariable(m_Working, *id, removing, pins ? &*pins : nullptr);
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedNodes"] = removed.GetValue();
			RecordChange(PinLevel);
			RecordRemovals();
			return true;
		}

		bool BatchExecutor::ReorderVariables(Json& result)
		{
			Json const* const list = Find("variables");
			if (!list->is_array())
			{
				return FailField("variables", "expected an array of variables");
			}
			std::vector<uint32_t> order;
			for (size_t index = 0; index < list->size(); index++)
			{
				std::optional<uint32_t> const id = ResolveVariableValue((*list)[index], fmt::format("variables[{}]", index));
				if (!id)
				{
					return false;
				}
				order.push_back(*id);
			}
			if (Result<void> const reordered = Editing::ReorderVariables(m_Working, order); !reordered)
			{
				return FailEdit(reordered.GetError());
			}
			RecordChange(ValueLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::RenameVariableTo(uint32_t id, std::string const& name)
		{
			auto const found = std::find_if(m_Working.Variables.begin(), m_Working.Variables.end(),
			                                [id](VisualScriptVariable const& variable)
			                                {
												return variable.ID == id;
											});
			VisualScriptVariable variable = *found;
			variable.Name = name;
			if (Result<bool> const set = Editing::SetVariable(m_Working, id, std::move(variable), GetEnumLookup()); !set)
			{
				return FailField("name", set.GetError());
			}
			return true;
		}

		bool BatchExecutor::DuplicateVariable(Json& result)
		{
			std::optional<std::string> name;
			if (!Read("name", name))
			{
				return false;
			}
			std::optional<uint32_t> const id = ResolveVariable("variable");
			if (!id)
			{
				return false;
			}
			Result<uint32_t> const copy = Editing::DuplicateVariable(m_Working, *id);
			if (!copy)
			{
				return FailEdit(copy.GetError());
			}
			if (name && !RenameVariableTo(copy.GetValue(), *name))
			{
				return false;
			}
			RecordChange(ValueLevel);
			RecordCreated(copy.GetValue());
			result["id"] = copy.GetValue();
			return true;
		}

		bool BatchExecutor::AddLocal(Json& result)
		{
			std::string name;
			std::string type;
			std::optional<std::string> label;
			if (!Read("name", name) || !Read("type", type) || !ReadNewLabel("as", label))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunction("function", std::nullopt);
			if (!function)
			{
				return false;
			}
			Result<uint32_t> const added = Editing::AddLocal(m_Working, *function, name, type);
			if (!added)
			{
				return FailEdit(added.GetError());
			}
			uint32_t const id = added.GetValue();
			if (Json const* const value = Find("value"))
			{
				VisualScriptFunction const* const owner = FindFunction(m_Working, *function);
				auto const found = std::find_if(owner->Locals.begin(), owner->Locals.end(),
				                                [id](VisualScriptLocal const& local)
				                                {
													return local.ID == id;
												});
				VisualScriptLocal local = *found;
				local.Value = *value;
				if (!ConvertDeclaredValue(local.Value, type, "value"))
				{
					return false;
				}
				if (Result<bool> const set = Editing::SetLocal(m_Working, *function, id, std::move(local), GetEnumLookup()); !set)
				{
					return FailField("value", set.GetError());
				}
			}
			RecordChange(LinkLevel);
			RecordCreated(id);
			if (label)
			{
				DefineLabel(*label, LabelKind::Local, id);
			}
			result["id"] = id;
			return true;
		}

		bool BatchExecutor::SetLocal(Json& result)
		{
			std::optional<uint32_t> const function = ResolveFunction("function", std::nullopt);
			if (!function)
			{
				return false;
			}
			std::optional<uint32_t> const id = ResolveLocal("local", *function);
			if (!id)
			{
				return false;
			}
			VisualScriptFunction const* const owner = FindFunction(m_Working, *function);
			auto const found = std::find_if(owner->Locals.begin(), owner->Locals.end(),
			                                [&id](VisualScriptLocal const& local)
			                                {
												return local.ID == *id;
											});
			VisualScriptLocal local = *found;
			std::optional<std::string> name;
			std::optional<std::string> type;
			if (!Read("name", name) || !Read("type", type))
			{
				return false;
			}
			uint8_t level = ValueLevel;
			if (name)
			{
				level = std::max(level, *name != local.Name ? TitleLevel : ValueLevel);
				local.Name = *name;
			}
			if (type)
			{
				level = std::max(level, *type != local.Type ? LinkLevel : ValueLevel);
				local.Type = *type;
			}
			if (Json const* const value = Find("value"))
			{
				local.Value = *value;
				if (!ConvertDeclaredValue(local.Value, local.Type, "value"))
				{
					return false;
				}
			}
			Result<bool> const set = Editing::SetLocal(m_Working, *function, *id, std::move(local), GetEnumLookup());
			if (!set)
			{
				return FailEdit(set.GetError());
			}
			result["valueCleared"] = set.GetValue();
			RecordChange(level);
			return true;
		}

		bool BatchExecutor::DeleteLocal(Json& result)
		{
			std::optional<bool> removeNodes;
			if (!Read("removeNodes", removeNodes))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunction("function", std::nullopt);
			if (!function)
			{
				return false;
			}
			std::optional<uint32_t> const id = ResolveLocal("local", *function);
			if (!id)
			{
				return false;
			}
			bool const removing = removeNodes.value_or(true);
			if (removing && !Require(BatchNeed::Pins, true))
			{
				return false;
			}
			std::optional<VisualScriptNodePins> const pins =
				removing && m_Library != nullptr ? std::optional<VisualScriptNodePins>(GetCanvas(*function).GetPins()) : std::nullopt;
			Result<uint32_t> const removed = Editing::RemoveLocal(m_Working, *function, *id, removing, pins ? &*pins : nullptr);
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedNodes"] = removed.GetValue();
			RecordChange(PinLevel);
			RecordRemovals();
			return true;
		}

		// --- Functions ---

		bool BatchExecutor::AddFunction(Json& result)
		{
			std::string name;
			std::optional<bool> pure;
			std::vector<VisualScriptParameter> inputs;
			std::vector<VisualScriptParameter> outputs;
			std::optional<std::string> label;
			std::optional<std::string> entryLabel;
			std::optional<std::string> returnLabel;
			if (!Read("name", name) || !Read("pure", pure) || !ReadParameters("inputs", inputs) || !ReadParameters("outputs", outputs) ||
			    !ReadNewLabel("as", label) || !ReadNewLabel("entryAs", entryLabel) || !ReadNewLabel("returnAs", returnLabel))
			{
				return false;
			}
			if ((label && (label == entryLabel || label == returnLabel)) || (entryLabel && entryLabel == returnLabel))
			{
				return Fail({}, "as, entryAs and returnAs must be different labels");
			}
			Result<uint32_t> const added = Editing::AddFunction(m_Working, name);
			if (!added)
			{
				return FailEdit(added.GetError());
			}
			uint32_t const id = added.GetValue();
			if (pure && *pure)
			{
				if (Result<uint32_t> const set = Editing::SetFunctionPure(m_Working, id, true); !set)
				{
					return FailField("pure", set.GetError());
				}
			}
			if (!inputs.empty() || !outputs.empty())
			{
				if (Result<uint32_t> const set = Editing::SetFunctionSignature(m_Working, id, std::move(inputs), std::move(outputs)); !set)
				{
					return FailField(Has("inputs") ? "inputs" : "outputs", set.GetError());
				}
			}
			RecordChange(PinLevel);
			RecordCreated(id);
			RecordCreated(id + 1);
			RecordCreated(id + 2);
			if (label)
			{
				DefineLabel(*label, LabelKind::Function, id);
			}
			if (entryLabel)
			{
				DefineLabel(*entryLabel, LabelKind::Element, id + 1);
			}
			if (returnLabel)
			{
				DefineLabel(*returnLabel, LabelKind::Element, id + 2);
			}
			result["id"] = id;
			result["entry"] = id + 1;
			result["return"] = id + 2;
			return true;
		}

		bool BatchExecutor::RenameFunction(Json& result)
		{
			std::string name;
			if (!Read("name", name))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunctionValue(*Find("function"), "function", false);
			if (!function)
			{
				return false;
			}
			if (Result<void> const renamed = Editing::RenameFunction(m_Working, *function, std::move(name)); !renamed)
			{
				return FailEdit(renamed.GetError());
			}
			RecordChange(TitleLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::SetFunctionSignature(Json& result)
		{
			std::vector<VisualScriptParameter> inputs;
			std::vector<VisualScriptParameter> outputs;
			if (!ReadParameters("inputs", inputs) || !ReadParameters("outputs", outputs))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunctionValue(*Find("function"), "function", false);
			if (!function)
			{
				return false;
			}
			Result<uint32_t> const removed = Editing::SetFunctionSignature(m_Working, *function, std::move(inputs), std::move(outputs));
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::SetFunctionPure(Json& result)
		{
			std::optional<bool> pure;
			if (!Read("pure", pure))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunctionValue(*Find("function"), "function", false);
			if (!function)
			{
				return false;
			}
			Result<uint32_t> const removed = Editing::SetFunctionPure(m_Working, *function, pure.value_or(false));
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedLinks"] = removed.GetValue();
			RecordChange(PinLevel);
			return true;
		}

		bool BatchExecutor::DeleteFunction(Json& result)
		{
			std::optional<bool> removeCalls;
			if (!Read("removeCalls", removeCalls))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunctionValue(*Find("function"), "function", false);
			if (!function)
			{
				return false;
			}
			bool const removing = removeCalls.value_or(true);
			if (removing && !Require(BatchNeed::Pins, true))
			{
				return false;
			}
			std::optional<VisualScriptNodePins> const pins =
				removing && m_Library != nullptr ? std::optional<VisualScriptNodePins>(GetDocumentPins()) : std::nullopt;
			Result<uint32_t> const removed = Editing::RemoveFunction(m_Working, *function, removing, pins ? &*pins : nullptr);
			if (!removed)
			{
				return FailEdit(removed.GetError());
			}
			result["removedCalls"] = removed.GetValue();
			RecordChange(PinLevel);
			RecordRemovals();
			return true;
		}

		bool BatchExecutor::DuplicateFunction(Json& result)
		{
			std::optional<std::string> name;
			if (!Read("name", name))
			{
				return false;
			}
			std::optional<uint32_t> const function = ResolveFunctionValue(*Find("function"), "function", false);
			if (!function)
			{
				return false;
			}
			uint32_t const largest = Editing::GetLargestVisualScriptID(m_Working);
			Result<uint32_t> const copy = Editing::DuplicateFunction(m_Working, *function);
			if (!copy)
			{
				return FailEdit(copy.GetError());
			}
			if (name)
			{
				if (Result<void> const renamed = Editing::RenameFunction(m_Working, copy.GetValue(), *name); !renamed)
				{
					return FailField("name", renamed.GetError());
				}
			}
			RecordChange(PinLevel);
			RecordCreatedAbove(largest);
			result["id"] = copy.GetValue();
			return true;
		}

		// --- Promotions ---

		bool BatchExecutor::PromoteToVariable(Json& result)
		{
			uint32_t node = 0;
			std::string pin;
			VisualScriptPinDirection direction = VisualScriptPinDirection::In;
			std::optional<std::string> name;
			std::optional<std::string> label;
			std::optional<ElementPlace> const place = ResolveNodeField("node", node);
			if (!place || !Read("pin", pin) || !ReadDirectionField("direction", direction) || !Read("name", name) ||
			    !ReadNewLabel("as", label) || !RequirePlaced(place->Function, std::span<uint32_t const>(&node, 1), "node"))
			{
				return false;
			}
			if (!Require(BatchNeed::Types))
			{
				return false;
			}
			VisualScriptNodePins const pins = GetCanvas(place->Function).GetPins();
			VisualScriptPinReference const reference{node, ToStoredPin(node, direction, pin)};
			if (!CheckPin(pins, reference, direction, "pin"))
			{
				return false;
			}
			Result<uint32_t> const variable = Editing::PromoteToVariable(m_Working, place->Function, reference, direction, pins);
			if (!variable)
			{
				return FailEdit(variable.GetError());
			}
			uint32_t const id = variable.GetValue();
			if (name && !RenameVariableTo(id, *name))
			{
				return false;
			}
			RecordChange(PinLevel);
			RecordCreated(id);
			RecordCreated(id + 1);
			if (label)
			{
				DefineLabel(*label, LabelKind::Element, id + 1);
			}
			result["variable"] = id;
			result["node"] = id + 1;
			return true;
		}

		bool BatchExecutor::PromoteToFunctionInput(Json& result)
		{
			uint32_t node = 0;
			std::string pin;
			std::optional<std::string> name;
			std::optional<ElementPlace> const place = ResolveNodeField("node", node);
			if (!place || !Read("pin", pin) || !Read("name", name))
			{
				return false;
			}
			if (place->Function == 0)
			{
				return FailField("node",
				                 fmt::format("node {} is in the event graph: function inputs are added in a function's graph", node));
			}
			if (!Require(BatchNeed::Types))
			{
				return false;
			}
			VisualScriptNodePins const pins = GetCanvas(place->Function).GetPins();
			VisualScriptPinReference const reference{node, ToStoredPin(node, VisualScriptPinDirection::In, pin)};
			if (!CheckPin(pins, reference, VisualScriptPinDirection::In, "pin"))
			{
				return false;
			}
			if (Result<void> const promoted = Editing::PromoteToFunctionInput(m_Working, place->Function, reference, pins); !promoted)
			{
				return FailEdit(promoted.GetError());
			}
			if (name)
			{
				// The new input is the function's last; renaming it through the signature renames its links and the calls' pins.
				VisualScriptFunction const* const function = FindFunction(m_Working, place->Function);
				std::vector<VisualScriptParameter> inputs = function->Inputs;
				std::vector<VisualScriptParameter> outputs = function->Outputs;
				if (inputs.back().Name != *name)
				{
					inputs.back().Name = *name;
					if (Result<uint32_t> const renamed =
					        Editing::SetFunctionSignature(m_Working, place->Function, std::move(inputs), std::move(outputs));
					    !renamed)
					{
						return FailField("name", renamed.GetError());
					}
				}
			}
			RecordChange(PinLevel);
			static_cast<void>(result);
			return true;
		}

		// --- Layout ---

		bool BatchExecutor::Arrange(Json& result)
		{
			std::string scope = "Graph";
			VisualScriptExtentsMode mode = VisualScriptExtentsMode::Measured;
			if (!Read("scope", scope) || !ReadExtentsMode(mode))
			{
				return false;
			}
			if (scope != "Graph" && scope != "Created" && scope != "Nodes")
			{
				return FailField("scope", "expected Graph, Created or Nodes");
			}
			if ((scope == "Nodes") != Has("nodes"))
			{
				return FailField(scope == "Nodes" ? "scope" : "nodes", "Arrange takes nodes with the scope Nodes, and only then");
			}
			std::optional<uint32_t> function = ResolveFunction("function", m_Context.Function);
			if (!function)
			{
				return false;
			}
			std::vector<uint32_t> nodes;
			if (scope == "Nodes")
			{
				uint32_t owner = *function;
				if (!ResolveElementsOfOneGraph("nodes", ElementWanted::Node, nodes, owner))
				{
					return false;
				}
				if (!nodes.empty() && owner != *function)
				{
					if (Has("function"))
					{
						return FailField("nodes", "the nodes are not of the graph function names");
					}
					function = owner;
				}
			}
			std::optional<VisualScriptNodeExtents> const extents = GetExtents(*function, mode);
			if (!extents)
			{
				return false;
			}
			size_t arranged = 0;
			if (scope == "Created")
			{
				if (!ArrangeCreated(*function, *extents, arranged))
				{
					return false;
				}
			}
			else
			{
				if (scope == "Graph")
				{
					for (VisualScriptNode const& node : FindGraph(m_Working, *function)->Nodes)
					{
						nodes.push_back(node.ID);
					}
				}
				if (!nodes.empty())
				{
					if (Result<void> const done = Editing::ArrangeNodes(m_Working, *function, nodes, *extents); !done)
					{
						return FailEdit(done.GetError());
					}
				}
				arranged = nodes.size();
			}
			for (uint32_t const id : nodes)
			{
				m_Unplaced.erase(id);
			}
			RecordChange(LayoutLevel);
			result["arranged"] = arranged;
			return true;
		}

		bool BatchExecutor::Align(Json& result)
		{
			std::string alignment;
			VisualScriptExtentsMode mode = VisualScriptExtentsMode::Measured;
			if (!Read("alignment", alignment) || !ReadExtentsMode(mode))
			{
				return false;
			}
			constexpr std::array<std::pair<std::string_view, VisualScriptAlignment>, 6> Alignments = {{
				{"Left", VisualScriptAlignment::Left},
				{"Right", VisualScriptAlignment::Right},
				{"Top", VisualScriptAlignment::Top},
				{"Bottom", VisualScriptAlignment::Bottom},
				{"Centers", VisualScriptAlignment::Centers},
				{"Middles", VisualScriptAlignment::Middles},
			}};
			auto const found = std::find_if(Alignments.begin(), Alignments.end(),
			                                [&alignment](auto const& entry)
			                                {
												return entry.first == alignment;
											});
			if (found == Alignments.end())
			{
				return FailField("alignment", "expected Left, Right, Top, Bottom, Centers or Middles");
			}
			std::vector<uint32_t> elements;
			uint32_t function = 0;
			if (!ResolveElementsOfOneGraph("elements", ElementWanted::Either, elements, function) ||
			    !RequirePlaced(function, elements, "elements"))
			{
				return false;
			}
			std::optional<VisualScriptNodeExtents> const extents = GetExtents(function, mode);
			if (!extents)
			{
				return false;
			}
			if (Result<void> const aligned = Editing::AlignElements(m_Working, function, elements, found->second, *extents); !aligned)
			{
				return FailEdit(aligned.GetError());
			}
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::Distribute(Json& result)
		{
			std::string axis;
			VisualScriptExtentsMode mode = VisualScriptExtentsMode::Measured;
			if (!Read("axis", axis) || !ReadExtentsMode(mode))
			{
				return false;
			}
			if (axis != "Horizontal" && axis != "Vertical")
			{
				return FailField("axis", "expected Horizontal or Vertical");
			}
			std::vector<uint32_t> elements;
			uint32_t function = 0;
			if (!ResolveElementsOfOneGraph("elements", ElementWanted::Either, elements, function) ||
			    !RequirePlaced(function, elements, "elements"))
			{
				return false;
			}
			std::optional<VisualScriptNodeExtents> const extents = GetExtents(function, mode);
			if (!extents)
			{
				return false;
			}
			VisualScriptDistribution const distribution =
				axis == "Horizontal" ? VisualScriptDistribution::Horizontal : VisualScriptDistribution::Vertical;
			if (Result<void> const distributed = Editing::DistributeElements(m_Working, function, elements, distribution, *extents);
			    !distributed)
			{
				return FailEdit(distributed.GetError());
			}
			RecordChange(LayoutLevel);
			static_cast<void>(result);
			return true;
		}

		bool BatchExecutor::Straighten(Json& result)
		{
			std::vector<uint32_t> nodes;
			uint32_t function = 0;
			if (!ResolveElementsOfOneGraph("nodes", ElementWanted::Node, nodes, function) || !RequirePlaced(function, nodes, "nodes"))
			{
				return false;
			}
			if (!Require(BatchNeed::Types, true))
			{
				return false;
			}
			VisualScriptNodePins const pins = GetCanvas(function).GetPins();
			Result<uint32_t> const moved = Editing::StraightenExec(m_Working, function, nodes, pins);
			if (!moved)
			{
				return FailEdit(moved.GetError());
			}
			result["moved"] = moved.GetValue();
			RecordChange(LayoutLevel);
			return true;
		}

		// --- Quick fixes ---

		// The operation that makes the edit a navigating fix only shows.
		std::string_view GetNavigatingFixOperation(VisualScriptQuickFix const& fix, VisualScriptDocument const& document)
		{
			switch (fix.Kind)
			{
				case VisualScriptFixKind::EditSetting:
					return "SetSetting";
				case VisualScriptFixKind::Rename:
				{
					if (std::any_of(document.Variables.begin(), document.Variables.end(),
					                [&fix](VisualScriptVariable const& variable)
					                {
										return variable.ID == fix.Target;
									}))
					{
						return "SetVariable";
					}
					if (FindFunction(document, fix.Target) != nullptr)
					{
						return "RenameFunction";
					}
					std::optional<ElementPlace> const place = FindElement(document, fix.Target);
					if (place && place->IsNode && place->Node->Type == "Event.Custom")
					{
						return "RenameCustomEvent";
					}
					return "SetLocal";
				}
				case VisualScriptFixKind::GoTo:
					return "Delete";
				case VisualScriptFixKind::OpenGraph:
					return "a graph.edit of the graph it names";
				case VisualScriptFixKind::Retarget:
				case VisualScriptFixKind::UpdateParameters:
				case VisualScriptFixKind::DeleteVariable:
				case VisualScriptFixKind::ChangeNodeType:
				case VisualScriptFixKind::UsePin:
				case VisualScriptFixKind::RemoveInput:
				case VisualScriptFixKind::RemoveSetting:
				case VisualScriptFixKind::KeepFirstLink:
				case VisualScriptFixKind::BreakLink:
				case VisualScriptFixKind::InsertConvert:
				case VisualScriptFixKind::ResetValue:
				case VisualScriptFixKind::CreatePublicVariable:
				case VisualScriptFixKind::UseIdentity:
				case VisualScriptFixKind::UseElement:
				case VisualScriptFixKind::UseType:
				case VisualScriptFixKind::MakePrivate:
				case VisualScriptFixKind::DeleteNode:
				case VisualScriptFixKind::AddEntry:
				case VisualScriptFixKind::AddReturn:
				case VisualScriptFixKind::MakeImpure:
				case VisualScriptFixKind::UpdateNode:
				case VisualScriptFixKind::UseSetting:
				case VisualScriptFixKind::RemoveSplit:
				case VisualScriptFixKind::EnableNode:
				case VisualScriptFixKind::ReplaceInFormula:
					return {};
			}
			return {};
		}

		Json DescribeDiagnosticPlace(VisualScriptDiagnostic const& diagnostic)
		{
			Json place = Json::object({{"code", diagnostic.Code}, {"function", diagnostic.Function}, {"message", diagnostic.Message}});
			if (diagnostic.Node)
			{
				place["node"] = *diagnostic.Node;
			}
			if (!diagnostic.Pin.empty())
			{
				place["pin"] = diagnostic.Pin;
			}
			if (diagnostic.Direction)
			{
				place["direction"] = GetDirectionName(*diagnostic.Direction);
			}
			if (diagnostic.Link)
			{
				place["link"] = DescribeLink(*diagnostic.Link);
			}
			if (diagnostic.Variable)
			{
				place["variable"] = *diagnostic.Variable;
			}
			return place;
		}

		bool BatchExecutor::Fix(Json& result)
		{
			std::string code;
			std::string label;
			std::optional<std::string> pin;
			std::optional<std::string> directionText;
			if (!Read("code", code) || !Read("fix", label) || !Read("pin", pin) || !Read("direction", directionText))
			{
				return false;
			}
			std::optional<VisualScriptPinDirection> direction;
			if (directionText && !(direction = ReadDirection(*directionText)))
			{
				return FailField("direction", "expected In or Out");
			}
			std::optional<uint32_t> node;
			if (Json const* const field = Find("node"))
			{
				uint32_t id = 0;
				if (!ResolveElement(*field, "node", ElementWanted::Node, &id))
				{
					return false;
				}
				node = id;
			}
			std::optional<uint32_t> function;
			if (Json const* const field = Find("function"))
			{
				if (!(function = ResolveFunctionValue(*field, "function", true)))
				{
					return false;
				}
			}
			std::optional<uint32_t> variable;
			if (Json const* const field = Find("variable"))
			{
				// A variable or a local: an ID, or a name (locals of the function given first).
				if (field->is_number())
				{
					if (!(variable = ReadStartID(*field, "variable", "variable or local")))
					{
						return false;
					}
				}
				else if (field->is_string())
				{
					std::string const& text = field->get_ref<std::string const&>();
					std::optional<uint32_t> const labelled =
						ResolveDeclarationLabel(text, "variable", LabelKind::Variable, LabelKind::Local);
					if (m_Error)
					{
						return false;
					}
					variable = labelled;
					for (VisualScriptFunction const& owner : m_Working.Functions)
					{
						for (VisualScriptLocal const& local : owner.Locals)
						{
							if (!variable && function && owner.ID == *function && local.Name == text)
							{
								variable = local.ID;
							}
						}
					}
					if (!variable && !(variable = ResolveVariableValue(*field, "variable")))
					{
						return false;
					}
				}
				else
				{
					return FailField("variable", "expected a variable's or local's ID or name");
				}
			}
			std::optional<VisualScriptLink> link;
			if (Json const* const field = Find("link"))
			{
				std::optional<LinkReference> const resolved = ResolveLink(*field, "link");
				if (!resolved)
				{
					return false;
				}
				// The diagnostic names the link as the analysis saw it: compare the names given.
				link = VisualScriptLink{{resolved->From.End.Node, resolved->From.Given}, {resolved->To.End.Node, resolved->To.Given}};
			}
			if (!Require(BatchNeed::Analysis))
			{
				return false;
			}

			std::vector<VisualScriptDiagnostic const*> matches;
			Json candidates = Json::array();
			for (VisualScriptDiagnostic const& diagnostic : m_Analysis->Diagnostics)
			{
				if (diagnostic.Code != code)
				{
					continue;
				}
				candidates.push_back(DescribeDiagnosticPlace(diagnostic));
				bool const matched = (!node || diagnostic.Node == node) && (!pin || diagnostic.Pin == *pin) &&
				                     (!direction || diagnostic.Direction == direction) && (!link || diagnostic.Link == link) &&
				                     (!variable || diagnostic.Variable == variable) && (!function || diagnostic.Function == *function);
				if (matched)
				{
					matches.push_back(&diagnostic);
				}
			}
			if (matches.size() != 1)
			{
				return Fail({},
				            matches.empty()
				                ? fmt::format("no diagnostic {} of the visual script as the earlier operations left it matches", code)
				                : fmt::format("{} diagnostics {} match: give the fields that tell them apart", matches.size(), code),
				            Json::object({{"candidates", std::move(candidates)}}));
			}
			VisualScriptDiagnostic const& diagnostic = *matches.front();
			VisualScriptQuickFixGraphs const graphs = MakeFixGraphs();
			std::vector<VisualScriptQuickFix> const fixes =
				GetVisualScriptQuickFixes(m_Working, *m_Analysis, diagnostic, *m_Library, &graphs);
			auto const chosen = std::find_if(fixes.begin(), fixes.end(),
			                                 [&label](VisualScriptQuickFix const& candidate)
			                                 {
												 return candidate.Label == label;
											 });
			if (chosen == fixes.end())
			{
				std::vector<std::string> labels;
				Json offered = Json::array();
				for (VisualScriptQuickFix const& candidate : fixes)
				{
					labels.push_back(candidate.Label);
					offered.push_back(candidate.Label);
				}
				return FailField("fix", fmt::format("the diagnostic offers no fix {}", label),
				                 Json::object({{"names", FindClosestNames(label, std::move(labels))}, {"fixes", std::move(offered)}}));
			}
			if (chosen->Navigates)
			{
				return FailField("fix", fmt::format("{} only shows where to edit, changing nothing: make the edit with {}", label,
				                                    GetNavigatingFixOperation(*chosen, m_Working)));
			}
			uint32_t const largest = Editing::GetLargestVisualScriptID(m_Working);
			if (Result<void> const applied = ApplyVisualScriptQuickFix(m_Working, *m_Analysis, diagnostic, *chosen, &graphs); !applied)
			{
				return FailEdit(applied.GetError());
			}
			RecordChange(PinLevel);
			RecordRemovals();
			RecordCreatedAbove(largest);
			result["fix"] = label;
			return true;
		}
	}

	std::span<std::string_view const> GetVisualScriptBatchOperations()
	{
		return BatchOperationNames;
	}

	std::variant<VisualScriptBatchResult, VisualScriptBatchError>
	ApplyVisualScriptBatch(VisualScriptDocument const& stored, Json const& operations, VisualScriptBatchContext const& context)
	{
		BatchExecutor executor(stored, context);
		return executor.Run(operations);
	}

	Json GetVisualScriptFixOperations(VisualScriptDocument const& document, VisualScriptAnalysis const& analysis,
	                                  VisualScriptDiagnostic const& diagnostic, VisualScriptLibrary const& library,
	                                  VisualScriptQuickFixGraphs const* graphs)
	{
		Json operations = Json::array();
		for (VisualScriptQuickFix const& fix : GetVisualScriptQuickFixes(document, analysis, diagnostic, library, graphs))
		{
			if (fix.Navigates)
			{
				continue;
			}
			Json operation =
				Json::object({{"op", "Fix"}, {"code", diagnostic.Code}, {"fix", fix.Label}, {"function", diagnostic.Function}});
			if (diagnostic.Node)
			{
				operation["node"] = *diagnostic.Node;
			}
			if (!diagnostic.Pin.empty())
			{
				operation["pin"] = diagnostic.Pin;
			}
			if (diagnostic.Direction)
			{
				operation["direction"] = GetDirectionName(*diagnostic.Direction);
			}
			if (diagnostic.Link)
			{
				operation["link"] = DescribeLink(*diagnostic.Link);
			}
			if (diagnostic.Variable)
			{
				operation["variable"] = *diagnostic.Variable;
			}
			operations.push_back(Json::object({{"label", fix.Label}, {"operation", std::move(operation)}}));
		}
		return operations;
	}

	std::vector<VisualScriptLink> FindVisualScriptLinksGone(std::span<VisualScriptLink const> before,
	                                                        std::span<VisualScriptLink const> after)
	{
		std::unordered_set<VisualScriptLink const*, LinkHash, LinkEqual> kept;
		kept.reserve(after.size());
		for (VisualScriptLink const& link : after)
		{
			kept.insert(&link);
		}
		std::vector<VisualScriptLink> gone;
		for (VisualScriptLink const& link : before)
		{
			if (!kept.contains(&link))
			{
				gone.push_back(link);
			}
		}
		return gone;
	}

	std::vector<VisualScriptHint> FindVisualScriptHints(VisualScriptDocument const& document, VisualScriptAnalysis const& analysis,
	                                                    std::span<uint32_t const> nodes)
	{
		// The document and the analysis indexed once, so the hints of every node of a large graph take time linear in it.
		std::unordered_map<uint32_t, VisualScriptNode const*> found;
		std::unordered_map<uint32_t, std::vector<std::string_view>> linkedInto;
		std::unordered_set<uint32_t> read;
		auto const index = [&](VisualScriptGraph const& graph)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				found.try_emplace(node.ID, &node);
			}
			for (VisualScriptLink const& link : graph.Links)
			{
				linkedInto[link.To.Node].push_back(link.To.Pin);
				read.insert(link.From.Node);
			}
		};
		index(document.Graph);
		for (VisualScriptFunction const& function : document.Functions)
		{
			index(function.Graph);
		}
		std::unordered_map<uint32_t, VisualScriptAnalyzedNode const*> analyzedNodes;
		for (VisualScriptAnalyzedNode const& analyzed : analysis.Nodes)
		{
			analyzedNodes.try_emplace(analyzed.ID, &analyzed);
		}

		std::vector<VisualScriptHint> hints;
		std::unordered_set<uint32_t> seen;
		for (uint32_t const id : nodes)
		{
			if (!seen.insert(id).second)
			{
				continue;
			}
			auto const node = found.find(id);
			auto const entry = analyzedNodes.find(id);
			if (node == found.end() || entry == analyzedNodes.end())
			{
				continue;
			}
			VisualScriptNode const& stored = *node->second;
			VisualScriptAnalyzedNode const* const analyzed = entry->second;
			std::string const title = analyzed->Title.empty() ? stored.Type : analyzed->Title;
			auto const into = linkedInto.find(id);
			auto const isLinkedInto = [&into, &linkedInto](std::string_view pin)
			{
				return into != linkedInto.end() && std::find(into->second.begin(), into->second.end(), pin) != into->second.end();
			};
			VisualScriptPin const* firstExecInput = nullptr;
			bool flows = false;
			bool hasOutput = false;
			for (VisualScriptPin const& pin : analyzed->Pins)
			{
				if (pin.Direction == VisualScriptPinDirection::In && pin.Type == VisualScriptExecType)
				{
					firstExecInput = firstExecInput != nullptr ? firstExecInput : &pin;
					flows = flows || isLinkedInto(pin.Name);
				}
				if (pin.Direction == VisualScriptPinDirection::Out && pin.Type != VisualScriptExecType)
				{
					hasOutput = true;
				}
			}
			bool const event = analyzed->Kind == VisualScriptNodeKind::Event || stored.Type == "Function.Entry";
			if (firstExecInput != nullptr && !event && !flows)
			{
				hints.push_back(
					{id, "NeverRuns", fmt::format("{} (node {}) has no flow into {}: it never runs", title, id, firstExecInput->Name)});
			}
			if (firstExecInput == nullptr && analyzed->Pure && hasOutput && !read.contains(id))
			{
				hints.push_back({id, "Unused", fmt::format("{} (node {}) is read by nothing: link one of its outputs", title, id)});
			}
			if (stored.Type == "Expression")
			{
				for (VisualScriptPin const& pin : analyzed->Pins)
				{
					if (pin.Direction != VisualScriptPinDirection::In || pin.Type == VisualScriptExecType || isLinkedInto(pin.Name) ||
					    (stored.Inputs.is_object() && stored.Inputs.contains(pin.Name)))
					{
						continue;
					}
					char const* const zero = pin.Type == "bool" ? "false" : "0";
					hints.push_back({id, "UnlinkedInput",
					                 fmt::format("{} (node {}) reads {} as {}: link it or store a value", title, id, pin.Name, zero)});
				}
			}
		}
		return hints;
	}
}
