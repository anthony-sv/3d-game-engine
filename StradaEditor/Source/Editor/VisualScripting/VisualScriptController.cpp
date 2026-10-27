#include "Editor/VisualScripting/VisualScriptController.h"

#include "Editor/VisualScripting/VisualScriptArrange.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>

namespace Strada
{
	namespace
	{
		using Action = VisualScriptEditorAction;
		using Direction = VisualScriptPinDirection;
		namespace Editing = VisualScriptEditing;
		namespace Metrics = VisualScriptCanvasMetrics;

		static_assert(VisualScriptNudgeStep == Metrics::GridSize, "an arrow key nudges by one grid step");

		// "Node" or "3 Nodes": a count of things the description names, the count left out for one.
		std::string CountNoun(size_t count, std::string_view singular, std::string_view plural)
		{
			return count == 1 ? std::string(singular) : fmt::format("{} {}", count, plural);
		}

		// What an edit of nodes and comment boxes acted on: "Node", "3 Nodes", "Comment", "2 Comments", "3 Nodes and 1 Comment".
		std::string DescribeElements(size_t nodes, size_t comments)
		{
			if (comments == 0)
			{
				return CountNoun(nodes, "Node", "Nodes");
			}
			if (nodes == 0)
			{
				return CountNoun(comments, "Comment", "Comments");
			}
			return fmt::format("{} {} and {} {}", nodes, nodes == 1 ? "Node" : "Nodes", comments, comments == 1 ? "Comment" : "Comments");
		}

		// The same in a sentence of a toast: "5 nodes", "1 node and 2 comments".
		std::string DescribeElementsInText(size_t nodes, size_t comments)
		{
			std::string text = fmt::format("{} {}", nodes, nodes == 1 ? "node" : "nodes");
			if (comments != 0)
			{
				text += fmt::format(" and {} {}", comments, comments == 1 ? "comment" : "comments");
			}
			return text;
		}

		glm::vec2 SnapToGrid(glm::vec2 point)
		{
			return glm::round(point / Metrics::GridSize) * Metrics::GridSize;
		}

