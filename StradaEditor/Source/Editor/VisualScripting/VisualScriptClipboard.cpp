#include "Editor/VisualScripting/VisualScriptClipboard.h"

#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strada
{
	namespace
	{
		using Format = VisualScriptSerializer;

		constexpr int ClipboardVersion = 1;
		constexpr std::string_view ClipboardType = "VisualScriptClipboard";
		constexpr std::string_view RefusalPrefix = "the clipboard holds no visual script: ";
		// How far into a text MayHoldVisualScriptClipboard looks for the clipboard's type.
		constexpr size_t SniffedBytes = 256;
		constexpr glm::vec2 DuplicateOffset{32.0f, 32.0f};
		constexpr float MaxCoordinate = static_cast<float>(Format::MaxCoordinate);

		constexpr std::string_view GetVariableType = "Variable.Get";
		constexpr std::string_view SetVariableType = "Variable.Set";
		constexpr std::string_view CallType = "Function.Call";
		constexpr std::string_view EntryType = "Function.Entry";
		constexpr std::string_view EventPrefix = "Event.";
		constexpr std::string_view SendEventType = "Event.Send";
		constexpr std::string_view CustomEventType = "Event.Custom";
		constexpr char const* VariableSetting = "Variable";
		constexpr char const* FunctionSetting = "Function";
		constexpr char const* NameSetting = "Name";
		constexpr std::string_view VariableKind = "Variable";
		constexpr std::string_view FunctionKind = "Function";

		// The events a script has at most one node of.
		constexpr std::array<std::string_view, 9> SingleEvents = {"Event.Create",       "Event.Update",         "Event.FixedUpdate",
		                                                          "Event.Destroy",      "Event.CollisionEnter", "Event.CollisionExit",
		                                                          "Event.TriggerEnter", "Event.TriggerExit",    "Event.Click"};
		// The input events a script has at most one node of per value of their setting (a key or a button).
		constexpr std::array<std::pair<std::string_view, char const*>, 6> InputEvents = {{
			{"Event.KeyPressed", "Key"},
			{"Event.KeyReleased", "Key"},
			{"Event.MouseButtonPressed", "Button"},
			{"Event.MouseButtonReleased", "Button"},
			{"Event.GamepadButtonPressed", "Button"},
			{"Event.GamepadButtonReleased", "Button"},
		}};

		// The members of the clipboard's objects; anything else is refused.
		constexpr std::array<std::string_view, 2> RootKeys = {"Strada", "VisualScriptClipboard"};
		constexpr std::array<std::string_view, 2> HeaderKeys = {"Version", "Type"};
		constexpr std::array<std::string_view, 2> PayloadKeys = {"Source", "Document"};
		constexpr std::array<std::string_view, 3> SourceKeys = {"Graph", "Function", "Locals"};
		constexpr std::array<std::string_view, 2> LocalKeys = {"ID", "Name"};

		enum class ReferenceKind : uint8_t
		{
			Variable = 0,
			Function
		};

		// A setting of a node that names a variable (or local) or a function by ID.
		struct Reference
		{
			std::string Setting;
			ReferenceKind Kind = ReferenceKind::Variable;
		};

		// The settings of a node that name variables or functions: those whose library setting is of that kind, and always
		// the built-in Variable setting of Variable.Get and Variable.Set and Function setting of Function.Call.
		std::vector<Reference> GetReferences(VisualScriptNode const& node, VisualScriptLibrary const* library)
		{
			std::vector<Reference> references;
			if (node.Type == GetVariableType || node.Type == SetVariableType)
			{
				references.push_back({VariableSetting, ReferenceKind::Variable});
			}
			else if (node.Type == CallType)
			{
				references.push_back({FunctionSetting, ReferenceKind::Function});
			}
			VisualScriptNodeTemplate const* const nodeTemplate = library != nullptr ? library->FindNode(node.Type) : nullptr;
			if (nodeTemplate == nullptr)
			{
				return references;
			}
			for (VisualScriptSetting const& setting : nodeTemplate->Settings)
			{
				bool const listed = std::any_of(references.begin(), references.end(),
				                                [&setting](Reference const& reference)
				                                {
													return reference.Setting == setting.Name;
												});
				if (listed)
				{
					continue;
				}
				if (setting.Kind == VariableKind)
				{
					references.push_back({setting.Name, ReferenceKind::Variable});
				}
				else if (setting.Kind == FunctionKind)
				{
					references.push_back({setting.Name, ReferenceKind::Function});
				}
			}
			return references;
		}

		// The ID a setting holds, however the JSON stores the integer; nothing for anything that is not an ID.
		std::optional<uint32_t> ReadID(Json const& settings, std::string const& key)
		{
			if (!settings.is_object())
			{
				return std::nullopt;
			}
			auto const found = settings.find(key);
			if (found == settings.end() || !found->is_number_integer())
			{
				return std::nullopt;
			}
			if (found->is_number_unsigned())
			{
				uint64_t const value = found->get<uint64_t>();
				return value >= 1 && value <= Format::MaxID ? std::optional<uint32_t>(static_cast<uint32_t>(value)) : std::nullopt;
			}
			int64_t const value = found->get<int64_t>();
			return value >= 1 && value <= static_cast<int64_t>(Format::MaxID) ? std::optional<uint32_t>(static_cast<uint32_t>(value))
			                                                                  : std::nullopt;
		}

		template<typename T>
		T const* FindByID(std::vector<T> const& elements, uint32_t id)
		{
			auto const found = std::find_if(elements.begin(), elements.end(),
			                                [id](T const& element)
			                                {
												return element.ID == id;
											});
			return found != elements.end() ? &*found : nullptr;
		}

		template<typename T>
		T* FindByID(std::vector<T>& elements, uint32_t id)
		{
			auto const found = std::find_if(elements.begin(), elements.end(),
			                                [id](T const& element)
			                                {
												return element.ID == id;
											});
			return found != elements.end() ? &*found : nullptr;
		}

		VisualScriptGraph const* FindGraph(VisualScriptDocument const& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			VisualScriptFunction const* const owner = FindByID(document.Functions, function);
			return owner != nullptr ? &owner->Graph : nullptr;
		}

		VisualScriptGraph* FindGraph(VisualScriptDocument& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			VisualScriptFunction* const owner = FindByID(document.Functions, function);
			return owner != nullptr ? &owner->Graph : nullptr;
		}

		glm::vec2 ClampPosition(glm::vec2 position)
		{
			return glm::clamp(position, glm::vec2(-MaxCoordinate), glm::vec2(MaxCoordinate));
		}

		// Hands out new IDs: from first upward, past the IDs in use. A clipboard of a document whose IDs end near the
		// format's last one starts again from 1, so what it writes stays readable.
		class IDAllocator
		{
		public:
			IDAllocator(uint32_t first, std::unordered_set<uint32_t> used)
				: m_Next(first),
				  m_Used(std::move(used))
			{
			}

			std::optional<uint32_t> Take()
			{
				while (m_Next != 0 && m_Next <= Format::MaxID && m_Used.contains(m_Next))
				{
					m_Next++;
				}
				if (m_Next == 0 || m_Next > Format::MaxID)
				{
					return std::nullopt;
				}
				m_Used.insert(m_Next);
				return m_Next++;
			}

		private:
			uint32_t m_Next;
			std::unordered_set<uint32_t> m_Used;
		};

		void CollectIDs(VisualScriptGraph const& graph, std::unordered_set<uint32_t>& ids)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				ids.insert(node.ID);
			}
			for (VisualScriptComment const& comment : graph.Comments)
			{
				ids.insert(comment.ID);
			}
		}

		// The top-left corner of the bounds of nodes and comment boxes, by their positions; (0, 0) when there is none. Nodes
		// without a position do not count.
		glm::vec2 GetCorner(std::span<VisualScriptNode const> nodes, std::span<VisualScriptComment const> comments)
		{
			std::optional<glm::vec2> corner;
			for (VisualScriptNode const& node : nodes)
			{
				if (node.Position)
				{
					corner = corner ? glm::min(*corner, *node.Position) : *node.Position;
				}
			}
			for (VisualScriptComment const& comment : comments)
			{
				corner = corner ? glm::min(*corner, comment.Position) : comment.Position;
			}
			return corner.value_or(glm::vec2(0.0f));
		}

		// What a copy holds before it becomes text, and where the copied elements stood.
		struct Copy
		{
			VisualScriptClipboardContent Content;
			glm::vec2 Corner{0.0f};
		};

		Copy MakeCopy(VisualScriptDocument const& document, uint32_t function, std::span<uint32_t const> elements, AssetHandle source,
		              VisualScriptLibrary const* library)
		{
			Copy copy;
			copy.Content.Source = source;
			copy.Content.Function = function;
			VisualScriptGraph const* const sourceGraph = FindGraph(document, function);
			if (sourceGraph == nullptr)
			{
				return copy;
			}
			VisualScriptFunction const* const sourceFunction = function != 0 ? FindByID(document.Functions, function) : nullptr;

			// Where the canvas shows the nodes: unplaced ones where placing them in memory puts them.
			VisualScriptDocument shown;
			shown.Graph = *sourceGraph;
			PlaceUnplacedNodes(shown);
			VisualScriptGraph& graph = shown.Graph;

			// The copied nodes and comments (the first of each ID, should a graph built in memory repeat one).
			std::unordered_set<uint32_t> const selected(elements.begin(), elements.end());
			std::unordered_set<uint32_t> copiedNodes;
			std::vector<VisualScriptNode> nodes;
			for (VisualScriptNode const& node : graph.Nodes)
			{
				if (selected.contains(node.ID) && copiedNodes.insert(node.ID).second)
				{
					nodes.push_back(node);
				}
			}
			std::unordered_set<uint32_t> copiedComments;
			std::vector<VisualScriptComment> comments;
			for (VisualScriptComment const& comment : graph.Comments)
			{
				if (selected.contains(comment.ID) && !copiedNodes.contains(comment.ID) && copiedComments.insert(comment.ID).second)
				{
					comments.push_back(comment);
				}
			}
			copy.Corner = GetCorner(nodes, comments);

			// What the nodes use: variables, the source function's locals, and functions with what their graphs use.
			std::unordered_set<uint32_t> usedVariables;
			std::unordered_set<uint32_t> usedLocals;
			std::unordered_set<uint32_t> usedFunctions;
			std::vector<uint32_t> pendingFunctions;
			auto const collect = [&](VisualScriptNode const& node, bool fromSource)
			{
				for (Reference const& reference : GetReferences(node, library))
				{
					std::optional<uint32_t> const id = ReadID(node.Settings, reference.Setting);
					if (!id)
					{
						continue;
					}
					if (reference.Kind == ReferenceKind::Function)
					{
						if (FindByID(document.Functions, *id) != nullptr && usedFunctions.insert(*id).second)
						{
							pendingFunctions.push_back(*id);
						}
					}
					else if (FindByID(document.Variables, *id) != nullptr)
					{
						usedVariables.insert(*id);
					}
					else if (fromSource && sourceFunction != nullptr && FindByID(sourceFunction->Locals, *id) != nullptr)
					{
						usedLocals.insert(*id);
					}
				}
			};
			for (VisualScriptNode const& node : nodes)
			{
				collect(node, true);
			}
			while (!pendingFunctions.empty())
			{
				uint32_t const id = pendingFunctions.back();
				pendingFunctions.pop_back();
				for (VisualScriptNode const& node : FindByID(document.Functions, id)->Graph.Nodes)
				{
					collect(node, false);
				}
			}

			VisualScriptDocument& clipboard = copy.Content.Document;
			for (VisualScriptVariable const& variable : document.Variables)
			{
				if (usedVariables.contains(variable.ID))
				{
					VisualScriptVariable& copied = clipboard.Variables.emplace_back(variable);
					copied.FormerNames.clear();
				}
			}
			for (VisualScriptFunction const& candidate : document.Functions)
			{
				if (usedFunctions.contains(candidate.ID))
				{
					clipboard.Functions.push_back(candidate);
				}
			}

			// New IDs for the copied nodes and comments, then for the flattened locals, above every ID kept.
			std::unordered_set<uint32_t> kept;
			for (VisualScriptVariable const& variable : clipboard.Variables)
			{
				kept.insert(variable.ID);
			}
			for (VisualScriptFunction const& copied : clipboard.Functions)
			{
				kept.insert(copied.ID);
				for (VisualScriptLocal const& local : copied.Locals)
				{
					kept.insert(local.ID);
				}
				CollectIDs(copied.Graph, kept);
			}
			uint32_t const largest = VisualScriptEditing::GetLargestVisualScriptID(document);
			size_t const needed = nodes.size() + comments.size() + usedLocals.size();
			bool const fitsAbove = largest < Format::MaxID && needed <= Format::MaxID - largest;
			IDAllocator ids(fitsAbove ? largest + 1 : 1, std::move(kept));
			std::unordered_map<uint32_t, uint32_t> renumbered;
			for (VisualScriptNode& node : nodes)
			{
				renumbered.emplace(node.ID, ids.Take().value_or(0));
			}
			std::vector<std::pair<uint32_t, uint32_t>> flattened;
			if (sourceFunction != nullptr)
			{
				for (VisualScriptLocal const& local : sourceFunction->Locals)
				{
					if (usedLocals.contains(local.ID))
					{
						flattened.emplace_back(local.ID, 0);
					}
				}
			}

			// The graph: nodes and comments relative to the corner, the links between the nodes.
			std::set<std::tuple<uint32_t, std::string, uint32_t, std::string>> copiedLinks;
			for (VisualScriptLink const& link : graph.Links)
			{
				auto const from = renumbered.find(link.From.Node);
				auto const to = renumbered.find(link.To.Node);
				if (from == renumbered.end() || to == renumbered.end())
				{
					continue;
				}
				// A graph built in memory may repeat a link, which the format's reader would drop.
				if (copiedLinks.emplace(from->second, link.From.Pin, to->second, link.To.Pin).second)
				{
					clipboard.Graph.Links.push_back({{from->second, link.From.Pin}, {to->second, link.To.Pin}});
				}
			}
			for (VisualScriptComment& comment : comments)
			{
				comment.ID = ids.Take().value_or(0);
				comment.Position = ClampPosition(comment.Position - copy.Corner);
				clipboard.Graph.Comments.push_back(std::move(comment));
			}
			for (std::pair<uint32_t, uint32_t>& local : flattened)
			{
				local.second = ids.Take().value_or(0);
			}
			for (VisualScriptNode& node : nodes)
			{
				node.ID = renumbered.at(node.ID);
				if (node.Position)
				{
					node.Position = ClampPosition(*node.Position - copy.Corner);
				}
				for (Reference const& reference : GetReferences(node, library))
				{
					if (reference.Kind != ReferenceKind::Variable)
					{
						continue;
					}
					std::optional<uint32_t> const id = ReadID(node.Settings, reference.Setting);
					if (!id)
					{
						continue;
					}
					auto const local = std::find_if(flattened.begin(), flattened.end(),
					                                [&id](std::pair<uint32_t, uint32_t> const& entry)
					                                {
														return entry.first == *id;
													});
					if (local != flattened.end())
					{
						node.Settings[reference.Setting] = local->second;
					}
				}
				clipboard.Graph.Nodes.push_back(std::move(node));
			}

			// The flattened locals: variables named apart from the clipboard's variables and its functions' locals.
			for (std::pair<uint32_t, uint32_t> const& entry : flattened)
			{
				VisualScriptLocal const& local = *FindByID(sourceFunction->Locals, entry.first);
				auto const taken = [&clipboard](std::string_view name)
				{
					bool const isVariable = std::any_of(clipboard.Variables.begin(), clipboard.Variables.end(),
					                                    [name](VisualScriptVariable const& variable)
					                                    {
															return variable.Name == name;
														});
					return isVariable || std::any_of(clipboard.Functions.begin(), clipboard.Functions.end(),
					                                 [name](VisualScriptFunction const& copied)
					                                 {
														 return std::any_of(copied.Locals.begin(), copied.Locals.end(),
						                                                    [name](VisualScriptLocal const& other)
						                                                    {
																				return other.Name == name;
																			});
													 });
				};
				VisualScriptVariable variable;
				variable.ID = entry.second;
				variable.Name = VisualScriptEditing::MakeUniqueVisualScriptName(local.Name, taken);
				variable.Type = local.Type;
				variable.Value = local.Value;
				clipboard.Variables.push_back(std::move(variable));
				copy.Content.Locals.push_back({entry.second, local.Name});
			}
			return copy;
		}

		bool HasOnlyKeys(Json const& object, std::span<std::string_view const> keys, std::string_view where, std::string& error)
		{
			for (auto const& member : object.items())
			{
				if (std::find(keys.begin(), keys.end(), member.key()) == keys.end())
				{
					error = fmt::format("{} has the unknown key {}", where,
					                    Json(member.key()).dump(-1, ' ', false, Json::error_handler_t::replace));
					return false;
				}
			}
			return true;
		}

		Error Refuse(std::string_view reason)
		{
			return Error{std::string(RefusalPrefix) + std::string(reason)};
		}

		bool IsEvent(std::string_view type)
		{
			return type.starts_with(EventPrefix) && type != SendEventType;
		}

		// What makes an event node unique in a script: its type, and for input events the key or button, for custom events
		// the name, as compact JSON (integers read alike however they are stored).
		using EventKey = std::pair<std::string, std::string>;

		EventKey MakeEventKey(std::string_view type, Json const& value)
		{
			return {std::string(type), value.dump(-1, ' ', false, Json::error_handler_t::replace)};
		}

		// Nothing for nodes that may repeat.

		std::optional<EventKey> GetUniqueEventKey(VisualScriptNode const& node)
		{
			if (std::find(SingleEvents.begin(), SingleEvents.end(), node.Type) != SingleEvents.end())
			{
				return MakeEventKey(node.Type, nullptr);
			}
			for (auto const& [type, setting] : InputEvents)
			{
				if (node.Type == type)
				{
					Json value = nullptr;
					if (node.Settings.is_object())
					{
						if (auto const found = node.Settings.find(setting); found != node.Settings.end())
						{
							value = *found;
						}
					}
					return MakeEventKey(type, value);
				}
			}
			if (node.Type == CustomEventType && node.Settings.is_object())
			{
				if (auto const name = node.Settings.find(NameSetting); name != node.Settings.end() && name->is_string())
				{
					return MakeEventKey(CustomEventType, *name);
				}
			}
			return std::nullopt;
		}

		std::string CountOf(size_t count, std::string_view one, std::string_view many)
		{
			return fmt::format("{} {}", count, count == 1 ? one : many);
		}

		// A paste in progress: the bindings made so far, on a copy of the target document.
		class Paste
		{
		public:
			Paste(VisualScriptDocument& document, AssetHandle targetGraph, VisualScriptClipboardContent const& content,
			      VisualScriptLibrary const* library)
				: m_Document(document),
				  m_Content(content),
				  m_Library(library),
				  m_SameScript(targetGraph.IsValid() && targetGraph == content.Source),
				  m_IDs(VisualScriptEditing::GetLargestVisualScriptID(document) + 1, {})
			{
				for (VisualScriptClipboardLocal const& local : content.Locals)
				{
					m_LocalNames.emplace(local.ID, local.Name);
				}
			}

			std::vector<std::string>& GetNotes() { return m_Notes; }

			Result<uint32_t> TakeID()
			{
				if (std::optional<uint32_t> const id = m_IDs.Take())
				{
					return *id;
				}
				return MakeError("the document has no free ID left: IDs end at {}", Format::MaxID);
			}

			// Rewrites the settings of a node that name what the content holds to what they are bound to. ownLocals maps the
			// locals of a function the content holds, for nodes of its graph; function is the target graph the node goes to.
			Result<void> BindReferences(VisualScriptNode& node, uint32_t function, std::unordered_map<uint32_t, uint32_t> const* ownLocals)
			{
				for (Reference const& reference : GetReferences(node, m_Library))
				{
					std::optional<uint32_t> const id = ReadID(node.Settings, reference.Setting);
					if (!id)
					{
						continue;
					}
					Result<std::optional<uint32_t>> bound =
						reference.Kind == ReferenceKind::Function ? BindFunction(*id) : BindVariable(*id, function, ownLocals);
					if (!bound)
					{
						return Error{bound.GetError()};
					}
					if (bound.GetValue())
					{
						node.Settings[reference.Setting] = *bound.GetValue();
					}
				}
				return {};
			}

			// Copies the graphs of the functions the paste added; their nodes bind what they use, which may add more.
			Result<void> CopyFunctionGraphs()
			{
				while (!m_PendingFunctions.empty())
				{
					auto const [source, target] = m_PendingFunctions.front();
					m_PendingFunctions.pop_front();
					VisualScriptFunction const& copied = *FindByID(m_Content.Document.Functions, source);
					std::unordered_map<uint32_t, uint32_t> const& locals = m_FunctionLocals.at(target);
					VisualScriptGraph graph;
					std::unordered_map<uint32_t, uint32_t> renumbered;
					for (VisualScriptNode const& original : copied.Graph.Nodes)
					{
						Result<uint32_t> id = TakeID();
						if (!id)
						{
							return Error{id.GetError()};
						}
						renumbered.emplace(original.ID, id.GetValue());
						VisualScriptNode& node = graph.Nodes.emplace_back(original);
						node.ID = id.GetValue();
						if (Result<void> bound = BindReferences(node, target, &locals); !bound)
						{
							return bound;
						}
					}
					for (VisualScriptLink const& link : copied.Graph.Links)
					{
						auto const from = renumbered.find(link.From.Node);
						auto const to = renumbered.find(link.To.Node);
						if (from != renumbered.end() && to != renumbered.end())
						{
							graph.Links.push_back({{from->second, link.From.Pin}, {to->second, link.To.Pin}});
						}
					}
					for (VisualScriptComment const& original : copied.Graph.Comments)
					{
						Result<uint32_t> id = TakeID();
						if (!id)
						{
							return Error{id.GetError()};
						}
						VisualScriptComment& comment = graph.Comments.emplace_back(original);
						comment.ID = id.GetValue();
					}
					FindByID(m_Document.Functions, target)->Graph = std::move(graph);
				}
				return {};
			}

		private:
			Result<std::optional<uint32_t>> BindVariable(uint32_t id, uint32_t function,
			                                             std::unordered_map<uint32_t, uint32_t> const* ownLocals)
			{
				if (ownLocals != nullptr)
				{
					if (auto const local = ownLocals->find(id); local != ownLocals->end())
					{
						return std::optional<uint32_t>(local->second);
					}
				}
				VisualScriptVariable const* const variable = FindByID(m_Content.Document.Variables, id);
				if (variable == nullptr)
				{
					return std::optional<uint32_t>();
				}
				auto const localName = m_LocalNames.find(id);
				bool const isLocal = localName != m_LocalNames.end();
				// A flattened local binds per target graph; a variable once for the paste.
				std::pair<uint32_t, uint32_t> const key(isLocal ? function : 0, id);
				if (auto const bound = m_BoundVariables.find(key); bound != m_BoundVariables.end())
				{
					return std::optional<uint32_t>(bound->second);
				}
				Result<uint32_t> bound = isLocal && function != 0
				                             ? BindLocal(*variable, localName->second, function)
				                             : BindDocumentVariable(*variable, isLocal ? localName->second : variable->Name, !isLocal);
				if (!bound)
				{
					return Error{bound.GetError()};
				}
				m_BoundVariables.emplace(key, bound.GetValue());
				return std::optional<uint32_t>(bound.GetValue());
			}

			Result<uint32_t> BindDocumentVariable(VisualScriptVariable const& variable, std::string const& name, bool byID)
			{
				if (byID && m_SameScript)
				{
					VisualScriptVariable const* const same = FindByID(m_Document.Variables, variable.ID);
					if (same != nullptr && same->Name == name && same->Type == variable.Type)
					{
						return same->ID;
					}
				}
				for (VisualScriptVariable const& existing : m_Document.Variables)
				{
					if (existing.Name == name && existing.Type == variable.Type)
					{
						return existing.ID;
					}
				}
				if (m_Document.Variables.size() >= Format::MaxVariables)
				{
					return MakeError("a script holds at most {} variables", Format::MaxVariables);
				}
				Result<uint32_t> id = TakeID();
				if (!id)
				{
					return id;
				}
				VisualScriptVariable added = variable;
				added.ID = id.GetValue();
				added.FormerNames.clear();
				added.Name = VisualScriptEditing::MakeUniqueVisualScriptName(name,
				                                                             [this](std::string_view candidate)
				                                                             {
																				 return VisualScriptEditing::IsVariableNameTaken(m_Document,
					                                                                                                             candidate);
																			 });
				m_Notes.push_back(fmt::format("added variable {} ({})", added.Name, added.Type));
				m_Document.Variables.push_back(std::move(added));
				return id;
			}

			Result<uint32_t> BindLocal(VisualScriptVariable const& variable, std::string const& name, uint32_t function)
			{
				VisualScriptFunction* const owner = FindByID(m_Document.Functions, function);
				if (owner == nullptr)
				{
					return MakeError("the document has no function {}", function);
				}
				for (VisualScriptLocal const& existing : owner->Locals)
				{
					if (existing.Name == name && existing.Type == variable.Type)
					{
						return existing.ID;
					}
				}
				if (owner->Locals.size() >= Format::MaxLocals)
				{
					return MakeError("a function holds at most {} locals", Format::MaxLocals);
				}
				Result<uint32_t> id = TakeID();
				if (!id)
				{
					return id;
				}
				VisualScriptLocal added;
				added.ID = id.GetValue();
				added.Name = VisualScriptEditing::MakeUniqueVisualScriptName(name,
				                                                             [this, function](std::string_view candidate)
				                                                             {
																				 return VisualScriptEditing::IsLocalNameTaken(
																					 m_Document, function, candidate);
																			 });
				added.Type = variable.Type;
				added.Value = variable.Value;
				m_Notes.push_back(fmt::format("added local {} ({}) to {}", added.Name, added.Type, owner->Name));
				owner->Locals.push_back(std::move(added));
				return id;
			}

			Result<std::optional<uint32_t>> BindFunction(uint32_t id)
			{
				if (auto const bound = m_BoundFunctions.find(id); bound != m_BoundFunctions.end())
				{
					return std::optional<uint32_t>(bound->second);
				}
				VisualScriptFunction const* const function = FindByID(m_Content.Document.Functions, id);
				if (function == nullptr)
				{
					return std::optional<uint32_t>();
				}
				auto const isAlike = [function](VisualScriptFunction const& existing)
				{
					return existing.Name == function->Name && existing.Pure == function->Pure && existing.Inputs == function->Inputs &&
					       existing.Outputs == function->Outputs;
				};
				VisualScriptFunction const* const same = m_SameScript ? FindByID(m_Document.Functions, id) : nullptr;
				if (same != nullptr && isAlike(*same))
				{
					m_BoundFunctions.emplace(id, same->ID);
					return std::optional<uint32_t>(same->ID);
				}
				auto const existing = std::find_if(m_Document.Functions.begin(), m_Document.Functions.end(), isAlike);
				if (existing != m_Document.Functions.end())
				{
					m_BoundFunctions.emplace(id, existing->ID);
					return std::optional<uint32_t>(existing->ID);
				}

				// A new function: its definition and locals now, its graph once the nodes pasted have bound what they use.
				if (m_Document.Functions.size() >= Format::MaxFunctions)
				{
					return MakeError("a script holds at most {} functions", Format::MaxFunctions);
				}
				Result<uint32_t> added = TakeID();
				if (!added)
				{
					return Error{added.GetError()};
				}
				VisualScriptFunction copy;
				copy.ID = added.GetValue();
				copy.Name = VisualScriptEditing::MakeUniqueVisualScriptName(function->Name,
				                                                            [this](std::string_view candidate)
				                                                            {
																				return VisualScriptEditing::IsFunctionNameTaken(m_Document,
					                                                                                                            candidate);
																			});
				copy.Pure = function->Pure;
				copy.Inputs = function->Inputs;
				copy.Outputs = function->Outputs;
				m_Notes.push_back(fmt::format("added function {}", copy.Name));
				m_Document.Functions.push_back(std::move(copy));
				m_BoundFunctions.emplace(id, added.GetValue());

				std::unordered_map<uint32_t, uint32_t>& locals = m_FunctionLocals[added.GetValue()];
				for (VisualScriptLocal const& local : function->Locals)
				{
					Result<uint32_t> localID = TakeID();
					if (!localID)
					{
						return Error{localID.GetError()};
					}
					VisualScriptLocal addedLocal = local;
					addedLocal.ID = localID.GetValue();
					addedLocal.Name = VisualScriptEditing::MakeUniqueVisualScriptName(local.Name,
					                                                                  [this, &added](std::string_view candidate)
					                                                                  {
																						  return VisualScriptEditing::IsLocalNameTaken(
																							  m_Document, added.GetValue(), candidate);
																					  });
					FindByID(m_Document.Functions, added.GetValue())->Locals.push_back(std::move(addedLocal));
					locals.emplace(local.ID, localID.GetValue());
				}
				m_PendingFunctions.emplace_back(id, added.GetValue());
				return std::optional<uint32_t>(added.GetValue());
			}

			VisualScriptDocument& m_Document;
			VisualScriptClipboardContent const& m_Content;
			VisualScriptLibrary const* m_Library;
			bool m_SameScript;
			IDAllocator m_IDs;
			// Flattened locals: the content's variable ID and the local's own name.
			std::unordered_map<uint32_t, std::string> m_LocalNames;
			// By target function (0 for variables and for flattened locals bound to variables) and content ID.
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> m_BoundVariables;
			std::unordered_map<uint32_t, uint32_t> m_BoundFunctions;
			// Of each function the paste added: its content locals' IDs and theirs.
			std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> m_FunctionLocals;
			// Functions added whose graphs are still to copy: content ID, target ID.
			std::deque<std::pair<uint32_t, uint32_t>> m_PendingFunctions;
			std::vector<std::string> m_Notes;
		};
	}

	std::string WriteVisualScriptClipboard(VisualScriptDocument const& document, uint32_t function, std::span<uint32_t const> elements,
	                                       AssetHandle source, VisualScriptLibrary const* library)
	{
		VisualScriptClipboardContent const content = MakeCopy(document, function, elements, source, library).Content;
		Json locals = Json::array();
		for (VisualScriptClipboardLocal const& local : content.Locals)
		{
			Json entry = Json::object();
			entry["ID"] = local.ID;
			entry["Name"] = local.Name;
			locals.push_back(std::move(entry));
		}
		Json sourceJson = Json::object();
		sourceJson["Graph"] = content.Source.ToString();
		sourceJson["Function"] = content.Function;
		sourceJson["Locals"] = std::move(locals);
		Json payload = Json::object();
		payload["Source"] = std::move(sourceJson);
		payload["Document"] = VisualScriptSerializer::SerializeDocument(content.Document);
		Json root = Json::object();
		root["Strada"] = MakeFileHeader(ClipboardType, ClipboardVersion);
		root[std::string(ClipboardType)] = std::move(payload);
		return DumpJson(root);
	}

	bool MayHoldVisualScriptClipboard(std::string_view text)
	{
		size_t const start = text.find_first_not_of(" \t\r\n");
		if (start == std::string_view::npos || text[start] != '{')
		{
			return false;
		}
		return text.substr(0, SniffedBytes).find(ClipboardType) != std::string_view::npos;
	}

	Result<VisualScriptClipboardContent> ReadVisualScriptClipboard(std::string_view text)
	{
		if (text.size() > MaxVisualScriptClipboardBytes)
		{
			return Refuse(fmt::format("the text is longer than {} bytes", MaxVisualScriptClipboardBytes));
		}
		Result<Json> parsed = ParseJson(text);
		if (!parsed)
		{
			return Refuse(parsed.GetError());
		}
		Json const& root = parsed.GetValue();
		if (Result<int> version = ReadFileHeader(root, ClipboardType, ClipboardVersion); !version)
		{
			return Refuse(version.GetError());
		}
		std::string error;
		if (!HasOnlyKeys(root, RootKeys, "the clipboard", error) || !HasOnlyKeys(*root.find("Strada"), HeaderKeys, "its header", error))
		{
			return Refuse(error);
		}
		auto const payload = root.find(ClipboardType);
		if (payload == root.end() || !payload->is_object())
		{
			return Refuse("\"VisualScriptClipboard\" is missing or not an object");
		}
		if (!HasOnlyKeys(*payload, PayloadKeys, "\"VisualScriptClipboard\"", error))
		{
			return Refuse(error);
		}
		auto const source = payload->find("Source");
		auto const documentJson = payload->find("Document");
		if (source == payload->end() || !source->is_object())
		{
			return Refuse("\"Source\" is missing or not an object");
		}
		if (documentJson == payload->end() || !documentJson->is_object())
		{
			return Refuse("\"Document\" is missing or not an object");
		}
		if (!HasOnlyKeys(*source, SourceKeys, "\"Source\"", error))
		{
			return Refuse(error);
		}

		VisualScriptClipboardContent content;
		auto const graph = source->find("Graph");
		std::optional<UUID> const handle =
			graph != source->end() && graph->is_string() ? UUID::FromString(graph->get_ref<std::string const&>()) : std::nullopt;
		if (!handle)
		{
			return Refuse("\"Source.Graph\" is not a visual script's handle in decimal");
		}
		content.Source = AssetHandle(*handle);
		auto const function = source->find("Function");
		if (function == source->end() || !function->is_number_unsigned() || function->get<uint64_t>() > Format::MaxID)
		{
			return Refuse("\"Source.Function\" is not 0 or an ID");
		}
		content.Function = static_cast<uint32_t>(function->get<uint64_t>());

		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument(*documentJson, context, &issues);
		if (!document)
		{
			return Refuse(document.GetError());
		}
		if (!issues.empty())
		{
			return Refuse(fmt::format("Document.{}: {}", issues.front().Path, issues.front().Message));
		}
		content.Document = document.TakeValue();

		auto const locals = source->find("Locals");
		if (locals != source->end())
		{
			if (!locals->is_array())
			{
				return Refuse("\"Source.Locals\" is not an array");
			}
			for (Json const& entry : *locals)
			{
				if (!entry.is_object() || !HasOnlyKeys(entry, LocalKeys, "an entry of \"Source.Locals\"", error))
				{
					return Refuse(error.empty() ? "an entry of \"Source.Locals\" is not an object" : error);
				}
				auto const id = entry.find("ID");
				auto const name = entry.find("Name");
				if (id == entry.end() || !id->is_number_unsigned() || id->get<uint64_t>() > Format::MaxID ||
				    FindByID(content.Document.Variables, static_cast<uint32_t>(id->get<uint64_t>())) == nullptr)
				{
					return Refuse("an entry of \"Source.Locals\" names no variable of the document");
				}
				uint32_t const localID = static_cast<uint32_t>(id->get<uint64_t>());
				if (name == entry.end() || !name->is_string() || !VisualScriptSerializer::IsValidName(name->get_ref<std::string const&>()))
				{
					return Refuse(fmt::format("the local {} has no name a local can have", localID));
				}
				if (FindByID(content.Locals, localID) != nullptr)
				{
					return Refuse(fmt::format("\"Source.Locals\" lists the variable {} twice", localID));
				}
				content.Locals.push_back({localID, name->get<std::string>()});
			}
		}
		return content;
	}

	Result<VisualScriptPasteResult> PasteVisualScriptClipboard(VisualScriptDocument& target, AssetHandle targetGraph, uint32_t function,
	                                                           VisualScriptClipboardContent const& content, glm::vec2 at,
	                                                           VisualScriptLibrary const* library)
	{
		if (FindGraph(target, function) == nullptr)
		{
			return MakeError("the document has no function {}", function);
		}
		if (!std::isfinite(at.x) || !std::isfinite(at.y))
		{
			return Error{"a paste's position must be finite"};
		}

		// Where the content's nodes stand: unplaced ones where the canvas would show them.
		VisualScriptDocument placed;
		placed.Graph = content.Document.Graph;
		PlaceUnplacedNodes(placed);
		VisualScriptGraph const& source = placed.Graph;
		glm::vec2 const offset = at - GetCorner(source.Nodes, source.Comments);

		// What stays out: events in a function's graph, events the event graph has, a second entry.
		VisualScriptGraph const& before = *FindGraph(target, function);
		std::set<EventKey> events;
		bool hasEntry = false;
		for (VisualScriptNode const& node : before.Nodes)
		{
			if (std::optional<EventKey> key = GetUniqueEventKey(node))
			{
				events.insert(std::move(*key));
			}
			hasEntry = hasEntry || node.Type == EntryType;
		}
		size_t eventsInFunction = 0;
		size_t repeatedEvents = 0;
		size_t repeatedEntries = 0;
		std::vector<VisualScriptNode const*> kept;
		for (VisualScriptNode const& node : source.Nodes)
		{
			if (function != 0 && IsEvent(node.Type))
			{
				eventsInFunction++;
				continue;
			}
			if (function == 0)
			{
				if (std::optional<EventKey> key = GetUniqueEventKey(node))
				{
					if (!events.insert(std::move(*key)).second)
					{
						repeatedEvents++;
						continue;
					}
				}
			}
			else if (node.Type == EntryType)
			{
				if (hasEntry)
				{
					repeatedEntries++;
					continue;
				}
				hasEntry = true;
			}
			kept.push_back(&node);
		}

		VisualScriptDocument document = target;
		Paste paste(document, targetGraph, content, library);
		VisualScriptPasteResult result;

		// What the nodes use first, then the nodes, then the graphs of the functions that added.
		std::vector<VisualScriptNode> nodes;
		nodes.reserve(kept.size());
		for (VisualScriptNode const* original : kept)
		{
			VisualScriptNode& node = nodes.emplace_back(*original);
			if (Result<void> bound = paste.BindReferences(node, function, nullptr); !bound)
			{
				return Error{bound.GetError()};
			}
		}
		std::unordered_map<uint32_t, uint32_t> renumbered;
		for (VisualScriptNode& node : nodes)
		{
			Result<uint32_t> id = paste.TakeID();
			if (!id)
			{
				return Error{id.GetError()};
			}
			renumbered.emplace(node.ID, id.GetValue());
			node.ID = id.GetValue();
			node.Position = ClampPosition(node.Position.value_or(glm::vec2(0.0f)) + offset);
			result.Pasted.push_back(node.ID);
		}
		std::vector<VisualScriptComment> comments;
		for (VisualScriptComment const& original : source.Comments)
		{
			Result<uint32_t> id = paste.TakeID();
			if (!id)
			{
				return Error{id.GetError()};
			}
			VisualScriptComment& comment = comments.emplace_back(original);
			comment.ID = id.GetValue();
			comment.Position = ClampPosition(comment.Position + offset);
			result.Pasted.push_back(comment.ID);
		}
		if (Result<void> copied = paste.CopyFunctionGraphs(); !copied)
		{
			return Error{copied.GetError()};
		}

		VisualScriptGraph& graph = *FindGraph(document, function);
		if (graph.Nodes.size() + nodes.size() > Format::MaxNodesPerGraph)
		{
			return MakeError("a graph holds at most {} nodes: the paste would make {}", Format::MaxNodesPerGraph,
			                 graph.Nodes.size() + nodes.size());
		}
		if (graph.Comments.size() + comments.size() > Format::MaxCommentsPerGraph)
		{
			return MakeError("a graph holds at most {} comments: the paste would make {}", Format::MaxCommentsPerGraph,
			                 graph.Comments.size() + comments.size());
		}
		size_t links = 0;
		for (VisualScriptLink const& link : source.Links)
		{
			auto const from = renumbered.find(link.From.Node);
			auto const to = renumbered.find(link.To.Node);
			if (from != renumbered.end() && to != renumbered.end())
			{
				links++;
			}
		}
		if (graph.Links.size() + links > Format::MaxLinksPerGraph)
		{
			return MakeError("a graph holds at most {} links: the paste would make {}", Format::MaxLinksPerGraph,
			                 graph.Links.size() + links);
		}
		for (VisualScriptLink const& link : source.Links)
		{
			auto const from = renumbered.find(link.From.Node);
			auto const to = renumbered.find(link.To.Node);
			if (from != renumbered.end() && to != renumbered.end())
			{
				graph.Links.push_back({{from->second, link.From.Pin}, {to->second, link.To.Pin}});
			}
		}
		result.NodeCount = static_cast<uint32_t>(nodes.size());
		std::move(nodes.begin(), nodes.end(), std::back_inserter(graph.Nodes));
		std::move(comments.begin(), comments.end(), std::back_inserter(graph.Comments));

		result.Notes = std::move(paste.GetNotes());
		if (eventsInFunction > 0)
		{
			result.Notes.push_back(
				fmt::format("left out {}: events belong in the event graph", CountOf(eventsInFunction, "event", "events")));
		}
		if (repeatedEvents > 0)
		{
			result.Notes.push_back(fmt::format("left out {} the event graph has already", CountOf(repeatedEvents, "event", "events")));
		}
		if (repeatedEntries > 0)
		{
			result.Notes.push_back(
				fmt::format("left out {}: the function has one already", CountOf(repeatedEntries, "function entry", "function entries")));
		}
		target = std::move(document);
		return result;
	}

	Result<VisualScriptPasteResult> DuplicateVisualScriptElements(VisualScriptDocument& document, AssetHandle graph, uint32_t function,
	                                                              std::span<uint32_t const> elements, VisualScriptLibrary const* library)
	{
		if (FindGraph(document, function) == nullptr)
		{
			return MakeError("the document has no function {}", function);
		}
		Copy const copy = MakeCopy(document, function, elements, graph, library);
		return PasteVisualScriptClipboard(document, graph, function, copy.Content, copy.Corner + DuplicateOffset, library);
	}
}