		// An ID setting as the scripting runtime reads it: an integer from 0 to the largest ID (a float names nothing).
		std::optional<uint32_t> ReadId(Json const& settings, char const* key)
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
				return value <= std::numeric_limits<uint32_t>::max() ? std::optional<uint32_t>(static_cast<uint32_t>(value)) : std::nullopt;
			}
			int64_t const value = found->get<int64_t>();
			return value >= 0 && value <= std::numeric_limits<uint32_t>::max() ? std::optional<uint32_t>(static_cast<uint32_t>(value))
			                                                                   : std::nullopt;
		}

		std::optional<std::string> ReadString(Json const& settings, char const* key)
		{
			if (!settings.is_object())
			{
				return std::nullopt;
			}
			auto const found = settings.find(key);
			if (found == settings.end() || !found->is_string())
			{
				return std::nullopt;
			}
			return found->get<std::string>();
		}

		VisualScriptGraph const* FindGraph(VisualScriptDocument const& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			for (VisualScriptFunction const& candidate : document.Functions)
			{
				if (candidate.ID == function)
				{
					return &candidate.Graph;
				}
			}
			return nullptr;
		}

		VisualScriptFunction const* FindFunction(VisualScriptDocument const& document, uint32_t function)
		{
			for (VisualScriptFunction const& candidate : document.Functions)
			{
				if (candidate.ID == function)
				{
					return &candidate;
				}
			}
			return nullptr;
		}

		VisualScriptNode const* FindNode(VisualScriptGraph const& graph, uint32_t id)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				if (node.ID == id)
				{
					return &node;
				}
			}
			return nullptr;
		}

		VisualScriptComment const* FindComment(VisualScriptGraph const& graph, uint32_t id)
		{
			for (VisualScriptComment const& comment : graph.Comments)
			{
				if (comment.ID == id)
				{
					return &comment;
				}
			}
			return nullptr;
		}

		bool HasRect(VisualScriptRect const& outer, VisualScriptRect const& inner)
		{
			return inner.Min.x >= outer.Min.x && inner.Min.y >= outer.Min.y && inner.Max.x <= outer.Max.x && inner.Max.y <= outer.Max.y;
		}

		bool IsLinkOf(VisualScriptLink const& link, uint32_t node)
		{
			return link.From.Node == node || link.To.Node == node;
		}

		// The edits of one call: what the controller computes on a copy of the window's display document.
		class Controller
		{
		public:
			explicit Controller(VisualScriptEditContext const& context)
				: m_Context(context),
				  m_Model(context.Model),
				  m_Canvas(context.Model.GetCanvas()),
				  m_Function(context.Model.GetFunction())
			{
			}

			VisualScriptControllerResult TakeResult() { return std::move(m_Result); }

			// --- Intents ---

			Result<void> Resolve(VisualScriptMoveIntent const& intent)
			{
				if (!CanEdit())
				{
					return {};
				}
				std::pair<size_t, size_t> const counts = CountElements(intent.Elements);
				VisualScriptDocument document = GetDisplay();
				if (Result<void> moved = Editing::MoveElements(document, m_Function, intent.Elements, intent.Offset); !moved)
				{
					return moved;
				}
				return Commit(std::move(document), "Move " + DescribeElements(counts.first, counts.second));
			}

			Result<void> Resolve(VisualScriptConnectIntent const& intent)
			{
				if (!CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				VisualScriptLink const link = ToStored(intent.Link);
				std::string const from = GetPinLabel(intent.Link.From.Node, intent.Link.From.Pin, Direction::Out);
				std::string const to = GetPinLabel(intent.Link.To.Node, intent.Link.To.Pin, Direction::In);
				VisualScriptDocument document = GetDisplay();
				VisualScriptNodePins const pins = MakePins(document);
				if (!intent.InsertConvert)
				{
					Result<uint32_t> const replaced = Editing::Connect(document, m_Function, link, pins);
					if (!replaced)
					{
						return Error{replaced.GetError()};
					}
					return Commit(std::move(document), fmt::format("Connect {} to {}{}", from, to, DescribeReplaced(replaced.GetValue())));
				}

				// A Convert to the input's type between the two pins, at the middle of where the wire would run.
				glm::vec2 const middle = (GetPinPoint(intent.Link.From.Node, intent.Link.From.Pin, Direction::Out) +
				                          GetPinPoint(intent.Link.To.Node, intent.Link.To.Pin, Direction::In)) *
				                         0.5f;
				Result<uint32_t> const convert = Editing::AddNode(document, m_Function, "Convert", Json::object({{"To", intent.ConvertTo}}),
				                                                  SnapToGrid(middle - ConvertHalfSize));
				if (!convert)
				{
					return Error{convert.GetError()};
				}
				VisualScriptLink const into{link.From, {convert.GetValue(), "Value"}};
				VisualScriptLink const out{{convert.GetValue(), "Result"}, link.To};
				if (Result<uint32_t> const connected = Editing::Connect(document, m_Function, into, pins); !connected)
				{
					return Error{connected.GetError()};
				}
				Result<uint32_t> const replaced = Editing::Connect(document, m_Function, out, pins);
				if (!replaced)
				{
					return Error{replaced.GetError()};
				}
				return Commit(std::move(document),
				              fmt::format("Connect {} to {} (Convert){}", from, to, DescribeReplaced(replaced.GetValue())));
			}

			Result<void> Resolve(VisualScriptRelinkIntent const& intent)
			{
				if (!CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				std::vector<VisualScriptLink> add;
				add.reserve(intent.Add.size());
				for (VisualScriptLink const& link : intent.Add)
				{
					add.push_back(ToStored(link));
				}
				VisualScriptDocument document = GetDisplay();
				Result<uint32_t> const relinked = Editing::Relink(document, m_Function, intent.Remove, add, MakePins(document));
				if (!relinked)
				{
					return Error{relinked.GetError()};
				}
				return Commit(std::move(document), "Move " + CountNoun(intent.Remove.size(), "Link", "Links"));
			}

			Result<void> Resolve(VisualScriptDisconnectIntent const& intent)
			{
				if (!CanEdit() || intent.Links.empty())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				if (Result<void> disconnected = Editing::Disconnect(document, m_Function, intent.Links); !disconnected)
				{
					return disconnected;
				}
				return Commit(std::move(document), DescribeDisconnect(intent.Links));
			}

			Result<void> Resolve(VisualScriptResizeCommentIntent const& intent)
			{
				if (!CanEdit())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				if (Result<void> resized = Editing::SetCommentGeometry(document, intent.Comment, intent.Rect.Min, intent.Rect.GetSize());
				    !resized)
				{
					return resized;
				}
				return Commit(std::move(document), "Resize Comment");
			}

			Result<void> Resolve(VisualScriptInsertRerouteIntent const& intent)
			{
				if (!CanEdit())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				Result<uint32_t> const reroute = Editing::InsertReroute(document, m_Function, intent.Link, intent.Position);
				if (!reroute)
				{
					return Error{reroute.GetError()};
				}
				return Commit(std::move(document), "Add Reroute");
			}

			Result<void> Resolve(VisualScriptSpliceIntent const& intent)
			{
				if (!CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				std::string const title = GetNodeTitle(intent.Node);
				std::string const input = ToStored(intent.Node, intent.InputPin, Direction::In);
				std::string const output = ToStored(intent.Node, intent.OutputPin, Direction::Out);
				VisualScriptDocument document = GetDisplay();
				uint32_t const nodes[] = {intent.Node};
				if (Result<void> moved = Editing::MoveElements(document, m_Function, nodes, intent.Offset); !moved)
				{
					return moved;
				}
				if (Result<void> spliced =
				        Editing::SpliceNode(document, m_Function, intent.Node, intent.Link, input, output, MakePins(document));
				    !spliced)
				{
					return spliced;
				}
				return Commit(std::move(document), "Insert " + title);
			}

			Result<void> Resolve(VisualScriptPaletteIntent const& intent)
			{
				m_Result.OpenPalette = intent;
				return {};
			}

			Result<void> Resolve(VisualScriptContextMenuIntent const& intent)
			{
				m_Result.ContextMenu = intent;
				return {};
			}

			Result<void> Resolve(VisualScriptPinMenuIntent const& intent)
			{
				m_Result.PinMenu = intent;
				return {};
			}

			Result<void> Resolve(VisualScriptOpenFunctionIntent const& intent)
			{
				m_Result.ShowFunction = intent.Function;
				return {};
			}

			Result<void> Resolve(VisualScriptDropIntent const& intent)
			{
				switch (intent.Kind)
				{
					case VisualScriptDropKind::Asset:
						return DropAsset(intent);
					case VisualScriptDropKind::Entities:
						return DropEntities(intent);
					case VisualScriptDropKind::Element:
						// My Script's rows are resolved with the panes, which encode them.
						return {};
				}
				return {};
			}

			Result<void> Resolve(VisualScriptActionIntent const& intent)
			{
				std::vector<uint32_t> nodes;
				std::vector<uint32_t> comments;
				SplitSelection(intent.Selection, nodes, comments);
				std::vector<uint32_t> elements = nodes;
				elements.insert(elements.end(), comments.begin(), comments.end());
				switch (intent.Action)
				{
					case Action::Delete:
					case Action::DeleteWithoutRelink:
					case Action::Cut:
						return Remove(intent.Action, elements);
					case Action::Copy:
						if (!elements.empty())
						{
							m_Result.Clipboard =
								WriteVisualScriptClipboard(GetDisplay(), m_Function, elements, m_Context.Graph, m_Context.Library);
						}
						return {};
					case Action::Paste:
						return Paste(intent.Position);
					case Action::Duplicate:
						return Duplicate(elements);
					case Action::Rename:
						return Rename(intent.Selection);
					case Action::AddCommentBox:
						return AddCommentBox(elements, intent.Position);
					case Action::Straighten:
						return Straighten(nodes);
					case Action::AlignLeft:
						return Align(elements, VisualScriptAlignment::Left, "Left");
					case Action::AlignRight:
						return Align(elements, VisualScriptAlignment::Right, "Right");
					case Action::AlignTop:
						return Align(elements, VisualScriptAlignment::Top, "Top");
					case Action::AlignBottom:
						return Align(elements, VisualScriptAlignment::Bottom, "Bottom");
					case Action::AlignCenters:
						return Align(elements, VisualScriptAlignment::Centers, "Centers");
					case Action::AlignMiddles:
						return Align(elements, VisualScriptAlignment::Middles, "Middles");
					case Action::DistributeHorizontally:
						return Distribute(elements, VisualScriptDistribution::Horizontal);
					case Action::DistributeVertically:
						return Distribute(elements, VisualScriptDistribution::Vertical);
					case Action::Arrange:
						return Arrange(nodes);
					case Action::ToggleCollapsed:
						return ToggleCollapsed(nodes);
					case Action::ToggleDisabled:
					case Action::SetEnabled:
					case Action::SetDisabled:
					case Action::SetDevelopmentOnly:
						return SetEnabledState(intent.Action, nodes);
					case Action::BreakNodeLinks:
						return BreakNodeLinks(nodes);
					case Action::AddNodeComment:
					case Action::EditNodeComment:
						return OpenNodeComment(nodes);
					case Action::RemoveNodeComment:
						return RemoveNodeComments(nodes);
					case Action::FindReferences:
						return FindReferences(nodes);
					case Action::OpenNode:
						return OpenNode(nodes);
					case Action::UpdateNode:
						return UpdateNodes();
					case Action::NudgeLeft:
						return Nudge(elements, glm::vec2(-VisualScriptNudgeStep, 0.0f));
					case Action::NudgeRight:
						return Nudge(elements, glm::vec2(VisualScriptNudgeStep, 0.0f));
					case Action::NudgeUp:
						return Nudge(elements, glm::vec2(0.0f, -VisualScriptNudgeStep));
					case Action::NudgeDown:
						return Nudge(elements, glm::vec2(0.0f, VisualScriptNudgeStep));
					case Action::NudgeLeftFine:
						return Nudge(elements, glm::vec2(-VisualScriptFineNudgeStep, 0.0f));
					case Action::NudgeRightFine:
						return Nudge(elements, glm::vec2(VisualScriptFineNudgeStep, 0.0f));
					case Action::NudgeUpFine:
						return Nudge(elements, glm::vec2(0.0f, -VisualScriptFineNudgeStep));
					case Action::NudgeDownFine:
						return Nudge(elements, glm::vec2(0.0f, VisualScriptFineNudgeStep));
					default:
						// Selection, view, navigation, bookmarks, find and the shortcut sheet: the model and the panel's.
						return {};
				}
			}

			// --- The window's edits ---

			Result<void> Resolve(VisualScriptLiteralEdit const& edit)
			{
				if (!CanEdit())
				{
					return {};
				}
				std::string const title = GetNodeTitle(edit.Pin.Node);
				std::string const label = GetPinLabel(edit.Pin.Node, edit.Pin.Pin, Direction::In);
				std::string const pin = ToStored(edit.Pin.Node, edit.Pin.Pin, Direction::In);
				VisualScriptDocument document = GetDisplay();
				bool const reset = edit.Literal.is_null();
				if (Result<void> set = Editing::SetInput(document, edit.Pin.Node, pin, edit.Literal); !set)
				{
					return set;
				}
				return Commit(std::move(document), fmt::format("{} {} on {}", reset ? "Reset" : "Set", label, title), m_Context.MergeKey);
			}

			Result<void> Resolve(VisualScriptPinEdit const& edit)
			{
				switch (edit.Action)
				{
					case VisualScriptPinAction::BreakAllLinks:
						return BreakPinLinks(edit);
					case VisualScriptPinAction::BreakLink:
						if (!edit.Link)
						{
							return Error{"Break Link names no link"};
						}
						return Resolve(VisualScriptDisconnectIntent{{*edit.Link}});
					case VisualScriptPinAction::ResetToDefault:
						return Resolve(VisualScriptLiteralEdit{edit.Pin, nullptr});
					case VisualScriptPinAction::Split:
					case VisualScriptPinAction::Recombine:
						return SplitPin(edit);
					case VisualScriptPinAction::PromoteToVariable:
					case VisualScriptPinAction::PromoteToFunctionInput:
						return Promote(edit);
				}
				return {};
			}

			Result<void> Resolve(VisualScriptNodeCommentEdit const& edit)
			{
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				VisualScriptNode const* node = graph != nullptr ? FindNode(*graph, edit.Node) : nullptr;
				if (node == nullptr || node->Comment == edit.Text)
				{
					// Nothing to write: a new comment left empty, or the text unchanged.
					return {};
				}
				if (!CanEdit())
				{
					return {};
				}
				std::string const title = GetNodeTitle(edit.Node);
				std::string action = node->Comment.empty() ? "Comment " + title
				                     : edit.Text.empty()   ? "Remove Comment of " + title
				                                           : "Edit Comment of " + title;
				VisualScriptDocument document = GetDisplay();
				uint32_t const nodes[] = {edit.Node};
				if (Result<void> set = Editing::SetNodeComment(document, nodes, edit.Text); !set)
				{
					return set;
				}
				// The text edited in place is a widget interaction: it commits under the shared edit session's key.
				return Commit(std::move(document), action, m_Context.MergeKey);
			}

			Result<void> Resolve(VisualScriptCommentEdit const& edit)
			{
				if (!CanEdit())
				{
					return {};
				}
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				VisualScriptComment const* comment = graph != nullptr ? FindComment(*graph, edit.Comment) : nullptr;
				if (comment == nullptr)
				{
					return MakeError("the graph has no comment {}", edit.Comment);
				}
				VisualScriptDocument document = GetDisplay();
				switch (edit.Action)
				{
					case VisualScriptCommentAction::Rename:
					{
						// The title is the first line; the body keeps the others.
						size_t const end = comment->Text.find('\n');
						std::string text = edit.Text + (end == std::string::npos ? std::string() : comment->Text.substr(end));
						if (Result<void> set = Editing::SetCommentText(document, edit.Comment, std::move(text)); !set)
						{
							return set;
						}
						return Commit(std::move(document), "Rename Comment", m_Context.MergeKey);
					}
					case VisualScriptCommentAction::Recolour:
						if (Result<void> set = Editing::SetCommentColor(document, edit.Comment, edit.Color); !set)
						{
							return set;
						}
						return Commit(std::move(document), "Recolour Comment");
					case VisualScriptCommentAction::FitToContents:
					{
						std::vector<uint32_t> contents = GetCommentContents(edit.Comment);
						std::optional<VisualScriptRect> const bounds = m_Canvas.GetBounds(contents);
						if (!bounds)
						{
							m_Result.Notes.emplace_back("The comment frames no nodes");
							return {};
						}
						glm::vec2 const position = bounds->Min - glm::vec2(Editing::CommentFrameMargin,
						                                                   Editing::CommentFrameMargin + Editing::CommentFrameTitleSpace);
						glm::vec2 const size =
							bounds->GetSize() + glm::vec2(2.0f * Editing::CommentFrameMargin,
						                                  2.0f * Editing::CommentFrameMargin + Editing::CommentFrameTitleSpace);
						if (Result<void> set = Editing::SetCommentGeometry(document, edit.Comment, position, size); !set)
						{
							return set;
						}
						return Commit(std::move(document), "Fit Comment to Contents");
					}
					case VisualScriptCommentAction::Delete:
					{
						uint32_t const elements[] = {edit.Comment};
						Result<VisualScriptRemoval> removed = Editing::RemoveElements(document, m_Function, elements, nullptr, false);
						if (!removed)
						{
							return Error{removed.GetError()};
						}
						return Commit(std::move(document), "Delete Comment");
					}
					case VisualScriptCommentAction::DeleteWithContents:
					{
						std::vector<uint32_t> elements = GetCommentContents(edit.Comment);
						std::pair<size_t, size_t> const contents = CountElements(elements);
						elements.push_back(edit.Comment);
						VisualScriptNodePins const pins = MakePins(document);
						Result<VisualScriptRemoval> removed =
							Editing::RemoveElements(document, m_Function, elements, m_Context.Library != nullptr ? &pins : nullptr, true);
						if (!removed)
						{
							return Error{removed.GetError()};
						}
						std::vector<std::string> notes = removed.GetValue().Notes;
						std::string action = contents.first + contents.second == 0
						                         ? std::string("Delete Comment")
						                         : "Delete Comment and " + DescribeElements(contents.first, contents.second);
						return Commit(std::move(document), action, 0, {}, std::move(notes));
					}
				}
				return {};
			}

			Result<void> Resolve(VisualScriptAddNodeEdit const& edit)
			{
				if (!CanEdit())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				Result<uint32_t> const added = Editing::AddNode(document, m_Function, edit.Type, edit.Settings, edit.Position);
				if (!added)
				{
					return Error{added.GetError()};
				}
				VisualScriptNodeTemplate const* found = m_Context.Library != nullptr ? m_Context.Library->FindNode(edit.Type) : nullptr;
				VisualScriptGraph const* graph = FindGraph(document, m_Function);
				VisualScriptNode const* node = graph != nullptr ? FindNode(*graph, added.GetValue()) : nullptr;
				std::string const title = found != nullptr  ? found->Title
				                          : node != nullptr ? DeriveVisualScriptNodeTitle(*node, document)
				                                            : edit.Type;
				return Commit(std::move(document), "Add " + title, 0, {added.GetValue()});
			}

			// --- The node menu ---

			std::vector<VisualScriptMenuItem> BuildNodeMenu();

		private:
			// Half the size a Convert node is laid out at, so the one a wire's conversion adds stands on the wire's middle.
			static constexpr glm::vec2 ConvertHalfSize{64.0f, 24.0f};

			VisualScriptDocument const& GetDisplay() const { return m_Model.GetDisplayDocument(); }

			// The analysis the edits read: none without the scripting runtime (the model's last one is then kept for its pins).
			VisualScriptAnalysis const* GetAnalysis() const { return m_Context.Library != nullptr ? m_Model.GetAnalysis() : nullptr; }

			// --- Refusals ---

			// Whether the window may edit; otherwise the note saying why.
			bool CanEdit()
			{
				std::optional<std::string> note = GetReadOnlyNote();
				if (!note)
				{
					return true;
				}
				m_Result.Notes.push_back(std::move(*note));
				return false;
			}

			std::optional<std::string> GetReadOnlyNote() const
			{
				switch (m_Model.GetDocumentState())
				{
					case VisualScriptDocumentState::Editable:
						return std::nullopt;
					case VisualScriptDocumentState::ReadOnly:
						return std::string(VisualScriptRefusals::ReadOnlyMemory);
					case VisualScriptDocumentState::ReadOnlyFile:
						return fmt::format("{} is read-only on disk: edits cannot be saved.", GetFileName());
					case VisualScriptDocumentState::Missing:
						return fmt::format("The file is missing: {}.", m_Model.GetState().Path);
					case VisualScriptDocumentState::Unloadable:
						return fmt::format("The file could not be read: {}.", m_Model.GetState().Path);
					case VisualScriptDocumentState::Gone:
						return std::string("The visual script was deleted");
				}
				return std::string(VisualScriptRefusals::ReadOnlyMemory);
			}

			std::string GetFileName() const
			{
				std::string const path = m_Model.GetState().Path;
				return FileSystem::PathToUtf8(FileSystem::PathFromUtf8(path).filename());
			}

			// Whether the scripting runtime is there for an edit that needs node details; otherwise the degraded note.
			bool NeedsRuntime()
			{
				if (m_Context.Library != nullptr)
				{
					return true;
				}
				m_Result.NeedsRuntime = true;
				m_Result.Notes.emplace_back(VisualScriptRefusals::Degraded);
				return false;
			}

			// --- Commits ---

			// Prepares the edited document as every commit is prepared and makes it the result's step, unless it changes nothing.
			Result<void> Commit(VisualScriptDocument edited, std::string const& action, uint64_t mergeKey = 0,
			                    std::vector<uint32_t> select = {}, std::vector<std::string> notes = {})
			{
				Ref<VisualScriptDocument const> const& current = m_Model.GetStoredDocument();
				if (current == nullptr)
				{
					return Error{"the graph window has no document to edit"};
				}
				VisualScriptDocument const& stored = m_Context.Stored != nullptr ? *m_Context.Stored : *current;
				Result<VisualScriptCommitRequest> request =
					MakeVisualScriptCommitRequest(stored, std::move(edited), GetAnalysis(), action, m_Context.ScriptName, mergeKey);
				if (!request)
				{
					return Error{request.GetError()};
				}
				VisualScriptCommitRequest made = request.TakeValue();
				if (IsSameVisualScriptDocument(*made.Document, *current))
				{
					return {};
				}
				made.Select = std::move(select);
				notes.insert(notes.end(), std::make_move_iterator(made.Notes.begin()), std::make_move_iterator(made.Notes.end()));
				made.Notes = std::move(notes);
				m_Result.Commit = std::move(made);
				return {};
			}

			// --- The shown graph as the canvas knows it ---

			VisualScriptNodeVisual const* FindVisual(uint32_t node) const
			{
				std::optional<uint32_t> const index = m_Canvas.FindNode(node);
				if (!index)
				{
					return nullptr;
				}
				std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
				return &nodes[*index];
			}

			VisualScriptPinVisual const* FindPinVisual(uint32_t node, std::string_view pin, Direction direction) const
			{
				std::optional<uint32_t> const index = m_Canvas.FindNode(node);
				if (!index)
				{
					return nullptr;
				}
				std::optional<uint32_t> const pinIndex = m_Canvas.FindPin(*index, pin, direction);
				if (!pinIndex)
				{
					return nullptr;
				}
				std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
				return &pins[*pinIndex];
			}

			// The analysis' entry of a node the canvas draws from it (none for nodes changed since, and without the runtime).
			VisualScriptAnalyzedNode const* FindAnalyzed(uint32_t node) const
			{
				VisualScriptNodeVisual const* visual = FindVisual(node);
				VisualScriptAnalysis const* analysis = GetAnalysis();
				if (visual == nullptr || analysis == nullptr || !visual->HasFlag(VisualScriptNodeFlags::Analyzed))
				{
					return nullptr;
				}
				return analysis->FindNode(node);
			}

			VisualScriptNode const* FindDisplayNode(uint32_t node) const
			{
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				return graph != nullptr ? FindNode(*graph, node) : nullptr;
			}

			// The name the document stores for a pin the canvas names.
			std::string ToStored(uint32_t node, std::string_view pin, Direction direction) const
			{
				return Editing::ToStoredVisualScriptPin(FindAnalyzed(node), direction, pin);
			}

			VisualScriptLink ToStored(VisualScriptLink const& link) const
			{
				return {{link.From.Node, ToStored(link.From.Node, link.From.Pin, Direction::Out)},
				        {link.To.Node, ToStored(link.To.Node, link.To.Pin, Direction::In)}};
			}

			// A node's title in descriptions: the analysis', else the one derived from the document.
			std::string GetNodeTitle(uint32_t node) const
			{
				VisualScriptAnalysis const* analysis = GetAnalysis();
				VisualScriptAnalyzedNode const* analyzed = analysis != nullptr ? analysis->FindNode(node) : nullptr;
				if (analyzed != nullptr && !analyzed->Title.empty())
				{
					return analyzed->Title;
				}
				if (VisualScriptNode const* stored = FindDisplayNode(node))
				{
					return DeriveVisualScriptNodeTitle(*stored, GetDisplay());
				}
				return fmt::format("Node {}", node);
			}

			// "Delay" for one node, "3 Nodes" for more.
			std::string DescribeNodes(std::span<uint32_t const> nodes) const
			{
				return nodes.size() == 1 ? GetNodeTitle(nodes.front()) : CountNoun(nodes.size(), "Node", "Nodes");
			}

			// A pin's label as the canvas shows it, else its name.
			std::string GetPinLabel(uint32_t node, std::string_view pin, Direction direction) const
			{
				VisualScriptPinVisual const* visual = FindPinVisual(node, pin, direction);
				if (visual != nullptr && visual->Label.Length != 0)
				{
					return std::string(m_Canvas.GetText(visual->Label));
				}
				return std::string(pin);
			}

			// Where a pin's wires end, in canvas units; a node's middle for a pin the canvas does not show.
			glm::vec2 GetPinPoint(uint32_t node, std::string_view pin, Direction direction) const
			{
				VisualScriptNodeVisual const* visual = FindVisual(node);
				if (visual == nullptr)
				{
					return glm::vec2(0.0f);
				}
				if (VisualScriptPinVisual const* pinVisual = FindPinVisual(node, pin, direction))
				{
					return VisualScriptGeometry::GetPinAnchor(*visual, *pinVisual);
				}
				return visual->Position + visual->Size * 0.5f;
			}

			// The pins of a node as the operations take them: from the canvas (stored names, defaults from the analysis or the
			// library's template, anchors relative to the node), else from the template of the node's type in the document the
			// operation edits (a node added in the same edit).
			std::vector<VisualScriptNodePin> GetPins(VisualScriptDocument const& document, uint32_t node) const
			{
				std::vector<VisualScriptNodePin> rows;
				VisualScriptLibrary const* library = m_Context.Library;
				if (VisualScriptNodeVisual const* visual = FindVisual(node))
				{
					VisualScriptAnalyzedNode const* analyzed = FindAnalyzed(node);
					VisualScriptNode const* stored = FindDisplayNode(node);
					VisualScriptNodeTemplate const* found =
						library != nullptr && stored != nullptr ? library->FindNode(stored->Type) : nullptr;
					std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins(*visual);
					for (VisualScriptPinVisual const& pin : pins)
					{
						if (pin.HasFlag(VisualScriptPinFlags::Orphan))
						{
							continue;
						}
						std::string_view const name = m_Canvas.GetText(pin.Name);
						VisualScriptPin const* info = analyzed != nullptr ? analyzed->FindPin(name, pin.Direction)
						                              : found != nullptr  ? found->FindPin(name, pin.Direction)
						                                                  : nullptr;
						VisualScriptNodePin& row = rows.emplace_back();
						row.Name = Editing::ToStoredVisualScriptPin(analyzed, pin.Direction, name);
						row.Direction = pin.Direction;
						row.Type = pin.Type;
						row.Anchor = VisualScriptGeometry::GetPinAnchor(*visual, pin) - visual->Position;
						if (info != nullptr)
						{
							row.Default = info->Default;
							row.DefaultsToSelf = info->DefaultsToSelf;
						}
					}
					return rows;
				}
				VisualScriptGraph const* graph = FindGraph(document, m_Function);
				VisualScriptNode const* added = graph != nullptr ? FindNode(*graph, node) : nullptr;
				VisualScriptNodeTemplate const* found = library != nullptr && added != nullptr ? library->FindNode(added->Type) : nullptr;
				if (found == nullptr)
				{
					return rows;
				}
				for (VisualScriptPin const& pin : found->Pins)
				{
					VisualScriptNodePin& row = rows.emplace_back();
					row.Name = pin.Name;
					row.Direction = pin.Direction;
					row.Type = pin.Type;
					row.Default = pin.Default;
					row.DefaultsToSelf = pin.DefaultsToSelf;
				}
				return rows;
			}

			VisualScriptNodePins MakePins(VisualScriptDocument const& document) const
			{
				return [this, &document](uint32_t node)
				{
					return GetPins(document, node);
				};
			}

			VisualScriptNodeExtents MakeExtents() const
			{
				return [this](uint32_t node)
				{
					return m_Canvas.GetExtents(node);
				};
			}

			// The selection's nodes and comments of the shown graph, in selection order.
			void SplitSelection(std::span<uint32_t const> selection, std::vector<uint32_t>& nodes, std::vector<uint32_t>& comments) const
			{
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				if (graph == nullptr)
				{
					return;
				}
				for (uint32_t const element : selection)
				{
					if (FindNode(*graph, element) != nullptr)
					{
						nodes.push_back(element);
					}
					else if (FindComment(*graph, element) != nullptr)
					{
						comments.push_back(element);
					}
				}
			}

			// How many of the elements are nodes and comments of the shown graph.
			std::pair<size_t, size_t> CountElements(std::span<uint32_t const> elements) const
			{
				std::vector<uint32_t> nodes;
				std::vector<uint32_t> comments;
				SplitSelection(elements, nodes, comments);
				return {nodes.size(), comments.size()};
			}

			// The nodes and comment boxes entirely inside a comment box, as the canvas draws them.
			std::vector<uint32_t> GetCommentContents(uint32_t comment) const
			{
				std::vector<uint32_t> contents;
				std::optional<uint32_t> const index = m_Canvas.FindComment(comment);
				if (!index)
				{
					return contents;
				}
				std::span<VisualScriptCommentVisual const> const comments = m_Canvas.GetComments();
				VisualScriptRect const frame = comments[*index].Rect;
				for (VisualScriptNodeVisual const& node : m_Canvas.GetNodes())
				{
					if (HasRect(frame, VisualScriptGeometry::GetNodeRect(node)))
					{
						contents.push_back(node.ID);
					}
				}
				for (VisualScriptCommentVisual const& other : comments)
				{
					if (other.ID != comment && HasRect(frame, other.Rect))
					{
						contents.push_back(other.ID);
					}
				}
				return contents;
			}

			static std::string DescribeReplaced(uint32_t replaced)
			{
				return replaced == 0 ? std::string() : fmt::format(" (replaces {})", CountNoun(replaced, "1 link", "links"));
			}

			// Breaking links names the pin they share, else counts them: an Alt+click on a pin, on a wire, or a cut line.
			std::string DescribeDisconnect(std::span<VisualScriptLink const> links) const
			{
				if (links.size() == 1)
				{
					return "Break Link";
				}
				auto const shares = [links](auto const& end)
				{
					return std::all_of(links.begin(), links.end(),
					                   [&end, links](VisualScriptLink const& link)
					                   {
										   return end(link) == end(links.front());
									   });
				};
				std::string const count = CountNoun(links.size(), "Link", "Links");
				if (shares(
						[](VisualScriptLink const& link)
						{
							return link.From;
						}))
				{
					VisualScriptPinReference const& from = links.front().From;
					return fmt::format("Break {} of {}", count, GetPinLabel(from.Node, from.Pin, Direction::Out));
				}
				if (shares(
						[](VisualScriptLink const& link)
						{
							return link.To;
						}))
				{
					VisualScriptPinReference const& to = links.front().To;
					return fmt::format("Break {} of {}", count, GetPinLabel(to.Node, to.Pin, Direction::In));
				}
				return "Cut " + count;
			}

			// --- Drops ---

			Result<void> DropAsset(VisualScriptDropIntent const& intent)
			{
				if (!CanEdit())
				{
					return {};
				}
				AssetHandle const asset(intent.Payload);
				AssetType const type = AssetManager::GetAssetType(asset);
				if (type == AssetType::None)
				{
					m_Result.Notes.emplace_back("The dropped asset is not in the project");
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				if (intent.Pin)
				{
					// Onto an input whose type takes assets of its type: the input's literal.
					VisualScriptPinVisual const* pin = FindPinVisual(intent.Pin->Node, intent.Pin->Pin, intent.Direction);
					std::optional<Json> const literal =
						pin != nullptr && intent.Direction == Direction::In ? EncodeAsset(asset, type, pin->Type) : std::nullopt;
					std::string const label = GetPinLabel(intent.Pin->Node, intent.Pin->Pin, Direction::In);
					if (!literal)
					{
						m_Result.Notes.push_back(fmt::format("{} cannot take a {}", label, AssetTypeToString(type)));
						return {};
					}
					std::string const stored = ToStored(intent.Pin->Node, intent.Pin->Pin, Direction::In);
					if (Result<void> set = Editing::SetInput(document, intent.Pin->Node, stored, *literal); !set)
					{
						return set;
					}
					return Commit(std::move(document), fmt::format("Set {} on {}", label, GetNodeTitle(intent.Pin->Node)));
				}

				// Onto the canvas: a Literal of the library's type of such assets.
				if (!NeedsRuntime())
				{
					return {};
				}
				for (VisualScriptTypeInfo const& candidate : m_Context.Library->GetTypes())
				{
					if (!candidate.HasLiteral || candidate.AcceptedAssetType != type)
					{
						continue;
					}
					std::optional<Json> const literal = EncodeAsset(asset, type, candidate.ID);
					if (!literal)
					{
						continue;
					}
					Result<uint32_t> const added =
						Editing::AddNode(document, m_Function, "Literal", Json::object({{"Type", candidate.ID}, {"Value", *literal}}),
					                     SnapToGrid(intent.Position));
					if (!added)
					{
						return Error{added.GetError()};
					}
					return Commit(std::move(document), "Add " + candidate.Title, 0, {added.GetValue()});
				}
				m_Result.Notes.push_back(fmt::format("Visual scripts have no values of {} assets", AssetTypeToString(type)));
				return {};
			}

			// The literal of a type that refers to the asset; nothing when the type takes no such asset.
			static std::optional<Json> EncodeAsset(AssetHandle asset, AssetType type, std::string_view typeId)
			{
				std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(typeId);
				if (!exposable || (exposable->AcceptedAssetType != AssetType::None && exposable->AcceptedAssetType != type))
				{
					return std::nullopt;
				}
				switch (exposable->FieldType)
				{
					case ScriptFieldType::Asset:
						return EncodeVisualScriptLiteral(ScriptFieldValue::FromAsset(asset), typeId, {});
					case ScriptFieldType::Prefab:
						return type == AssetType::Prefab ? EncodeVisualScriptLiteral(ScriptFieldValue::FromPrefab(asset), typeId, {})
						                                 : std::nullopt;
					default:
						return std::nullopt;
				}
			}

			Result<void> DropEntities(VisualScriptDropIntent const& intent)
			{
				if (!CanEdit())
				{
					return {};
				}
				VisualScriptSceneContext const* scene = m_Context.Scene;
				if (scene == nullptr || !scene->EntityName)
				{
					return Error{"entity drops need the edited scene"};
				}
				if (scene->Playing)
				{
					m_Result.Notes.emplace_back(VisualScriptRefusals::EntitiesWhilePlaying);
					return {};
				}
				// The entities that still exist, each named.
				std::vector<std::pair<UUID, std::string>> entities;
				for (uint64_t const entity : intent.Entities)
				{
					if (std::optional<std::string> name = scene->EntityName(UUID(entity)))
					{
						entities.emplace_back(UUID(entity), std::move(*name));
					}
				}
				if (entities.empty())
				{
					m_Result.Notes.emplace_back("The dropped entities no longer exist");
					return {};
				}
				std::optional<std::string> target;
				if (intent.Pin)
				{
					VisualScriptPinVisual const* pin = FindPinVisual(intent.Pin->Node, intent.Pin->Pin, intent.Direction);
					if (pin == nullptr || intent.Direction != Direction::In || pin->Type != "Strada.Entity")
					{
						m_Result.Notes.emplace_back("Entities are dropped on Entity inputs or on the canvas");
						return {};
					}
					target = ToStored(intent.Pin->Node, intent.Pin->Pin, Direction::In);
				}

				VisualScriptDocument document = GetDisplay();
				ScriptFieldMap values = scene->ContextVariables;
				std::vector<uint32_t> created;
				std::vector<std::string> names;
				for (size_t index = 0; index < entities.size(); index++)
				{
					std::string name = Editing::MakeUniqueVisualScriptName(entities[index].second,
					                                                       [&document](std::string_view candidate)
					                                                       {
																			   return Editing::IsVariableNameTaken(document, candidate);
																		   });
					Result<uint32_t> const variable = Editing::AddVariable(document, name, "Strada.Entity");
					if (!variable)
					{
						return Error{variable.GetError()};
					}
					auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
					                                [id = variable.GetValue()](VisualScriptVariable const& candidate)
					                                {
														return candidate.ID == id;
													});
					VisualScriptVariable made = *found;
					made.Public = true;
					if (Result<bool> const set = Editing::SetVariable(document, variable.GetValue(), std::move(made)); !set)
					{
						return Error{set.GetError()};
					}
					glm::vec2 const position =
						SnapToGrid(intent.Position) + glm::vec2(0.0f, VisualScriptDropSpacing * static_cast<float>(index));
					Result<uint32_t> const get =
						Editing::AddNode(document, m_Function, "Variable.Get", Json::object({{"Variable", variable.GetValue()}}), position);
					if (!get)
					{
						return Error{get.GetError()};
					}
					if (target && index == 0)
					{
						VisualScriptLink const link{{get.GetValue(), "Value"}, {intent.Pin->Node, *target}};
						if (Result<uint32_t> const connected = Editing::Connect(document, m_Function, link, MakePins(document)); !connected)
						{
							return Error{connected.GetError()};
						}
					}
					created.push_back(get.GetValue());
					values[name] = ScriptFieldValue::FromEntity(entities[index].first);
					names.push_back(std::move(name));
				}

				std::vector<std::string> notes;
				if (scene->ContextEntity.IsValid())
				{
					Json patch = Json::object({{"Variables", JsonTraits<ScriptFieldMap>::ToJson(values)}});
					m_Result.ComponentEdits.push_back({scene->ContextEntity, "VisualScript", std::move(patch)});
				}
				else
				{
					std::string joined;
					for (std::string const& name : names)
					{
						joined += joined.empty() ? name : ", " + name;
					}
					notes.push_back(fmt::format("Set {} on the entities that run {}: Inspector > Variables", joined, m_Context.ScriptName));
				}
				std::string const action =
					entities.size() == 1 ? "Reference " + names.front() : fmt::format("Reference {} Entities", entities.size());
				Result<void> committed = Commit(std::move(document), action, 0, std::move(created), std::move(notes));
				if (!committed || !m_Result.Commit)
				{
					m_Result.ComponentEdits.clear();
				}
				return committed;
			}

			// --- Actions ---

			Result<void> Remove(Action action, std::span<uint32_t const> elements)
			{
				if (elements.empty())
				{
					return {};
				}
				if (action == Action::Cut)
				{
					m_Result.Clipboard = WriteVisualScriptClipboard(GetDisplay(), m_Function, elements, m_Context.Graph, m_Context.Library);
				}
				if (!CanEdit())
				{
					return {};
				}
				bool const bridge = action != Action::DeleteWithoutRelink;
				VisualScriptDocument document = GetDisplay();
				VisualScriptNodePins const pins = MakePins(document);
				Result<VisualScriptRemoval> removed =
					Editing::RemoveElements(document, m_Function, elements, m_Context.Library != nullptr ? &pins : nullptr, bridge);
				if (!removed)
				{
					return Error{removed.GetError()};
				}
				VisualScriptRemoval removal = removed.TakeValue();
				if (removal.Nodes + removal.Comments == 0)
				{
					m_Result.Notes = std::move(removal.Notes);
					return {};
				}
				if (bridge && m_Context.Library == nullptr && HasNodeBesidesReroutes(elements))
				{
					removal.Notes.emplace_back(VisualScriptRefusals::NotBridged);
				}
				std::string const verb = action == Action::Cut ? "Cut " : "Delete ";
				return Commit(std::move(document), verb + DescribeElements(removal.Nodes, removal.Comments), 0, {},
				              std::move(removal.Notes));
			}

			bool HasNodeBesidesReroutes(std::span<uint32_t const> elements) const
			{
				return std::any_of(elements.begin(), elements.end(),
				                   [this](uint32_t element)
				                   {
									   VisualScriptNode const* node = FindDisplayNode(element);
									   return node != nullptr && node->Type != "Reroute";
								   });
			}

			Result<void> Paste(glm::vec2 position)
			{
				if (!CanEdit())
				{
					return {};
				}
				if (!MayHoldVisualScriptClipboard(m_Context.ClipboardText))
				{
					m_Result.Notes.emplace_back(VisualScriptRefusals::NoClipboard);
					return {};
				}
				Result<VisualScriptClipboardContent> content = ReadVisualScriptClipboard(m_Context.ClipboardText);
				if (!content)
				{
					m_Result.Notes.emplace_back(VisualScriptRefusals::NoClipboard);
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				Result<VisualScriptPasteResult> pasted = PasteVisualScriptClipboard(
					document, m_Context.Graph, m_Function, content.GetValue(), SnapToGrid(position), m_Context.Library);
				if (!pasted)
				{
					// A limit of the format: what the user is told, not an editor bug.
					m_Result.Notes.push_back(pasted.GetError());
					return {};
				}
				return CommitPaste("Paste ", "Pasted ", std::move(document), pasted.TakeValue());
			}

			Result<void> Duplicate(std::span<uint32_t const> elements)
			{
				if (elements.empty() || !CanEdit())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				Result<VisualScriptPasteResult> duplicated =
					DuplicateVisualScriptElements(document, m_Context.Graph, m_Function, elements, m_Context.Library);
				if (!duplicated)
				{
					m_Result.Notes.push_back(duplicated.GetError());
					return {};
				}
				return CommitPaste("Duplicate ", "Duplicated ", std::move(document), duplicated.TakeValue());
			}

			// A paste's step selects what it added; its notes become one toast ("Pasted 5 nodes; added variable Score (int)").
			Result<void> CommitPaste(std::string_view verb, std::string_view done, VisualScriptDocument document,
			                         VisualScriptPasteResult pasted)
			{
				size_t const nodes = pasted.NodeCount;
				size_t const comments = pasted.Pasted.size() - std::min<size_t>(pasted.Pasted.size(), nodes);
				if (nodes + comments == 0)
				{
					m_Result.Notes = std::move(pasted.Notes);
					return {};
				}
				std::vector<std::string> notes;
				if (!pasted.Notes.empty())
				{
					std::string toast = fmt::format("{}{}", done, DescribeElementsInText(nodes, comments));
					for (std::string const& note : pasted.Notes)
					{
						toast += "; " + note;
					}
					notes.push_back(std::move(toast));
				}
				return Commit(std::move(document), fmt::format("{}{}", verb, DescribeElements(nodes, comments)), 0,
				              std::move(pasted.Pasted), std::move(notes));
			}

			Result<void> Rename(std::span<uint32_t const> selection)
			{
				if (selection.empty() || !CanEdit())
				{
					return {};
				}
				uint32_t const primary = selection.front();
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				if (graph == nullptr)
				{
					return {};
				}
				if (VisualScriptComment const* comment = FindComment(*graph, primary))
				{
					size_t const end = comment->Text.find('\n');
					m_Result.EditText =
						VisualScriptTextEditRequest{VisualScriptTextEditKind::CommentTitle, primary, comment->Text.substr(0, end)};
					return {};
				}
				if (std::optional<VisualScriptReveal> reveal = FindElementOf(primary))
				{
					reveal->Rename = true;
					m_Result.Reveal = reveal;
					return {};
				}
				m_Result.Notes.emplace_back("Select a comment, or a node of a variable, a function or a custom event, to rename it");
				return {};
			}

			// The variable, local, function or custom event a node stands for.
			std::optional<VisualScriptReveal> FindElementOf(uint32_t node) const
			{
				VisualScriptNode const* stored = FindDisplayNode(node);
				if (stored == nullptr)
				{
					return std::nullopt;
				}
				if (stored->Type == "Variable.Get" || stored->Type == "Variable.Set")
				{
					std::optional<uint32_t> const id = ReadId(stored->Settings, "Variable");
					if (!id)
					{
						return std::nullopt;
					}
					VisualScriptFunction const* function = FindFunction(GetDisplay(), m_Function);
					bool const local = function != nullptr && std::any_of(function->Locals.begin(), function->Locals.end(),
					                                                      [id](VisualScriptLocal const& candidate)
					                                                      {
																			  return candidate.ID == *id;
																		  });
					return VisualScriptReveal{local ? VisualScriptRevealKind::Local : VisualScriptRevealKind::Variable, *id};
				}
				if (stored->Type == "Function.Call")
				{
					std::optional<uint32_t> const id = ReadId(stored->Settings, "Function");
					return id ? std::optional<VisualScriptReveal>(VisualScriptReveal{VisualScriptRevealKind::Function, *id}) : std::nullopt;
				}
				if ((stored->Type == "Function.Entry" || stored->Type == "Function.Return") && m_Function != 0)
				{
					return VisualScriptReveal{VisualScriptRevealKind::Function, m_Function};
				}
				if (stored->Type == "Event.Custom")
				{
					return VisualScriptReveal{VisualScriptRevealKind::CustomEvent, node};
				}
				return std::nullopt;
			}

			Result<void> AddCommentBox(std::span<uint32_t const> elements, glm::vec2 position)
			{
				if (!CanEdit())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				Result<uint32_t> const comment = Editing::AddComment(document, m_Function, "Comment", elements, MakeExtents(),
				                                                     SnapToGrid(position), m_Context.CommentColor);
				if (!comment)
				{
					return Error{comment.GetError()};
				}
				Result<void> committed = Commit(std::move(document), "Add Comment Box", m_Context.MergeKey, {comment.GetValue()});
				if (committed && m_Result.Commit)
				{
					m_Result.EditText = VisualScriptTextEditRequest{VisualScriptTextEditKind::CommentTitle, comment.GetValue(), "Comment"};
				}
				return committed;
			}

			Result<void> Align(std::span<uint32_t const> elements, VisualScriptAlignment alignment, std::string_view edge)
			{
				if (!CanEdit())
				{
					return {};
				}
				if (elements.size() < 2)
				{
					m_Result.Notes.emplace_back("Select two or more nodes to align them");
					return {};
				}
				std::pair<size_t, size_t> const counts = CountElements(elements);
				VisualScriptDocument document = GetDisplay();
				if (Result<void> aligned = Editing::AlignElements(document, m_Function, elements, alignment, MakeExtents()); !aligned)
				{
					return aligned;
				}
				return Commit(std::move(document), fmt::format("Align {} {}", DescribeElements(counts.first, counts.second), edge));
			}

			Result<void> Distribute(std::span<uint32_t const> elements, VisualScriptDistribution distribution)
			{
				if (!CanEdit())
				{
					return {};
				}
				if (elements.size() < 3)
				{
					m_Result.Notes.emplace_back("Select three or more nodes to distribute them");
					return {};
				}
				std::pair<size_t, size_t> const counts = CountElements(elements);
				VisualScriptDocument document = GetDisplay();
				if (Result<void> spaced = Editing::DistributeElements(document, m_Function, elements, distribution, MakeExtents()); !spaced)
				{
					return spaced;
				}
				return Commit(std::move(document), "Distribute " + DescribeElements(counts.first, counts.second));
			}

			Result<void> Straighten(std::span<uint32_t const> nodes)
			{
				if (!CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				if (nodes.size() < 2)
				{
					m_Result.Notes.emplace_back("Select two or more linked nodes to straighten them");
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				Result<uint32_t> const moved = Editing::StraightenExec(document, m_Function, nodes, MakePins(document));
				if (!moved)
				{
					return Error{moved.GetError()};
				}
				if (moved.GetValue() == 0)
				{
					return {};
				}
				return Commit(std::move(document), "Straighten " + CountNoun(moved.GetValue(), "Node", "Nodes"));
			}

			Result<void> Arrange(std::span<uint32_t const> selected)
			{
				if (!CanEdit())
				{
					return {};
				}
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				if (graph == nullptr || graph->Nodes.empty())
				{
					return {};
				}
				// The selection with two nodes or more, else the whole graph.
				std::vector<uint32_t> nodes;
				if (selected.size() >= 2)
				{
					nodes.assign(selected.begin(), selected.end());
				}
				std::vector<std::pair<uint32_t, glm::vec2>> before;
				for (VisualScriptNode const& node : graph->Nodes)
				{
					if (node.Position && (nodes.empty() || std::find(nodes.begin(), nodes.end(), node.ID) != nodes.end()))
					{
						before.emplace_back(node.ID, *node.Position);
					}
				}
				size_t const count = nodes.empty() ? graph->Nodes.size() : nodes.size();
				VisualScriptDocument document = GetDisplay();
				if (Result<void> arranged = Editing::ArrangeNodes(document, m_Function, nodes, MakeExtents()); !arranged)
				{
					return arranged;
				}
				Result<void> committed = Commit(std::move(document), "Arrange " + CountNoun(count, "Node", "Nodes"));
				if (committed && m_Result.Commit)
				{
					m_Result.GlideFrom = std::move(before);
				}
				return committed;
			}

			Result<void> ToggleCollapsed(std::span<uint32_t const> nodes)
			{
				if (nodes.empty() || !CanEdit())
				{
					return {};
				}
				bool const collapsed = std::all_of(nodes.begin(), nodes.end(),
				                                   [this](uint32_t node)
				                                   {
													   VisualScriptNode const* stored = FindDisplayNode(node);
													   return stored != nullptr && stored->Collapsed;
												   });
				std::string const action = fmt::format("{} {}", collapsed ? "Expand" : "Collapse", DescribeNodes(nodes));
				VisualScriptDocument document = GetDisplay();
				if (Result<void> set = Editing::SetNodeCollapsed(document, nodes, !collapsed); !set)
				{
					return set;
				}
				return Commit(std::move(document), action);
			}

			Result<void> SetEnabledState(Action action, std::span<uint32_t const> selected)
			{
				if (selected.empty() || !CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				std::vector<uint32_t> nodes;
				for (uint32_t const node : selected)
				{
					if (IsEnableable(node))
					{
						nodes.push_back(node);
					}
				}
				if (nodes.empty())
				{
					m_Result.Notes.push_back(GetEnabledStateRefusal(selected));
					return {};
				}
				VisualScriptNodeEnabled state = VisualScriptNodeEnabled::Disabled;
				switch (action)
				{
					case Action::SetEnabled:
						state = VisualScriptNodeEnabled::Enabled;
						break;
					case Action::SetDevelopmentOnly:
						state = VisualScriptNodeEnabled::DevelopmentOnly;
						break;
					case Action::ToggleDisabled:
						// Disables them, or enables them when all are disabled.
						state = std::all_of(nodes.begin(), nodes.end(),
						                    [this](uint32_t node)
						                    {
												return FindDisplayNode(node)->Enabled == VisualScriptNodeEnabled::Disabled;
											})
						            ? VisualScriptNodeEnabled::Enabled
						            : VisualScriptNodeEnabled::Disabled;
						break;
					default:
						break;
				}
				std::string const what = DescribeNodes(nodes);
				std::string description = state == VisualScriptNodeEnabled::Enabled    ? "Enable " + what
				                          : state == VisualScriptNodeEnabled::Disabled ? "Disable " + what
				                                                                       : fmt::format("Set {} to Development Only", what);
				VisualScriptDocument document = GetDisplay();
				if (Result<void> set = Editing::SetNodeEnabled(document, nodes, state); !set)
				{
					return set;
				}
				return Commit(std::move(document), description);
			}

			Result<void> BreakNodeLinks(std::span<uint32_t const> nodes)
			{
				if (nodes.empty() || !CanEdit())
				{
					return {};
				}
				std::vector<VisualScriptLink> links = GetLinksOf(nodes);
				if (links.empty())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				if (Result<void> disconnected = Editing::Disconnect(document, m_Function, links); !disconnected)
				{
					return disconnected;
				}
				return Commit(std::move(document), "Break All Links of " + DescribeNodes(nodes));
			}

			std::vector<VisualScriptLink> GetLinksOf(std::span<uint32_t const> nodes) const
			{
				std::vector<VisualScriptLink> links;
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				if (graph == nullptr)
				{
					return links;
				}
				for (VisualScriptLink const& link : graph->Links)
				{
					if (std::any_of(nodes.begin(), nodes.end(),
					                [&link](uint32_t node)
					                {
										return IsLinkOf(link, node);
									}) &&
					    std::find(links.begin(), links.end(), link) == links.end())
					{
						links.push_back(link);
					}
				}
				return links;
			}

			Result<void> OpenNodeComment(std::span<uint32_t const> nodes)
			{
				if (nodes.empty() || !CanEdit())
				{
					return {};
				}
				VisualScriptNode const* node = FindDisplayNode(nodes.front());
				m_Result.EditText =
					VisualScriptTextEditRequest{VisualScriptTextEditKind::NodeComment, nodes.front(), node != nullptr ? node->Comment : ""};
				return {};
			}

			Result<void> RemoveNodeComments(std::span<uint32_t const> selected)
			{
				if (selected.empty() || !CanEdit())
				{
					return {};
				}
				std::vector<uint32_t> nodes;
				for (uint32_t const node : selected)
				{
					VisualScriptNode const* stored = FindDisplayNode(node);
					if (stored != nullptr && !stored->Comment.empty())
					{
						nodes.push_back(node);
					}
				}
				if (nodes.empty())
				{
					return {};
				}
				std::string const action = nodes.size() == 1 ? "Remove Comment of " + GetNodeTitle(nodes.front())
				                                             : fmt::format("Remove Comments of {} Nodes", nodes.size());
				VisualScriptDocument document = GetDisplay();
				if (Result<void> set = Editing::SetNodeComment(document, nodes, std::string()); !set)
				{
					return set;
				}
				return Commit(std::move(document), action);
			}

			Result<void> FindReferences(std::span<uint32_t const> nodes)
			{
				if (nodes.empty())
				{
					return {};
				}
				if (std::optional<VisualScriptReference> reference = GetReference(nodes.front()))
				{
					m_Result.FindReferences = std::move(reference);
					return {};
				}
				m_Result.Notes.push_back(fmt::format("{} refers to no variable, function or event", GetNodeTitle(nodes.front())));
				return {};
			}

			// What Find References looks for from a node: its variable or local, the function it calls (a function's entry:
			// that function's callers), its custom event.
			std::optional<VisualScriptReference> GetReference(uint32_t node) const
			{
				VisualScriptNode const* stored = FindDisplayNode(node);
				if (stored == nullptr)
				{
					return std::nullopt;
				}
				if (stored->Type == "Variable.Get" || stored->Type == "Variable.Set")
				{
					std::optional<uint32_t> const id = ReadId(stored->Settings, "Variable");
					return id ? std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Variable, *id, {}})
					          : std::nullopt;
				}
				if (stored->Type == "Function.Call")
				{
					std::optional<uint32_t> const id = ReadId(stored->Settings, "Function");
					return id ? std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Function, *id, {}})
					          : std::nullopt;
				}
				if (stored->Type == "Function.Entry" && m_Function != 0)
				{
					return VisualScriptReference{VisualScriptReference::Kind::Function, m_Function, {}};
				}
				if (stored->Type == "Event.Custom" || stored->Type == "Event.Send")
				{
					std::optional<std::string> name = ReadString(stored->Settings, "Name");
					return name ? std::optional<VisualScriptReference>(
									  VisualScriptReference{VisualScriptReference::Kind::CustomEvent, 0, std::move(*name)})
					            : std::nullopt;
				}
				return std::nullopt;
			}

			// What opening a node does (a double-click on its header, the menu's first item): a call shows its function, a sender
			// goes to its event, a variable's node selects the variable in My Script, the others show the node in Details. The
			// label of the menu's item; nothing for the nodes Details shows.
			std::optional<std::string> GetOpenLabel(uint32_t node) const
			{
				VisualScriptNode const* stored = FindDisplayNode(node);
				if (stored == nullptr)
				{
					return std::nullopt;
				}
				if (stored->Type == "Function.Call")
				{
					std::optional<uint32_t> const id = ReadId(stored->Settings, "Function");
					VisualScriptFunction const* function = id ? FindFunction(GetDisplay(), *id) : nullptr;
					return function != nullptr ? std::optional<std::string>("Open " + function->Name) : std::nullopt;
				}
				if (stored->Type == "Event.Send")
				{
					std::optional<std::string> const name = ReadString(stored->Settings, "Name");
					return name && FindCustomEvent(*name) ? std::optional<std::string>("Go to Event " + *name) : std::nullopt;
				}
				if (std::optional<VisualScriptReveal> const element = FindElementOf(node);
				    element && (element->Kind == VisualScriptRevealKind::Variable || element->Kind == VisualScriptRevealKind::Local))
				{
					return "Select Variable " + GetVariableName(*element);
				}
				return std::nullopt;
			}

			std::string GetVariableName(VisualScriptReveal const& element) const
			{
				VisualScriptDocument const& document = GetDisplay();
				if (element.Kind == VisualScriptRevealKind::Local)
				{
					if (VisualScriptFunction const* function = FindFunction(document, m_Function))
					{
						for (VisualScriptLocal const& local : function->Locals)
						{
							if (local.ID == element.ID)
							{
								return local.Name;
							}
						}
					}
				}
				for (VisualScriptVariable const& variable : document.Variables)
				{
					if (variable.ID == element.ID)
					{
						return variable.Name;
					}
				}
				return std::to_string(element.ID);
			}

			// The event graph's Event.Custom of a name.
			std::optional<uint32_t> FindCustomEvent(std::string_view name) const
			{
				for (VisualScriptNode const& node : GetDisplay().Graph.Nodes)
				{
					if (node.Type == "Event.Custom" && ReadString(node.Settings, "Name") == name)
					{
						return node.ID;
					}
				}
				return std::nullopt;
			}

			Result<void> OpenNode(std::span<uint32_t const> nodes)
			{
				if (nodes.empty())
				{
					return {};
				}
				uint32_t const node = nodes.front();
				VisualScriptNode const* stored = FindDisplayNode(node);
				if (stored == nullptr)
				{
					return {};
				}
				if (stored->Type == "Function.Call")
				{
					std::optional<uint32_t> const id = ReadId(stored->Settings, "Function");
					if (id && FindFunction(GetDisplay(), *id) != nullptr)
					{
						m_Result.ShowFunction = *id;
						return {};
					}
				}
				if (stored->Type == "Event.Send")
				{
					std::optional<std::string> const name = ReadString(stored->Settings, "Name");
					if (std::optional<uint32_t> const event = name ? FindCustomEvent(*name) : std::nullopt)
					{
						VisualScriptOpenRequest request;
						request.Graph = m_Context.Graph;
						request.Function = 0;
						request.Select = {*event};
						request.Frame = VisualScriptFrame::Selection;
						m_Result.Open = std::move(request);
						return {};
					}
				}
				if (std::optional<VisualScriptReveal> const element = FindElementOf(node);
				    element && (element->Kind == VisualScriptRevealKind::Variable || element->Kind == VisualScriptRevealKind::Local))
				{
					m_Result.Reveal = element;
					return {};
				}
				m_Result.Reveal = VisualScriptReveal{VisualScriptRevealKind::Node, node};
				return {};
			}

			Result<void> UpdateNodes()
			{
				if (!CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				// The commit stores every node the analysis resolved inexactly under its canonical type, and places the nodes
				// without a position: the display document itself is the edit.
				Result<void> committed = Commit(GetDisplay(), "Update Nodes");
				if (committed && !m_Result.Commit)
				{
					m_Result.Notes.emplace_back("Every node is up to date");
				}
				return committed;
			}

			Result<void> Nudge(std::span<uint32_t const> elements, glm::vec2 offset)
			{
				if (elements.empty() || !CanEdit())
				{
					return {};
				}
				std::pair<size_t, size_t> const counts = CountElements(elements);
				VisualScriptDocument document = GetDisplay();
				if (Result<void> moved = Editing::MoveElements(document, m_Function, elements, offset); !moved)
				{
					return moved;
				}
				return Commit(std::move(document), "Nudge " + DescribeElements(counts.first, counts.second), m_Context.MergeKey);
			}

			// --- Pins ---

			// The links of a pin as the document stores them; of a collapsed anchor, those of every pin of its side.
			std::vector<VisualScriptLink> GetPinLinks(VisualScriptPinEdit const& edit) const
			{
				std::vector<VisualScriptLink> links;
				VisualScriptGraph const* graph = FindGraph(GetDisplay(), m_Function);
				if (graph == nullptr)
				{
					return links;
				}
				std::string const pin = ToStored(edit.Pin.Node, edit.Pin.Pin, edit.Direction);
				for (VisualScriptLink const& link : graph->Links)
				{
					VisualScriptPinReference const& end = edit.Direction == Direction::Out ? link.From : link.To;
					if (end.Node == edit.Pin.Node && (edit.WholeSide || end.Pin == pin))
					{
						links.push_back(link);
					}
				}
				return links;
			}

			Result<void> BreakPinLinks(VisualScriptPinEdit const& edit)
			{
				if (!CanEdit())
				{
					return {};
				}
				std::vector<VisualScriptLink> const links = GetPinLinks(edit);
				if (links.empty())
				{
					return {};
				}
				VisualScriptDocument document = GetDisplay();
				if (Result<void> disconnected = Editing::Disconnect(document, m_Function, links); !disconnected)
				{
					return disconnected;
				}
				std::string const owner =
					edit.WholeSide ? GetNodeTitle(edit.Pin.Node) : GetPinLabel(edit.Pin.Node, edit.Pin.Pin, edit.Direction);
				std::string const action =
					links.size() == 1 ? std::string("Break Link") : fmt::format("Break {} Links of {}", links.size(), owner);
				return Commit(std::move(document), action);
			}

			Result<void> SplitPin(VisualScriptPinEdit const& edit)
			{
				if (!CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				bool const split = edit.Action == VisualScriptPinAction::Split;
				std::string label = GetPinLabel(edit.Pin.Node, edit.Pin.Pin, edit.Direction);
				std::string path = ToStored(edit.Pin.Node, edit.Pin.Pin, edit.Direction);
				if (!split)
				{
					// Recombine is picked on a field pin ("Force.X"): what recombines is the split pin it is a field of, the
					// innermost one when splits nest ("Hit.Point" for "Hit.Point.X").
					std::optional<std::string> entry = FindSplitOf(edit.Pin.Node, path, edit.Direction);
					if (!entry)
					{
						return MakeError("{} of node {} is no field of a split pin", path, edit.Pin.Node);
					}
					path = std::move(*entry);
					label = path;
					std::replace(label.begin(), label.end(), '.', ' ');
				}
				VisualScriptDocument document = GetDisplay();
				VisualScriptNodePins const pins = MakePins(document);
				Result<uint32_t> const removed =
					split ? Editing::SplitPin(document, edit.Pin.Node, edit.Direction, path, pins, *m_Context.Library)
						  : Editing::RecombinePin(document, edit.Pin.Node, edit.Direction, path, pins, *m_Context.Library);
				if (!removed)
				{
					return Error{removed.GetError()};
				}
				std::string const removes = removed.GetValue() == 0
				                                ? std::string()
				                                : fmt::format(" (removes {})", CountNoun(removed.GetValue(), "1 link", "links"));
				return Commit(std::move(document), fmt::format("{} {}{}", split ? "Split" : "Recombine", label, removes));
			}

			// The entry of a node's Split list a field pin (by its stored path) belongs to: the longest one it lies under.
			std::optional<std::string> FindSplitOf(uint32_t node, std::string_view field, Direction direction) const
			{
				VisualScriptNode const* stored = FindDisplayNode(node);
				if (stored == nullptr)
				{
					return std::nullopt;
				}
				std::vector<std::string> const& entries = direction == Direction::In ? stored->Split.Inputs : stored->Split.Outputs;
				std::optional<std::string> found;
				for (std::string const& entry : entries)
				{
					bool const under = field.size() > entry.size() && field.starts_with(entry) && field[entry.size()] == '.';
					if (under && (!found || entry.size() > found->size()))
					{
						found = entry;
					}
				}
				return found;
			}

			Result<void> Promote(VisualScriptPinEdit const& edit)
			{
				if (!CanEdit() || !NeedsRuntime())
				{
					return {};
				}
				std::string const label = GetPinLabel(edit.Pin.Node, edit.Pin.Pin, edit.Direction);
				VisualScriptPinReference const pin{edit.Pin.Node, ToStored(edit.Pin.Node, edit.Pin.Pin, edit.Direction)};
				VisualScriptDocument document = GetDisplay();
				if (edit.Action == VisualScriptPinAction::PromoteToVariable)
				{
					Result<uint32_t> const variable =
						Editing::PromoteToVariable(document, m_Function, pin, edit.Direction, MakePins(document));
					if (!variable)
					{
						return Error{variable.GetError()};
					}
					Result<void> committed = Commit(std::move(document), fmt::format("Promote {} to Variable", label));
					if (committed && m_Result.Commit)
					{
						m_Result.Reveal = VisualScriptReveal{VisualScriptRevealKind::Variable, variable.GetValue(), true};
					}
					return committed;
				}
				if (m_Function == 0)
				{
					m_Result.Notes.emplace_back("Function inputs are added in a function's graph");
					return {};
				}
				if (Result<void> promoted = Editing::PromoteToFunctionInput(document, m_Function, pin, MakePins(document)); !promoted)
				{
					return promoted;
				}
				return Commit(std::move(document), fmt::format("Promote {} to Function Input", label));
			}

			// --- Enabled state ---

			bool IsEnableable(uint32_t node) const
			{
				VisualScriptNodeVisual const* visual = FindVisual(node);
				VisualScriptNode const* stored = FindDisplayNode(node);
				if (visual == nullptr || stored == nullptr || stored->Type == "Function.Entry" || stored->Type == "Function.Return")
				{
					return false;
				}
				if (visual->Kind == VisualScriptNodeKind::Event && stored->Type != "Event.Send")
				{
					return true;
				}
				if (visual->HasFlag(VisualScriptNodeFlags::Pure))
				{
					return false;
				}
				bool execIn = false;
				bool execOut = false;
				for (VisualScriptPinVisual const& pin : m_Canvas.GetPins(*visual))
				{
					if (pin.Type == VisualScriptExecType && !pin.HasFlag(VisualScriptPinFlags::Orphan))
					{
						(pin.Direction == Direction::In ? execIn : execOut) = true;
					}
				}
				return execIn && execOut;
			}

			// Why none of the nodes can be disabled.
			std::string GetEnabledStateRefusal(std::span<uint32_t const> nodes) const
			{
				bool const pure = std::any_of(nodes.begin(), nodes.end(),
				                              [this](uint32_t node)
				                              {
												  VisualScriptNodeVisual const* visual = FindVisual(node);
												  return visual != nullptr && visual->HasFlag(VisualScriptNodeFlags::Pure);
											  });
				if (pure)
				{
					return std::string(VisualScriptRefusals::PureNodes);
				}
				bool const ends =
					std::all_of(nodes.begin(), nodes.end(),
				                [this](uint32_t node)
				                {
									VisualScriptNode const* stored = FindDisplayNode(node);
									return stored != nullptr && (stored->Type == "Function.Entry" || stored->Type == "Function.Return");
								});
				return ends ? std::string("A function's Entry and Return always run")
				            : std::string("Only nodes the flow passes through can be disabled");
			}

			VisualScriptEditContext const& m_Context;
			VisualScriptEditorModel const& m_Model;
			VisualScriptCanvasModel const& m_Canvas;
			uint32_t m_Function = 0;
			VisualScriptControllerResult m_Result;
		};

		std::vector<VisualScriptMenuItem> Controller::BuildNodeMenu()
		{
			std::vector<VisualScriptMenuItem> items;
			std::vector<uint32_t> nodes;
			std::vector<uint32_t> comments;
			SplitSelection(m_Model.GetSelection(), nodes, comments);
			if (nodes.empty())
			{
				return items;
			}
			std::vector<uint32_t> elements = nodes;
			elements.insert(elements.end(), comments.begin(), comments.end());
			uint32_t const primary = nodes.front();
			std::optional<std::string> const readOnly = GetReadOnlyNote();
			bool const runtime = m_Context.Library != nullptr;
			std::string const counted = nodes.size() == 1 ? std::string() : " " + CountNoun(nodes.size(), "Node", "Nodes");
			bool group = false;

			auto const add = [&](Action action, std::string label, std::string_view submenu = {})
			{
				VisualScriptMenuItem& item = items.emplace_back();
				item.Action = action;
				item.Label = std::move(label);
				item.Submenu = submenu;
				item.SeparatorBefore = group && items.size() > 1;
				group = false;
				return &item;
			};
			auto const disable = [](VisualScriptMenuItem* item, std::string reason)
			{
				if (item->Enabled)
				{
					item->Enabled = false;
					item->Reason = std::move(reason);
				}
			};
			// An edit: disabled in read-only states, and without the runtime when it needs node details.
			auto const edit = [&](Action action, std::string label, bool needsRuntime = false, std::string_view submenu = {})
			{
				VisualScriptMenuItem* item = add(action, std::move(label), submenu);
				if (readOnly)
				{
					disable(item, *readOnly);
				}
				else if (needsRuntime && !runtime)
				{
					disable(item, std::string(VisualScriptRefusals::Degraded));
				}
				return item;
			};

			// Open.
			if (std::optional<std::string> label = GetOpenLabel(primary))
			{
				add(Action::OpenNode, std::move(*label));
			}

			// Edit.
			group = true;
			edit(Action::Cut, "Cut" + counted);
			add(Action::Copy, "Copy" + counted);
			edit(Action::Duplicate, "Duplicate" + counted);
			edit(Action::Delete, "Delete" + counted);
			edit(Action::DeleteWithoutRelink, "Delete Without Relinking");

			// Links.
			group = true;
			VisualScriptMenuItem* breakLinks = edit(Action::BreakNodeLinks, "Break All Links of " + DescribeNodes(nodes));
			if (GetLinksOf(nodes).empty())
			{
				disable(breakLinks, nodes.size() == 1 ? GetNodeTitle(primary) + " has no links" : std::string("The nodes have no links"));
			}

			// Find.
			group = true;
			VisualScriptNode const* primaryNode = FindDisplayNode(primary);
			bool const entry = primaryNode != nullptr && primaryNode->Type == "Function.Entry";
			VisualScriptMenuItem* find = add(Action::FindReferences, entry ? "Find Callers" : "Find References");
			if (!GetReference(primary))
			{
				disable(find, GetNodeTitle(primary) + " refers to no variable, function or event");
			}

			// Node.
			group = true;
			bool const commented = primaryNode != nullptr && !primaryNode->Comment.empty();
			edit(commented ? Action::EditNodeComment : Action::AddNodeComment, commented ? "Edit Node Comment..." : "Add Node Comment...");
			size_t const withComments = static_cast<size_t>(std::count_if(nodes.begin(), nodes.end(),
			                                                              [this](uint32_t node)
			                                                              {
																			  VisualScriptNode const* stored = FindDisplayNode(node);
																			  return stored != nullptr && !stored->Comment.empty();
																		  }));
			VisualScriptMenuItem* removeComment = edit(
				Action::RemoveNodeComment, withComments > 1 ? fmt::format("Remove {} Node Comments", withComments) : "Remove Node Comment");
			if (withComments == 0)
			{
				disable(removeComment, nodes.size() == 1 ? GetNodeTitle(primary) + " has no node comment"
				                                         : std::string("The nodes have no node comments"));
			}
			bool const collapsed = std::all_of(nodes.begin(), nodes.end(),
			                                   [this](uint32_t node)
			                                   {
												   VisualScriptNode const* stored = FindDisplayNode(node);
												   return stored != nullptr && stored->Collapsed;
											   });
			edit(Action::ToggleCollapsed, (collapsed ? "Expand" : "Collapse") + counted);
			bool const functionEnds =
				std::all_of(nodes.begin(), nodes.end(),
			                [this](uint32_t node)
			                {
								VisualScriptNode const* stored = FindDisplayNode(node);
								return stored != nullptr && (stored->Type == "Function.Entry" || stored->Type == "Function.Return");
							});
			if (!functionEnds)
			{
				// The submenu's radio items: the state every node that can be disabled shares is chosen.
				std::vector<uint32_t> enableable;
				for (uint32_t const node : nodes)
				{
					if (IsEnableable(node))
					{
						enableable.push_back(node);
					}
				}
				struct State
				{
					Action StateAction;
					std::string_view Label;
					VisualScriptNodeEnabled Value;
				};
				constexpr State states[] = {{Action::SetEnabled, "Enabled", VisualScriptNodeEnabled::Enabled},
				                            {Action::SetDisabled, "Disabled", VisualScriptNodeEnabled::Disabled},
				                            {Action::SetDevelopmentOnly, "Development Only", VisualScriptNodeEnabled::DevelopmentOnly}};
				for (State const& state : states)
				{
					VisualScriptMenuItem* item = edit(state.StateAction, std::string(state.Label), true, "Enabled State");
					item->Checked = !enableable.empty() && std::all_of(enableable.begin(), enableable.end(),
					                                                   [this, &state](uint32_t node)
					                                                   {
																		   return FindDisplayNode(node)->Enabled == state.Value;
																	   });
					if (enableable.empty())
					{
						disable(item, GetEnabledStateRefusal(nodes));
					}
				}
			}

			// Arrange.
			group = true;
			edit(Action::AddCommentBox, "Comment Selection");
			struct Alignment
			{
				Action AlignAction;
				std::string_view Label;
			};
			constexpr Alignment alignments[] = {{Action::AlignLeft, "Left"},       {Action::AlignRight, "Right"},
			                                    {Action::AlignTop, "Top"},         {Action::AlignBottom, "Bottom"},
			                                    {Action::AlignCenters, "Centers"}, {Action::AlignMiddles, "Middles"}};
			for (Alignment const& alignment : alignments)
			{
				VisualScriptMenuItem* item = edit(alignment.AlignAction, std::string(alignment.Label), false, "Align");
				if (elements.size() < 2)
				{
					disable(item, "Select two or more nodes to align them");
				}
			}
			for (auto const& [action, label] :
			     {std::pair{Action::DistributeHorizontally, "Horizontally"}, std::pair{Action::DistributeVertically, "Vertically"}})
			{
				VisualScriptMenuItem* item = edit(action, label, false, "Distribute");
				if (elements.size() < 3)
				{
					disable(item, "Select three or more nodes to distribute them");
				}
			}
			VisualScriptMenuItem* straighten = edit(Action::Straighten, "Straighten" + counted, true);
			if (nodes.size() < 2)
			{
				disable(straighten, "Select two or more linked nodes to straighten them");
			}
			VisualScriptMenuItem* arrange = edit(Action::Arrange, "Arrange Selection" + counted);
			if (nodes.size() < 2)
			{
				disable(arrange, "Select two or more nodes to arrange them");
			}

			// Fix.
			VisualScriptAnalysis const* analysis = GetAnalysis();
			bool const updatable =
				analysis != nullptr && std::any_of(analysis->Diagnostics.begin(), analysis->Diagnostics.end(),
			                                       [&nodes](VisualScriptDiagnostic const& diagnostic)
			                                       {
													   return diagnostic.Code == "NodeUpdated" && diagnostic.Node &&
				                                              std::find(nodes.begin(), nodes.end(), *diagnostic.Node) != nodes.end();
												   });
			if (updatable)
			{
				group = true;
				edit(Action::UpdateNode, "Update Node", true);
			}
			return items;
		}
	}

	Result<VisualScriptControllerResult> ResolveVisualScriptIntent(VisualScriptEditorIntent const& intent,
	                                                               VisualScriptEditContext const& context)
	{
		Controller controller(context);
		Result<void> const resolved = std::visit(
			[&controller](auto const& typed)
			{
				return controller.Resolve(typed);
			},
			intent);
		if (!resolved)
		{
			return Error{resolved.GetError()};
		}
		return controller.TakeResult();
	}

	Result<VisualScriptControllerResult> ResolveVisualScriptEdit(VisualScriptCanvasEdit const& edit, VisualScriptEditContext const& context)
	{
		Controller controller(context);
		Result<void> const resolved = std::visit(
			[&controller](auto const& typed)
			{
				return controller.Resolve(typed);
			},
			edit);
		if (!resolved)
		{
			return Error{resolved.GetError()};
		}
		return controller.TakeResult();
	}

	std::vector<VisualScriptMenuItem> BuildVisualScriptNodeMenu(VisualScriptEditContext const& context)
	{
		Controller controller(context);
		return controller.BuildNodeMenu();
	}
}
