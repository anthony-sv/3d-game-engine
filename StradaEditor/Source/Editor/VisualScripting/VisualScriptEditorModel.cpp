#include "Editor/VisualScripting/VisualScriptEditorModel.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <tuple>

namespace Strada
{
	namespace
	{
		namespace Metrics = VisualScriptCanvasMetrics;
		using PackedIndex = std::pair<uint32_t, uint64_t>;
		using Action = VisualScriptEditorAction;
		using Kind = VisualScriptHitKind;

		// A pin a wire drag has not classed yet (it was not on the screen).
		constexpr uint8_t Unclassified = 0xFF;
		// The longest step auto-pan and the minimap's fade take: a stalled frame does not throw the view.
		constexpr double MaxFrameSeconds = 0.1;

		// Adds the milliseconds from its construction to its destruction to a phase of the Sync timings.
		class PhaseTimer
		{
		public:
			explicit PhaseTimer(double& milliseconds)
				: m_Milliseconds(milliseconds),
				  m_Start(std::chrono::steady_clock::now())
			{
			}
			~PhaseTimer()
			{
				m_Milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_Start).count();
			}
			PhaseTimer(PhaseTimer const&) = delete;
			PhaseTimer& operator=(PhaseTimer const&) = delete;

		private:
			double& m_Milliseconds;
			std::chrono::steady_clock::time_point m_Start;
		};

		// The document's graphs by position: 0 is the event graph, i + 1 the i-th function's.
		VisualScriptGraph const* GetGraphAt(VisualScriptDocument const& document, size_t graph)
		{
			if (graph == 0)
			{
				return &document.Graph;
			}
			return graph <= document.Functions.size() ? &document.Functions[graph - 1].Graph : nullptr;
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

		bool HasUnplacedNodes(VisualScriptDocument const& document)
		{
			for (size_t graph = 0; graph <= document.Functions.size(); graph++)
			{
				for (VisualScriptNode const& node : GetGraphAt(document, graph)->Nodes)
				{
					if (!node.Position)
					{
						return true;
					}
				}
			}
			return false;
		}

		// Every node of a document by ID, as (graph position << 32 | node index); the first of repeated IDs first.
		void IndexNodes(VisualScriptDocument const& document, std::vector<PackedIndex>& out)
		{
			out.clear();
			for (size_t graph = 0; graph <= document.Functions.size(); graph++)
			{
				std::vector<VisualScriptNode> const& nodes = GetGraphAt(document, graph)->Nodes;
				for (size_t index = 0; index < nodes.size(); index++)
				{
					out.emplace_back(nodes[index].ID, (static_cast<uint64_t>(graph) << 32) | index);
				}
			}
			std::stable_sort(out.begin(), out.end(),
			                 [](PackedIndex const& a, PackedIndex const& b)
			                 {
								 return a.first < b.first;
							 });
		}

		template<typename T>
		std::optional<T> FindByID(std::vector<std::pair<uint32_t, T>> const& sorted, uint32_t id)
		{
			auto const found = std::lower_bound(sorted.begin(), sorted.end(), id,
			                                    [](std::pair<uint32_t, T> const& entry, uint32_t value)
			                                    {
													return entry.first < value;
												});
			if (found == sorted.end() || found->first != id)
			{
				return std::nullopt;
			}
			return found->second;
		}

		VisualScriptNode const* GetPackedNode(VisualScriptDocument const& document, uint64_t packed,
		                                      VisualScriptGraph const** graph = nullptr)
		{
			VisualScriptGraph const* owner = GetGraphAt(document, static_cast<size_t>(packed >> 32));
			size_t const index = static_cast<size_t>(packed & 0xFFFFFFFFull);
			if (owner == nullptr || index >= owner->Nodes.size())
			{
				return nullptr;
			}
			if (graph != nullptr)
			{
				*graph = owner;
			}
			return &owner->Nodes[index];
		}

		// An asset as value chips name it: its file's name (a memory asset's name), marked when its file is missing.
		std::string DescribeAsset(AssetHandle handle)
		{
			if (!AssetManager::IsInitialized())
			{
				return handle.ToString();
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
			if (!metadata)
			{
				return "Unknown asset " + handle.ToString();
			}
			std::string name = metadata->GetDisplayName();
			return AssetManager::IsMissing(handle) ? name + " (missing)" : name;
		}

		// Whether an element is stored identically, values as they are written included (operator== takes 1 and 1.0 as equal).
		bool IsSameElement(VisualScriptNode const& a, VisualScriptNode const& b)
		{
			return a == b && HaveSameVisualScriptProgram(a, b);
		}

		// Comments hold no values written in JSON.
		bool IsSameElement(VisualScriptComment const& a, VisualScriptComment const& b)
		{
			return a == b;
		}

		template<typename T>
		size_t GetCapacity(std::vector<T> const& vector)
		{
			return vector.capacity() * sizeof(T);
		}

		VisualScriptRect MakeRect(glm::vec2 a, glm::vec2 b)
		{
			return {glm::min(a, b), glm::max(a, b)};
		}

		bool ContainsRect(VisualScriptRect const& outer, VisualScriptRect const& inner)
		{
			return inner.Min.x >= outer.Min.x && inner.Min.y >= outer.Min.y && inner.Max.x <= outer.Max.x && inner.Max.y <= outer.Max.y;
		}

		float SnapToGrid(float value)
		{
			return std::round(value / Metrics::GridSize) * Metrics::GridSize;
		}

		glm::vec2 SnapToGrid(glm::vec2 point)
		{
			return glm::vec2(SnapToGrid(point.x), SnapToGrid(point.y));
		}

		bool IsFinite(glm::vec2 point)
		{
			return std::isfinite(point.x) && std::isfinite(point.y);
		}

		bool LinkLess(VisualScriptLink const& a, VisualScriptLink const& b)
		{
			return std::tie(a.From.Node, a.From.Pin, a.To.Node, a.To.Pin) < std::tie(b.From.Node, b.From.Pin, b.To.Node, b.To.Pin);
		}

		bool IsCollapsedBox(VisualScriptNodeVisual const& node)
		{
			return node.HasFlag(VisualScriptNodeFlags::Collapsed) && node.Shape == VisualScriptNodeShape::Standard;
		}

		bool IsNodeHit(VisualScriptHitKind kind)
		{
			return kind == Kind::Pin || kind == Kind::Badge || kind == Kind::NodeHeader || kind == Kind::Node || kind == Kind::Reroute;
		}

		// A setting of a node that names a function by ID (Function.Call's Function).
		std::optional<uint32_t> ReadIdSetting(VisualScriptNode const& node, char const* name)
		{
			if (!node.Settings.is_object() || !node.Settings.contains(name))
			{
				return std::nullopt;
			}
			Json const& value = node.Settings.at(name);
			if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > static_cast<int64_t>(UINT32_MAX))
			{
				return std::nullopt;
			}
			return static_cast<uint32_t>(value.get<int64_t>());
		}

		std::optional<std::string> ReadStringSetting(VisualScriptNode const& node, char const* name)
		{
			if (!node.Settings.is_object() || !node.Settings.contains(name) || !node.Settings.at(name).is_string())
			{
				return std::nullopt;
			}
			return node.Settings.at(name).get<std::string>();
		}

		bool IsNumeric(VisualScriptTypeInfo const* type)
		{
			return type != nullptr && (type->Kind == VisualScriptTypeKind::Integer || type->Kind == VisualScriptTypeKind::Float);
		}

		// The type a Convert node bridges an output and an input with when they cannot connect: the palette's two Convert
		// families, any value to string and a number to another number type, which the scripting runtime always converts.
		std::optional<std::string> FindConvertBridge(VisualScriptLibrary const& library, VisualScriptPin const& output,
		                                             VisualScriptPin const& input)
		{
			if (library.CanConnect(output, input) || output.Type == VisualScriptExecType || input.Type == VisualScriptExecType ||
			    output.Type == VisualScriptWildcardType || input.Type == VisualScriptWildcardType || output.CarriesExec ||
			    input.CarriesExec)
			{
				return std::nullopt;
			}
			if (input.Type == "string")
			{
				return input.Type;
			}
			if (IsNumeric(library.FindType(output.Type)) && IsNumeric(library.FindType(input.Type)))
			{
				return input.Type;
			}
			return std::nullopt;
		}

		bool IsExecPin(VisualScriptPin const* pin)
		{
			return pin != nullptr && (pin->Type == VisualScriptExecType || pin->CarriesExec);
		}

		uint8_t GetBookmarkSlot(Action action, Action first)
		{
			return static_cast<uint8_t>(static_cast<int>(action) - static_cast<int>(first) + 1);
		}

		bool IsInRange(Action action, Action first, Action last)
		{
			return static_cast<int>(action) >= static_cast<int>(first) && static_cast<int>(action) <= static_cast<int>(last);
		}

		// What typed characters give a palette query: ImGui queues the Space key's own space (and its repeats while it is held)
		// with the letters, so spaces and control characters never start one. Bytes of UTF-8 sequences are kept.
		std::string GetQueryCharacters(std::string_view typed)
		{
			std::string query;
			for (char const character : typed)
			{
				unsigned char const byte = static_cast<unsigned char>(character);
				if (byte > 0x20 && byte != 0x7F)
				{
					query.push_back(character);
				}
			}
			return query;
		}
	}

	bool IsVisualScriptEditAction(VisualScriptEditorAction action)
	{
		switch (action)
		{
			case Action::Delete:
			case Action::DeleteWithoutRelink:
			case Action::Cut:
			case Action::Paste:
			case Action::Duplicate:
			case Action::Rename:
			case Action::OpenPalette:
			case Action::AddCommentBox:
			case Action::Straighten:
			case Action::AlignLeft:
			case Action::AlignRight:
			case Action::AlignTop:
			case Action::AlignBottom:
			case Action::AlignCenters:
			case Action::AlignMiddles:
			case Action::DistributeHorizontally:
			case Action::DistributeVertically:
			case Action::Arrange:
			case Action::ToggleCollapsed:
			case Action::ToggleDisabled:
			case Action::SetEnabled:
			case Action::SetDisabled:
			case Action::SetDevelopmentOnly:
			case Action::BreakNodeLinks:
			case Action::AddNodeComment:
			case Action::EditNodeComment:
			case Action::RemoveNodeComment:
			case Action::UpdateNode:
			case Action::NudgeLeft:
			case Action::NudgeRight:
			case Action::NudgeUp:
			case Action::NudgeDown:
			case Action::NudgeLeftFine:
			case Action::NudgeRightFine:
			case Action::NudgeUpFine:
			case Action::NudgeDownFine:
				return true;
			case Action::Copy:
			case Action::SelectAll:
			case Action::DeselectAll:
			case Action::Cancel:
			case Action::FrameSelection:
			case Action::FrameAll:
			case Action::ZoomIn:
			case Action::ZoomOut:
			case Action::ZoomReset:
			case Action::ToggleMinimap:
			case Action::ToggleSnapToGrid:
			case Action::FindReferences:
			case Action::OpenNode:
			case Action::Back:
			case Action::Forward:
			case Action::Find:
			case Action::FindInProject:
			case Action::ShowShortcuts:
			case Action::StoreBookmark1:
			case Action::StoreBookmark2:
			case Action::StoreBookmark3:
			case Action::StoreBookmark4:
			case Action::StoreBookmark5:
			case Action::StoreBookmark6:
			case Action::StoreBookmark7:
			case Action::StoreBookmark8:
			case Action::StoreBookmark9:
			case Action::GoToBookmark1:
			case Action::GoToBookmark2:
			case Action::GoToBookmark3:
			case Action::GoToBookmark4:
			case Action::GoToBookmark5:
			case Action::GoToBookmark6:
			case Action::GoToBookmark7:
			case Action::GoToBookmark8:
			case Action::GoToBookmark9:
				return false;
		}
		return false;
	}

	VisualScriptEditorModel::VisualScriptEditorModel(AssetHandle graph, VisualScriptViewSettings& settings)
		: m_Graph(graph),
		  m_Settings(settings)
	{
		VisualScriptGraphViews const* views = settings.FindGraph(graph);
		if (views != nullptr)
		{
			m_Function = views->Shown;
		}
		VisualScriptViewState const* view = views != nullptr ? views->FindView(m_Function) : nullptr;
		if (view != nullptr)
		{
			m_View.SetState(*view);
			m_StoredView = m_View.GetState();
		}
		else
		{
			VisualScriptViewState initial;
			initial.Function = m_Function;
			m_View.SetState(initial);
			m_NeedsFrame = true;
		}
	}

	VisualScriptSyncResult VisualScriptEditorModel::Sync(VisualScriptSemantics& semantics, VisualScriptTextMeasurer const& measure,
	                                                     VisualScriptThemeAnchors const& theme, bool widgetActive, double time)
	{
		VisualScriptSyncResult result;
		m_Semantics = &semantics;
		// The caller's measurer of this frame is the one used until the next Sync (its captures may not outlive the frame); only
		// a new Signature lays everything out anew.
		if (!m_HasMeasure || measure.Signature != m_Measure.Signature)
		{
			m_CanvasStale = true;
		}
		m_Measure = measure;
		m_HasMeasure = true;
		// What changed outside, and the links an own commit added, are reported until the next frame's first Sync.
		if (time != m_LastSyncTime)
		{
			m_ChangedElements.clear();
			m_CommittedLinks.clear();
			m_SyncTimings = {};
			m_LastSyncTime = time;
		}
		m_Time = time;
		LookUpAsset(result);
		result.State = m_State;
		if (m_PreviewChanged)
		{
			m_PreviewChanged = false;
			result.DocumentChanged = true;
		}
		bool const shows = m_State != VisualScriptDocumentState::Gone && m_State != VisualScriptDocumentState::Missing &&
		                   m_State != VisualScriptDocumentState::Unloadable && m_Display != nullptr;
		if (!shows)
		{
			CancelGesture();
			m_OutsideChange = false;
			return result;
		}
		if (m_OutsideChange)
		{
			// The gesture was made on what is no longer there: it ends without an edit.
			m_OutsideChange = false;
			CancelGesture();
		}
		if (m_Function != 0 && FindGraph(GetDrawnDocument(), m_Function) == nullptr)
		{
			// The function shown was deleted (an undo, automation): the event graph shows.
			SwitchFunction(0);
		}
		if (result.DocumentChanged)
		{
			IndexNodes(GetDrawnDocument(), m_DrawnNodes);
			m_AnalysisPending = true;
		}
		// Never during a gesture: its pins stay what it started with.
		if (m_Gesture == VisualScriptGesture::Idle)
		{
			RefreshAnalysis(semantics, widgetActive, time, false, result);
		}
		SyncCanvas(theme, result);
		return result;
	}

	void VisualScriptEditorModel::OnCommitted(Ref<VisualScriptDocument const> committed, bool layoutOnly)
	{
		m_OwnDocument = std::move(committed);
		m_OwnLayoutOnly = layoutOnly;
	}

	void VisualScriptEditorModel::SetWidgetPreview(VisualScriptDocument preview)
	{
		m_Preview = CreateRef<VisualScriptDocument const>(std::move(preview));
		m_PreviewChanged = true;
	}

	void VisualScriptEditorModel::ClearWidgetPreview()
	{
		if (m_Preview != nullptr)
		{
			m_Preview.reset();
			m_PreviewChanged = true;
		}
	}

	void VisualScriptEditorModel::CheckWritable()
	{
		m_CheckWritable = true;
	}

	VisualScriptDocument const& VisualScriptEditorModel::GetDisplayDocument() const
	{
		static VisualScriptDocument const s_Empty;
		return m_Display != nullptr ? *m_Display : s_Empty;
	}

	VisualScriptAnalysis const* VisualScriptEditorModel::GetAnalysis() const
	{
		return m_Library != nullptr && !m_AnalysisStale ? m_Analysis.get() : nullptr;
	}

	VisualScriptPinTypeLookup VisualScriptEditorModel::MakePinTypeLookup() const
	{
		return [this](uint32_t node, std::string_view pin, VisualScriptPinDirection direction)
		{
			return FindPinType(node, pin, direction);
		};
	}

	size_t VisualScriptEditorModel::GetCapacityBytes() const
	{
		return m_Canvas.GetCapacityBytes() + m_Index.GetCapacityBytes() + GetCapacity(m_ChangedElements) + GetCapacity(m_CommittedLinks) +
		       GetCapacity(m_AnalysisNodes) + GetCapacity(m_AnalyzedNodes) + GetCapacity(m_DrawnNodes) + GetCapacity(m_Selection) +
		       GetCapacity(m_Raised) + GetCapacity(m_NodeSelected) + GetCapacity(m_CommentSelected) + GetCapacity(m_NodeRaise) +
		       GetCapacity(m_NodeMoved) + GetCapacity(m_HitScratch.Candidates) + GetCapacity(m_HitScratch.Nodes) +
		       GetCapacity(m_HitScratch.Polyline) + GetCapacity(m_Query) + GetCapacity(m_Polyline) + GetCapacity(m_DynamicNodes) +
		       GetCapacity(m_DynamicComments) + GetCapacity(m_DynamicLinks) + GetCapacity(m_MarqueeElements) + GetCapacity(m_CutLinks) +
		       GetCapacity(m_Wire.Classes) + GetCapacity(m_Wire.Shown) + GetCapacity(m_Move.Elements) + GetCapacity(m_Move.Nodes);
	}

	VisualScriptEditorState VisualScriptEditorModel::GetState() const
	{
		VisualScriptEditorState state;
		state.Graph = m_Graph;
		if (AssetManager::IsInitialized())
		{
			if (std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(m_Graph))
			{
				state.Path = metadata->Path.empty() ? metadata->Name : metadata->Path;
			}
		}
		state.Function = m_Function;
		state.View = m_View.GetState();
		state.Level = m_View.GetDetailLevel();
		for (uint32_t const element : m_Selection)
		{
			if (m_Canvas.FindNode(element))
			{
				state.SelectedNodes.push_back(element);
			}
			else if (m_Canvas.FindComment(element))
			{
				state.SelectedComments.push_back(element);
			}
		}
		state.Document = m_State;
		if (VisualScriptAnalysis const* analysis = GetAnalysis())
		{
			state.Analyzed = true;
			for (VisualScriptDiagnostic const& diagnostic : analysis->Diagnostics)
			{
				(diagnostic.Severity == VisualScriptSeverity::Error ? state.Errors : state.Warnings)++;
			}
		}
		if (VisualScriptGraphViews const* views = m_Settings.FindGraph(m_Graph))
		{
			for (VisualScriptBookmark const& bookmark : views->Bookmarks)
			{
				state.Bookmarks.push_back(bookmark.Slot);
			}
			std::sort(state.Bookmarks.begin(), state.Bookmarks.end());
		}
		state.MinimapVisible = m_Minimap.Alpha > 0.0f;
		return state;
	}

	VisualScriptGestureView VisualScriptEditorModel::GetGesture() const
	{
		VisualScriptGestureView view;
		view.Kind = m_Gesture;
		view.SpaceHeld = m_SpaceHeld;
		switch (m_Gesture)
		{
			case VisualScriptGesture::Marquee:
				view.Marquee = MakeRect(m_Press.Canvas, m_GestureCanvas);
				view.MarqueeElements = m_MarqueeElements;
				break;
			case VisualScriptGesture::MovingElements:
				view.MovedElements = m_Move.Elements;
				view.MoveOffset = m_Move.Offset;
				view.SpliceTarget = m_Move.SpliceTarget ? &*m_Move.SpliceTarget : nullptr;
				break;
			case VisualScriptGesture::ResizingComment:
				view.ResizedComment = m_Resize.Comment;
				view.ResizedRect = m_Resize.Rect;
				view.ResizeEdges = m_Resize.Edges;
				break;
			case VisualScriptGesture::Wiring:
			{
				view.ExpandedNode = m_Wire.Expanded;
				view.GhostWire = m_Wire.Ghost;
				view.GhostColorFrom = m_Wire.ColorFrom;
				view.GhostColorTo = m_Wire.ColorTo;
				view.GhostInvalid = m_Wire.Invalid;
				view.WireSource = &m_Wire.SourceReference;
				view.WireDirection = m_Wire.Source.Direction;
				view.PinClasses = m_Wire.Shown;
				break;
			}
			case VisualScriptGesture::Relinking:
				view.PickedLinks = m_Relink.Links;
				view.PickedWires = m_Relink.Wires;
				break;
			case VisualScriptGesture::Cutting:
				view.CutLine = std::make_pair(m_Press.Canvas, m_GestureCanvas);
				view.CutLinks = m_CutLinks;
				break;
			case VisualScriptGesture::Idle:
			case VisualScriptGesture::Panning:
			case VisualScriptGesture::MinimapDrag:
				break;
		}
		return view;
	}

	VisualScriptNodePreview VisualScriptEditorModel::GetNodePreview() const
	{
		VisualScriptNodePreview preview;
		if (m_Gesture == VisualScriptGesture::MovingElements)
		{
			preview.Moved = m_NodeMoved;
			preview.Offset = m_Move.Offset;
		}
		if (m_Gesture == VisualScriptGesture::Wiring && m_Wire.Expanded)
		{
			preview.Expanded = m_Canvas.FindNode(*m_Wire.Expanded);
		}
		return preview;
	}

	// --- Sync ---

	void VisualScriptEditorModel::LookUpAsset(VisualScriptSyncResult& result)
	{
		if (!AssetManager::IsInitialized() || !AssetManager::IsValid(m_Graph))
		{
			m_State = VisualScriptDocumentState::Gone;
			return;
		}
		if (AssetManager::IsMissing(m_Graph))
		{
			m_State = VisualScriptDocumentState::Missing;
			return;
		}
		Result<Ref<VisualScriptAsset>> asset = AssetManager::TryGetAsset<VisualScriptAsset>(m_Graph);
		if (!asset)
		{
			m_State = VisualScriptDocumentState::Unloadable;
			return;
		}
		Ref<VisualScriptAsset> const& current = asset.GetValue();
		if (current != m_SeenAsset || current->GetVersion() != m_SeenVersion)
		{
			if (current != m_SeenAsset)
			{
				std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(m_Graph);
				m_IsFileAsset = metadata && metadata->IsFileAsset();
			}
			m_SeenAsset = current;
			m_SeenVersion = current->GetVersion();
			m_CheckWritable = true;
			AdoptDocument(current->GetSharedDocument(), result);
		}
		if (!m_IsFileAsset)
		{
			m_State = VisualScriptDocumentState::ReadOnly;
			return;
		}
		if (m_CheckWritable)
		{
			// What cannot be told is taken as writable; a write that then fails shows as one.
			m_Writable = !FileSystem::IsReadOnly(AssetManager::GetAbsolutePath(m_Graph));
			m_CheckWritable = false;
		}
		m_State = m_Writable ? VisualScriptDocumentState::Editable : VisualScriptDocumentState::ReadOnlyFile;
	}

	void VisualScriptEditorModel::AdoptDocument(Ref<VisualScriptDocument const> document, VisualScriptSyncResult& result)
	{
		PhaseTimer const timer(m_SyncTimings.Adopt);
		if (document == m_Document)
		{
			return;
		}
		bool const own = m_OwnDocument != nullptr && document == m_OwnDocument;
		bool const layoutOnly = own && m_OwnLayoutOnly && m_Document != nullptr;
		// The commit the window waited for is this one, or was replaced before it was seen: either way it is done with, so a
		// later redo of the same step brings the object back as an outside change.
		m_OwnDocument.reset();
		m_OwnLayoutOnly = false;
		if (m_Document != nullptr && own)
		{
			// A layout-only commit adds no link.
			if (!layoutOnly)
			{
				RecordCommittedLinks(*m_Document, *document);
			}
		}
		else if (m_Document != nullptr && !(*document == *m_Document && HaveSameVisualScriptProgram(*document, *m_Document)))
		{
			// (operator== takes 1 and 1.0 as equal, which the runtime reads as different programs: only an identical document
			// is adopted silently.)
			// An outside change (undo, redo, automation, another window, a reload): a widget's pending value no longer applies,
			// and a gesture in progress ends without an edit.
			RecordChangedElements(*m_Document, *document);
			m_Preview.reset();
			m_OutsideChange = true;
		}
		Ref<VisualScriptDocument const> const previous = m_Display;
		m_Document = std::move(document);
		if (HasUnplacedNodes(*m_Document))
		{
			// Placed in memory only: the first commit writes the places.
			VisualScriptDocument placed = *m_Document;
			PlaceUnplacedNodes(placed);
			m_Display = CreateRef<VisualScriptDocument const>(std::move(placed));
		}
		else
		{
			m_Display = m_Document;
		}
		m_LayoutOnlyFrom.reset();
		if (layoutOnly)
		{
			// The window's own layout-only commit has the program of the document it was made from (the display document), so
			// what was known of that program is known of this document: its analysis, and the runtime's failure on it. Its
			// nodes are at the same indices (HaveSameVisualScriptProgram compares them in order).
			if (m_AnalyzedDocument == previous)
			{
				m_AnalyzedDocument = m_Display;
			}
			if (m_FailedDocument == previous)
			{
				m_FailedDocument = m_Display;
			}
			m_LayoutOnlyFrom = previous;
		}
		else
		{
			m_ProgramRevision++;
		}
		result.DocumentChanged = true;
	}

	void VisualScriptEditorModel::RecordChangedElements(VisualScriptDocument const& before, VisualScriptDocument const& after)
	{
		VisualScriptGraph const* previous = FindGraph(before, m_Function);
		VisualScriptGraph const* current = FindGraph(after, m_Function);
		if (current == nullptr)
		{
			return;
		}
		// The elements as they were, by ID; an element that was not there, or was there otherwise, changed.
		auto const record = [this](auto const& elements, auto const* earlier)
		{
			std::vector<IdIndex> sorted;
			if (earlier != nullptr)
			{
				sorted.reserve(earlier->size());
				for (uint32_t index = 0; index < earlier->size(); index++)
				{
					sorted.emplace_back((*earlier)[index].ID, index);
				}
				std::stable_sort(sorted.begin(), sorted.end(),
				                 [](IdIndex const& a, IdIndex const& b)
				                 {
									 return a.first < b.first;
								 });
			}
			for (auto const& element : elements)
			{
				std::optional<uint32_t> const found = FindByID(sorted, element.ID);
				if (!found || !IsSameElement((*earlier)[*found], element))
				{
					m_ChangedElements.push_back(element.ID);
				}
			}
		};
		record(current->Nodes, previous != nullptr ? &previous->Nodes : nullptr);
		record(current->Comments, previous != nullptr ? &previous->Comments : nullptr);
	}

	void VisualScriptEditorModel::RecordCommittedLinks(VisualScriptDocument const& before, VisualScriptDocument const& after)
	{
		VisualScriptGraph const* previous = FindGraph(before, m_Function);
		VisualScriptGraph const* current = FindGraph(after, m_Function);
		if (current == nullptr)
		{
			return;
		}
		std::vector<VisualScriptLink const*> earlier;
		if (previous != nullptr)
		{
			earlier.reserve(previous->Links.size());
			for (VisualScriptLink const& link : previous->Links)
			{
				earlier.push_back(&link);
			}
			std::sort(earlier.begin(), earlier.end(),
			          [](VisualScriptLink const* a, VisualScriptLink const* b)
			          {
						  return LinkLess(*a, *b);
					  });
		}
		for (VisualScriptLink const& link : current->Links)
		{
			bool const existed = std::binary_search(earlier.begin(), earlier.end(), &link,
			                                        [](VisualScriptLink const* a, VisualScriptLink const* b)
			                                        {
														return LinkLess(*a, *b);
													});
			if (!existed)
			{
				m_CommittedLinks.push_back(link);
			}
		}
	}

	void VisualScriptEditorModel::RefreshAnalysis(VisualScriptSemantics& semantics, bool widgetActive, double time, bool force,
	                                              VisualScriptSyncResult& result)
	{
		PhaseTimer const timer(m_SyncTimings.Analysis);
		Ref<VisualScriptLibrary const> library = semantics.GetLibrary();
		bool const libraryChanged = library != m_Library;
		if (libraryChanged)
		{
			// The runtime started, stopped, or read its library again: analyze anew and lay everything out anew.
			m_Library = std::move(library);
			m_AnalysisPending = true;
			m_CanvasStale = true;
		}
		if (m_Library == nullptr)
		{
			// No analysis without the runtime: the last one keeps its pins, without its problems.
			if (m_Analysis != nullptr && !m_AnalysisStale)
			{
				m_AnalysisStale = true;
				result.AnalysisChanged = true;
			}
			return;
		}
		if (!m_AnalysisPending)
		{
			return;
		}
		Ref<VisualScriptDocument const> const& drawn = GetDrawnDocumentRef();
		// Made with the library of now: a library seen first in a frame whose analysis was put off still counts as new later.
		bool const current = m_AnalyzedDocument != nullptr && !m_AnalysisStale && m_AnalyzedLibrary == m_Library &&
		                     (drawn == m_AnalyzedDocument || HaveSameVisualScriptProgram(*drawn, *m_AnalyzedDocument));
		if (current || (m_FailedLibrary == m_Library && drawn == m_FailedDocument))
		{
			// The program the analysis describes (only positions, comments or collapsing changed), or the very document the
			// runtime could not analyze. The analysis describes the drawn document as well as the one it was made from (their
			// nodes are at the same indices), so it is taken as the drawn one's: a layout-only commit of it then keeps it
			// without a comparison (AdoptDocument).
			if (current)
			{
				m_AnalyzedDocument = drawn;
			}
			m_AnalysisPending = false;
			return;
		}
		if (!force && m_HasAnalyzed && (time == m_LastAnalysisTime || (widgetActive && time - m_LastAnalysisTime < AnalysisInterval)))
		{
			// At most once per frame, and at most every AnalysisInterval while a widget is being used: the previous analysis
			// keeps drawing meanwhile.
			return;
		}
		Result<VisualScriptAnalysis> analysis = semantics.Analyze(*drawn);
		m_AnalysisCount++;
		m_HasAnalyzed = true;
		m_LastAnalysisTime = time;
		m_AnalysisPending = false;
		if (analysis)
		{
			m_Analysis = CreateRef<VisualScriptAnalysis const>(analysis.TakeValue());
			m_AnalyzedDocument = drawn;
			m_AnalyzedLibrary = m_Library;
			m_AnalysisStale = false;
			m_FailedDocument.reset();
			m_FailedLibrary.reset();
			IndexAnalysis();
			result.AnalysisChanged = true;
			return;
		}
		m_FailedDocument = drawn;
		m_FailedLibrary = m_Library;
		if (m_Analysis != nullptr && !m_AnalysisStale)
		{
			m_AnalysisStale = true;
			result.AnalysisChanged = true;
		}
	}

	void VisualScriptEditorModel::SyncCanvas(VisualScriptThemeAnchors const& theme, VisualScriptSyncResult& result)
	{
		// The asset registry's version moves often (thumbnails, focus refreshes, model loads): it matters only while a chip
		// shows an asset's name.
		uint64_t const assetVersion = AssetManager::IsInitialized() ? AssetManager::GetVersion() : 0;
		bool const assetNamesChanged = assetVersion != m_AssetVersion && m_Canvas.ShowsAssetNames();
		bool const stale = m_CanvasStale || result.DocumentChanged || result.AnalysisChanged || !(theme == m_Theme) || assetNamesChanged ||
		                   m_Function != m_CanvasFunction;
		if (!stale)
		{
			m_AssetVersion = assetVersion;
			return;
		}
		VisualScriptCanvasSource source;
		source.Display = &GetDrawnDocument();
		source.Function = m_Function;
		source.Analysis = m_Analysis.get();
		source.AnalyzedDocument = m_AnalyzedDocument.get();
		source.Library = m_Library.get();
		if (VisualScriptLibrary const* library = m_Library.get())
		{
			source.EnumLookup = [library](std::string_view typeId)
			{
				return library->FindEnum(typeId);
			};
		}
		source.ShowDiagnostics = !m_AnalysisStale;
		source.AssetNames = DescribeAsset;
		source.AssetVersion = assetVersion;
		std::optional<bool> synced;
		{
			PhaseTimer const timer(m_SyncTimings.Canvas);
			// An own layout-only commit of the document the canvas shows, when nothing else needs laying out: only what moved
			// moves. SyncPositions checks what it relies on and declines anything else.
			if (m_LayoutOnlyFrom != nullptr && m_CanvasDocument == m_LayoutOnlyFrom && m_Preview == nullptr && !m_CanvasStale &&
			    !result.AnalysisChanged && theme == m_Theme && !assetNamesChanged && m_Function == m_CanvasFunction)
			{
				synced = m_Canvas.SyncPositions(*m_CanvasDocument, source, m_Measure, theme);
			}
			if (!synced)
			{
				synced = m_Canvas.Sync(source, m_Measure, theme);
			}
		}
		m_LayoutOnlyFrom.reset();
		m_CanvasDocument = GetDrawnDocumentRef();
		if (*synced)
		{
			PhaseTimer const timer(m_SyncTimings.Index);
			m_Index.Build(m_Canvas);
			result.GeometryChanged = true;
		}
		m_Theme = theme;
		m_AssetVersion = assetVersion;
		m_CanvasFunction = m_Function;
		m_CanvasStale = false;
		m_CanvasRevision++;
		OnCanvasChanged();
	}

	void VisualScriptEditorModel::OnCanvasChanged()
	{
		m_ContentBounds = m_Canvas.GetBounds();
		PruneToCanvas();
		RebuildSelectionMasks();
		RebuildRaise();
		RebuildDynamic();
		// Pin indices may have moved: what the mouse is over, again.
		if (m_Hover.Kind != Kind::None)
		{
			UpdateHover(m_LastMouse);
		}
	}

	void VisualScriptEditorModel::ForceAnalysis()
	{
		// Never during a gesture: a wire keeps pointers to the pins it started with, which a new analysis would free.
		if (m_Semantics == nullptr || m_Display == nullptr || !IsInteractive() || m_Gesture != VisualScriptGesture::Idle)
		{
			return;
		}
		VisualScriptSyncResult result;
		RefreshAnalysis(*m_Semantics, false, m_Time, true, result);
		SyncCanvas(m_Theme, result);
	}

	void VisualScriptEditorModel::IndexAnalysis()
	{
		m_AnalysisNodes.clear();
		for (uint32_t index = 0; index < m_Analysis->Nodes.size(); index++)
		{
			m_AnalysisNodes.emplace_back(m_Analysis->Nodes[index].ID, index);
		}
		std::stable_sort(m_AnalysisNodes.begin(), m_AnalysisNodes.end(),
		                 [](IdIndex const& a, IdIndex const& b)
		                 {
							 return a.first < b.first;
						 });
		IndexNodes(*m_AnalyzedDocument, m_AnalyzedNodes);
	}

	VisualScriptDocument const& VisualScriptEditorModel::GetDrawnDocument() const
	{
		return *GetDrawnDocumentRef();
	}

	Ref<VisualScriptDocument const> const& VisualScriptEditorModel::GetDrawnDocumentRef() const
	{
		return m_Preview != nullptr ? m_Preview : m_Display;
	}

	VisualScriptNode const* VisualScriptEditorModel::FindDrawnNode(uint32_t node, VisualScriptGraph const** graph) const
	{
		if (m_Display == nullptr)
		{
			return nullptr;
		}
		std::optional<uint64_t> const packed = FindByID(m_DrawnNodes, node);
		return packed ? GetPackedNode(GetDrawnDocument(), *packed, graph) : nullptr;
	}

	std::optional<std::string> VisualScriptEditorModel::FindPinType(uint32_t node, std::string_view pin,
	                                                                VisualScriptPinDirection direction) const
	{
		VisualScriptGraph const* graph = nullptr;
		VisualScriptNode const* stored = FindDrawnNode(node, &graph);
		if (stored == nullptr)
		{
			return std::nullopt;
		}
		// The analysis, while the node is what it analyzed.
		if (m_Analysis != nullptr && m_AnalyzedDocument != nullptr)
		{
			std::optional<uint32_t> const entry = FindByID(m_AnalysisNodes, node);
			std::optional<uint64_t> const counterpart = FindByID(m_AnalyzedNodes, node);
			VisualScriptNode const* analyzed = counterpart ? GetPackedNode(*m_AnalyzedDocument, *counterpart) : nullptr;
			if (entry && analyzed != nullptr && MatchesVisualScriptAnalysis(*stored, *analyzed, m_Analysis->Nodes[*entry]))
			{
				VisualScriptAnalyzedNode const& analyzedNode = m_Analysis->Nodes[*entry];
				std::string buffer;
				std::string_view const name = MapVisualScriptPinName(&analyzedNode, pin, direction, buffer);
				VisualScriptPin const* found = analyzedNode.FindPin(name, direction);
				if (found == nullptr)
				{
					found = analyzedNode.FindPin(pin, direction);
				}
				return found != nullptr ? std::optional<std::string>(found->Type) : std::nullopt;
			}
		}
		// The library's template; then the pins the node's links and literals name, of no known type.
		if (m_Library != nullptr)
		{
			if (VisualScriptNodeTemplate const* kind = m_Library->FindNode(stored->Type))
			{
				if (VisualScriptPin const* found = kind->FindPin(pin, direction))
				{
					return found->Type;
				}
			}
		}
		bool const input = direction == VisualScriptPinDirection::In;
		bool named = input && stored->Inputs.is_object() && stored->Inputs.contains(pin);
		for (size_t index = 0; index < graph->Links.size() && !named; index++)
		{
			VisualScriptLink const& link = graph->Links[index];
			named = input ? (link.To.Node == node && link.To.Pin == pin) : (link.From.Node == node && link.From.Pin == pin);
		}
		return named ? std::optional<std::string>(std::string(VisualScriptWildcardType)) : std::nullopt;
	}

	VisualScriptPin const* VisualScriptEditorModel::FindPinInfo(uint32_t node, std::string_view pin,
	                                                            VisualScriptPinDirection direction) const
	{
		VisualScriptNode const* stored = FindDrawnNode(node);
		if (stored == nullptr)
		{
			return nullptr;
		}
		if (m_Analysis != nullptr && m_AnalyzedDocument != nullptr)
		{
			std::optional<uint32_t> const entry = FindByID(m_AnalysisNodes, node);
			std::optional<uint64_t> const counterpart = FindByID(m_AnalyzedNodes, node);
			VisualScriptNode const* analyzed = counterpart ? GetPackedNode(*m_AnalyzedDocument, *counterpart) : nullptr;
			if (entry && analyzed != nullptr && MatchesVisualScriptAnalysis(*stored, *analyzed, m_Analysis->Nodes[*entry]))
			{
				// The canvas' names are the analysis' own; a name the document stores maps to them.
				VisualScriptAnalyzedNode const& analyzedNode = m_Analysis->Nodes[*entry];
				if (VisualScriptPin const* found = analyzedNode.FindPin(pin, direction))
				{
					return found;
				}
				std::string buffer;
				return analyzedNode.FindPin(MapVisualScriptPinName(&analyzedNode, pin, direction, buffer), direction);
			}
		}
		if (m_Library != nullptr)
		{
			if (VisualScriptNodeTemplate const* kind = m_Library->FindNode(stored->Type))
			{
				return kind->FindPin(pin, direction);
			}
		}
		return nullptr;
	}

	VisualScriptPin const* VisualScriptEditorModel::FindPinInfo(PinKey const& key) const
	{
		return FindPinInfo(key.Node, key.Pin, key.Direction);
	}

	bool VisualScriptEditorModel::HasFunction(uint32_t function) const
	{
		return function == 0 || (m_Display != nullptr && FindGraph(*m_Display, function) != nullptr);
	}

	bool VisualScriptEditorModel::IsInteractive() const
	{
		bool const shows = m_State == VisualScriptDocumentState::Editable || m_State == VisualScriptDocumentState::ReadOnly ||
		                   m_State == VisualScriptDocumentState::ReadOnlyFile;
		return shows && m_Display != nullptr && !m_CanvasStale && m_CanvasFunction == m_Function;
	}

	// --- Update ---

	std::vector<VisualScriptEditorIntent> VisualScriptEditorModel::Update(VisualScriptCanvasInput const& input)
	{
		std::vector<VisualScriptEditorIntent> intents;
		bool const first = !m_HasUpdated;
		double const deltaTime = first || !std::isfinite(input.Time) ? 0.0 : std::clamp(input.Time - m_InputTime, 0.0, MaxFrameSeconds);
		m_HasUpdated = true;
		if (std::isfinite(input.Time))
		{
			m_InputTime = input.Time;
			m_Time = input.Time;
		}
		m_View.SetCanvas(input.CanvasMin, input.CanvasSize, input.UIScale);
		m_View.Advance(input.Time);
		m_CurveTolerance = input.CurveTessellationTolerance > 0.0f && std::isfinite(input.CurveTessellationTolerance)
		                       ? input.CurveTessellationTolerance
		                       : 1.25f;
		if (!IsInteractive())
		{
			CancelGesture();
			m_Hover = {};
			m_SpaceHeld = false;
			return intents;
		}
		glm::vec2 const canvasSize = m_View.GetCanvasSize();
		if (canvasSize.x > 0.0f && canvasSize.y > 0.0f)
		{
			ApplyPendingView(input.Time);
		}
		UpdateMinimap(deltaTime, first);

		// What the mouse is over as the last frame drew it: what a press this frame means.
		bool const tracking = input.Hovered || m_Gesture != VisualScriptGesture::Idle || m_Press.Active;
		m_LastMouse = input.Mouse;
		if (tracking)
		{
			UpdateHover(input.Mouse);
		}
		else
		{
			m_Hover = {};
		}
		VisualScriptViewState const viewBefore = m_View.GetState();
		uint64_t const revisionBefore = m_CanvasRevision;

		bool const spaceHeld = UpdateSpace(input, intents);
		// While Space is held, keys without Ctrl or Alt type the palette's query, never their canvas actions.
		bool const keysAreTyping = spaceHeld && !input.Ctrl && !input.Alt;
		for (VisualScriptEditorAction const action : input.Actions)
		{
			// A gesture or a press in progress takes no key but Escape: an edit, a jump or a framing would act under its preview.
			bool const busy = m_Gesture != VisualScriptGesture::Idle || m_Press.Active;
			if ((keysAreTyping || busy) && action != Action::Cancel)
			{
				continue;
			}
			ApplyAction(action, input, intents);
		}
		UpdateMouse(input, deltaTime, intents);
		UpdateDrop(input, intents);

		if (tracking && (!(m_View.GetState() == viewBefore) || m_CanvasRevision != revisionBefore))
		{
			// The view moved or the canvas was laid out again: what the renderer shows under the mouse now.
			UpdateHover(input.Mouse);
		}
		StoreView();
		return intents;
	}

	void VisualScriptEditorModel::CancelGesture()
	{
		EndGesture();
	}

	void VisualScriptEditorModel::EndGesture()
	{
		m_Gesture = VisualScriptGesture::Idle;
		m_Press.Active = false;
		m_Press.Kind = PressKind::Inert;
		m_Move.Elements.clear();
		m_Move.Nodes.clear();
		m_Move.Comments = 0;
		m_Move.Offset = glm::vec2(0.0f);
		m_Move.SpliceCandidate.reset();
		m_Move.SpliceTarget.reset();
		m_Wire.Classes.clear();
		m_Wire.Shown.clear();
		m_Wire.Snapped.reset();
		m_Wire.Resting.reset();
		m_Wire.Expanded.reset();
		m_Wire.SourceInfo = nullptr;
		m_Relink.Links.clear();
		m_Relink.FarEnds.clear();
		m_Relink.Wires.clear();
		m_Relink.Snapped.reset();
		m_MarqueeElements.clear();
		m_CutLinks.clear();
		m_Resize = ResizeState{};
		RebuildDynamic();
	}

	void VisualScriptEditorModel::ApplyOpenRequest(VisualScriptOpenRequest const& request)
	{
		PendingOpen pending;
		pending.Request = request;
		if (request.Function && *request.Function != m_Function && (m_Display == nullptr || HasFunction(*request.Function)))
		{
			if (m_HasShownView)
			{
				PushHistory(m_View.GetTargetState());
				pending.Pushed = true;
			}
			SwitchFunction(*request.Function);
		}
		m_PendingOpen = std::move(pending);
	}

	void VisualScriptEditorModel::ShowFunction(uint32_t function)
	{
		if (function == m_Function || (m_Display != nullptr && !HasFunction(function)))
		{
			return;
		}
		PushHistory(m_View.GetTargetState());
		SwitchFunction(function);
	}

	void VisualScriptEditorModel::Select(std::span<uint32_t const> elements, VisualScriptSelectMode mode)
	{
		std::vector<uint32_t> valid;
		valid.reserve(elements.size());
		for (uint32_t const element : elements)
		{
			if ((m_Canvas.FindNode(element) || m_Canvas.FindComment(element)) &&
			    std::find(valid.begin(), valid.end(), element) == valid.end())
			{
				valid.push_back(element);
			}
		}
		std::vector<uint32_t> sorted = m_Selection;
		std::sort(sorted.begin(), sorted.end());
		auto const isSelected = [&sorted](uint32_t element)
		{
			return std::binary_search(sorted.begin(), sorted.end(), element);
		};
		std::vector<uint32_t> result;
		switch (mode)
		{
			case VisualScriptSelectMode::Replace:
				result = std::move(valid);
				break;
			case VisualScriptSelectMode::Add:
				result = m_Selection;
				for (uint32_t const element : valid)
				{
					if (!isSelected(element))
					{
						result.push_back(element);
					}
				}
				if (valid.size() == 1)
				{
					std::erase(result, valid.front());
					result.insert(result.begin(), valid.front());
				}
				break;
			case VisualScriptSelectMode::Toggle:
			{
				std::vector<uint32_t> sortedValid = valid;
				std::sort(sortedValid.begin(), sortedValid.end());
				for (uint32_t const element : m_Selection)
				{
					if (!std::binary_search(sortedValid.begin(), sortedValid.end(), element))
					{
						result.push_back(element);
					}
				}
				for (uint32_t const element : valid)
				{
					if (!isSelected(element))
					{
						result.push_back(element);
					}
				}
				if (valid.size() == 1 && !isSelected(valid.front()))
				{
					std::erase(result, valid.front());
					result.insert(result.begin(), valid.front());
				}
				break;
			}
			case VisualScriptSelectMode::Remove:
			{
				std::sort(valid.begin(), valid.end());
				for (uint32_t const element : m_Selection)
				{
					if (!std::binary_search(valid.begin(), valid.end(), element))
					{
						result.push_back(element);
					}
				}
				break;
			}
		}
		SetSelection(std::move(result));
	}

	void VisualScriptEditorModel::StartWire(uint32_t node, std::string_view pin, VisualScriptPinDirection direction)
	{
		if (!IsInteractive())
		{
			return;
		}
		CancelGesture();
		PinKey key;
		key.Node = node;
		key.Pin = std::string(pin);
		key.Direction = direction;
		BeginWire(key, true);
	}

	bool VisualScriptEditorModel::IsInCanvas(glm::vec2 screenPoint) const
	{
		glm::vec2 const min = m_View.GetCanvasMin();
		glm::vec2 const max = min + m_View.GetCanvasSize();
		return screenPoint.x >= min.x && screenPoint.y >= min.y && screenPoint.x < max.x && screenPoint.y < max.y;
	}

	VisualScriptHit VisualScriptEditorModel::HitTest(glm::vec2 mouse, bool skipMoved)
	{
		if (!IsInCanvas(mouse))
		{
			// A gesture keeps the mouse when it leaves the canvas, but what lies beyond the canvas's edges is not drawn there.
			return VisualScriptHit{};
		}
		return HitTestCanvas(m_Canvas, m_Index, m_View, MakeHitContext(skipMoved), mouse);
	}

	void VisualScriptEditorModel::UpdateHover(glm::vec2 mouse)
	{
		if (!IsInCanvas(mouse))
		{
			m_Hover = {};
			return;
		}
		HitTestCanvas(m_Canvas, m_Index, m_View, MakeHitContext(false), mouse, m_Hover);
	}

	VisualScriptHitContext VisualScriptEditorModel::MakeHitContext(bool skipMoved)
	{
		VisualScriptHitContext context;
		if (m_Minimap.Alpha > 0.0f && m_Minimap.Transform.Scale > 0.0f)
		{
			context.Minimap = m_Minimap.Transform.Rect;
		}
		context.Selected = m_NodeSelected;
		context.Z = m_NodeRaise;
		context.Preview = GetNodePreview();
		context.DynamicLinks = m_DynamicLinks;
		context.SkipMoved = skipMoved;
		context.CurveTessellationTolerance = m_CurveTolerance;
		context.Scratch = &m_HitScratch;
		return context;
	}

	void VisualScriptEditorModel::UpdateMinimap(double deltaTime, bool first)
	{
		m_Minimap.Transform = VisualScriptGeometry::GetMinimapTransform(
			VisualScriptGeometry::GetMinimapRect(m_View.GetCanvasMin(), m_View.GetCanvasSize(), m_View.GetUIScale()), m_ContentBounds);
		bool const shown = m_Settings.GetPanes().ShowMinimap && m_ContentBounds && m_Minimap.Transform.Scale > 0.0f &&
		                   !ContainsRect(m_View.GetVisibleRect(), *m_ContentBounds);
		float const target = shown ? 1.0f : 0.0f;
		if (first || m_Settings.IsReduceMotion())
		{
			m_Minimap.Alpha = target;
			return;
		}
		float const step = static_cast<float>(deltaTime / Metrics::MinimapFadeSeconds);
		m_Minimap.Alpha = target > m_Minimap.Alpha ? std::min(target, m_Minimap.Alpha + step) : std::max(target, m_Minimap.Alpha - step);
	}

	void VisualScriptEditorModel::ApplyPendingView(double time)
	{
		if (m_PendingOpen)
		{
			PendingOpen const pending = std::move(*m_PendingOpen);
			m_PendingOpen.reset();
			VisualScriptOpenRequest request = pending.Request;
			if (!request.Select.empty())
			{
				Select(request.Select, VisualScriptSelectMode::Replace);
			}
			if (m_NeedsFrame && request.Frame == VisualScriptFrame::Keep)
			{
				// A sub-graph never shown has no view to keep: it shows whole.
				request.Frame = VisualScriptFrame::All;
			}
			std::optional<VisualScriptRect> bounds;
			if (request.Frame == VisualScriptFrame::Selection)
			{
				bounds = m_Selection.empty() ? m_ContentBounds : m_Canvas.GetBounds(m_Selection);
			}
			else if (request.Frame == VisualScriptFrame::All)
			{
				bounds = m_ContentBounds;
			}
			VisualScriptViewState const target = m_View.GetOpenTarget(request, bounds);
			VisualScriptViewState const current = m_View.GetTargetState();
			if (!(target == current))
			{
				if (m_HasShownView && !pending.Pushed)
				{
					PushHistory(current);
				}
				m_View.AnimateTo(target, time, m_HasShownView ? GetMoveDuration() : 0.0);
			}
			m_NeedsFrame = false;
		}
		else if (m_NeedsFrame)
		{
			m_View.AnimateTo(m_View.GetFrameTarget(m_ContentBounds), time, 0.0);
			m_NeedsFrame = false;
		}
		m_HasShownView = true;
	}

	bool VisualScriptEditorModel::UpdateSpace(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		bool const keys = input.Focused && !input.TextInput;
		if (input.SpacePressed && keys)
		{
			m_SpaceHeld = true;
			m_SpaceTap = true;
			m_SpacePaletteOpened = false;
			m_SpaceDownTime = input.Time;
		}
		if (!m_SpaceHeld)
		{
			return false;
		}
		if (input.Pressed[0] || input.Pressed[1] || input.Pressed[2])
		{
			m_SpaceTap = false;
		}
		// Nothing opens over a gesture or a press in progress (a Space drag pans).
		bool const idle = m_Gesture == VisualScriptGesture::Idle && !m_Press.Active;
		std::string typed = GetQueryCharacters(input.Characters);
		if (!typed.empty())
		{
			// A character typed while Space is down opens the palette at once with it (Space, f, o, r gives "for").
			m_SpaceTap = false;
			if (keys && idle && !m_SpacePaletteOpened)
			{
				m_SpacePaletteOpened = true;
				OpenPalette(GetActionPosition(input), std::move(typed), intents);
			}
		}
		if (input.SpaceReleased || !input.SpaceDown || !input.Focused)
		{
			if (input.SpaceReleased && keys && idle && m_SpaceTap && input.Time - m_SpaceDownTime <= Metrics::SpaceTapSeconds)
			{
				OpenPalette(GetActionPosition(input), {}, intents);
			}
			m_SpaceHeld = false;
		}
		return true;
	}

	void VisualScriptEditorModel::ApplyAction(VisualScriptEditorAction action, VisualScriptCanvasInput const& input,
	                                          std::vector<VisualScriptEditorIntent>& intents)
	{
		if (IsInRange(action, Action::StoreBookmark1, Action::StoreBookmark9))
		{
			StoreBookmark(GetBookmarkSlot(action, Action::StoreBookmark1));
			return;
		}
		if (IsInRange(action, Action::GoToBookmark1, Action::GoToBookmark9))
		{
			GoToBookmark(GetBookmarkSlot(action, Action::GoToBookmark1), input.Time);
			return;
		}
		switch (action)
		{
			case Action::Cancel:
				if (m_Gesture != VisualScriptGesture::Idle || m_Press.Active)
				{
					CancelGesture();
				}
				else
				{
					SetSelection({});
				}
				return;
			case Action::SelectAll:
			{
				std::vector<uint32_t> all = GetAllElements();
				if (!m_Selection.empty())
				{
					// The primary element stays primary.
					auto const primary = std::find(all.begin(), all.end(), m_Selection.front());
					if (primary != all.end())
					{
						std::rotate(all.begin(), primary, primary + 1);
					}
				}
				SetSelection(std::move(all));
				return;
			}
			case Action::DeselectAll:
				SetSelection({});
				return;
			case Action::FrameSelection:
				Frame(m_Selection, input.Time);
				return;
			case Action::FrameAll:
				Frame({}, input.Time);
				return;
			case Action::ZoomIn:
				m_View.StepZoom(1, m_View.GetScreenCenter());
				return;
			case Action::ZoomOut:
				m_View.StepZoom(-1, m_View.GetScreenCenter());
				return;
			case Action::ZoomReset:
				m_View.ZoomAt(input.Hovered ? input.Mouse : m_View.GetScreenCenter(), 1.0f);
				return;
			case Action::ToggleMinimap:
			{
				VisualScriptPaneSettings panes = m_Settings.GetPanes();
				panes.ShowMinimap = !panes.ShowMinimap;
				m_Settings.SetPanes(panes);
				return;
			}
			case Action::ToggleSnapToGrid:
				m_Settings.SetSnapToGrid(!m_Settings.IsSnapToGrid());
				return;
			case Action::Back:
				GoBack(input.Time);
				return;
			case Action::Forward:
				GoForward(input.Time);
				return;
			case Action::OpenPalette:
				OpenPalette(GetActionPosition(input), input.TextInput ? std::string() : GetQueryCharacters(input.Characters), intents);
				return;
			default:
				break;
		}
		if (IsVisualScriptEditAction(action) && IsReadOnly())
		{
			return;
		}
		if (action == Action::Delete || action == Action::Cut || action == Action::Paste || action == Action::UpdateNode)
		{
			// Relinking a delete, binding a paste and a quick fix read the pins.
			ForceAnalysis();
		}
		VisualScriptActionIntent intent;
		intent.Action = action;
		intent.Selection = m_Selection;
		intent.Position = GetActionPosition(input);
		intents.emplace_back(std::move(intent));
	}

	void VisualScriptEditorModel::OpenPalette(glm::vec2 position, std::string query, std::vector<VisualScriptEditorIntent>& intents)
	{
		if (IsReadOnly())
		{
			return;
		}
		if (m_Library != nullptr)
		{
			ForceAnalysis();
		}
		if (m_Library == nullptr)
		{
			// The palette lists the node library, which only the scripting runtime has.
			m_RefusedCount++;
			return;
		}
		VisualScriptPaletteIntent palette;
		palette.Position = position;
		palette.Query = std::move(query);
		intents.emplace_back(std::move(palette));
	}

	glm::vec2 VisualScriptEditorModel::GetActionPosition(VisualScriptCanvasInput const& input) const
	{
		return input.Hovered ? m_View.ToCanvas(input.Mouse) : m_View.GetState().Center;
	}

	void VisualScriptEditorModel::AutoPan(VisualScriptCanvasInput const& input, double deltaTime)
	{
		float const fontSize = input.FontSize > 0.0f && std::isfinite(input.FontSize) ? input.FontSize : 15.0f * m_View.GetUIScale();
		m_View.AutoPan(input.Mouse, 2.0f * fontSize, 20.0f * fontSize, static_cast<float>(deltaTime));
	}

	double VisualScriptEditorModel::GetMoveDuration() const
	{
		return m_Settings.IsReduceMotion() ? 0.0 : VisualScriptView::AnimationSeconds;
	}

	void VisualScriptEditorModel::StoreView()
	{
		if (m_View.IsAnimating() || !m_HasShownView)
		{
			return;
		}
		VisualScriptViewState const& view = m_View.GetState();
		if (m_StoredView && *m_StoredView == view)
		{
			return;
		}
		m_Settings.SetView(m_Graph, view);
		m_StoredView = view;
	}

	void VisualScriptEditorModel::UpdateMouse(VisualScriptCanvasInput const& input, double deltaTime,
	                                          std::vector<VisualScriptEditorIntent>& intents)
	{
		// The mouse's back and forward buttons act as Back and Forward do, so not over a gesture or a press in progress.
		bool const busy = m_Gesture != VisualScriptGesture::Idle || m_Press.Active;
		if (input.ExtraPressed[0] && !busy)
		{
			GoBack(input.Time);
		}
		if (input.ExtraPressed[1] && !busy)
		{
			GoForward(input.Time);
		}
		if (input.Hovered && (input.Wheel.x != 0.0f || input.Wheel.y != 0.0f))
		{
			if (m_Hover.Kind == Kind::Minimap)
			{
				// Over the minimap the wheel zooms about the view's centre, which the minimap shows.
				m_View.ZoomByWheel(input.Wheel.y, m_View.GetScreenCenter());
			}
			else
			{
				m_View.Scroll(input.Wheel, input.Mouse, m_Settings.GetScrollMode(), input.Ctrl, input.Shift);
			}
		}
		bool const idle = m_Gesture == VisualScriptGesture::Idle && !m_Press.Active;
		if (m_Gesture != VisualScriptGesture::Idle)
		{
			ContinueGesture(input, deltaTime, intents);
		}
		else if (m_Press.Active)
		{
			ContinuePress(input, deltaTime, intents);
		}
		if (idle && m_Gesture == VisualScriptGesture::Idle)
		{
			BeginPress(input, intents);
		}
	}

	void VisualScriptEditorModel::UpdateDrop(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		if (!input.Drop || IsReadOnly())
		{
			return;
		}
		VisualScriptDropIntent drop;
		drop.Kind = input.Drop->Kind;
		drop.Payload = input.Drop->Payload;
		drop.Entities = input.Drop->Entities;
		drop.Position = m_View.ToCanvas(input.Mouse);
		drop.Modifiers =
			static_cast<uint8_t>((input.Ctrl ? VisualScriptDropModifiers::Ctrl : 0u) |
		                         (input.Shift ? VisualScriptDropModifiers::Shift : 0u) | (input.Alt ? VisualScriptDropModifiers::Alt : 0u));
		VisualScriptHit const hit = HitTest(input.Mouse);
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		if (hit.Kind == Kind::Pin && hit.Pin < pins.size())
		{
			VisualScriptPinVisual const& pin = pins[hit.Pin];
			drop.Pin = VisualScriptPinReference{hit.Element, std::string(m_Canvas.GetText(pin.Name))};
			drop.Direction = pin.Direction;
		}
		intents.emplace_back(std::move(drop));
	}

	// --- Presses and clicks ---

	void VisualScriptEditorModel::BeginPress(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		if (!input.Hovered || input.WidgetActive)
		{
			return;
		}
		int button = -1;
		for (int candidate = 0; candidate < 3; candidate++)
		{
			if (input.Pressed[static_cast<size_t>(candidate)])
			{
				button = candidate;
				break;
			}
		}
		if (button < 0)
		{
			return;
		}
		m_Press.Active = true;
		m_Press.Button = button;
		m_Press.Kind = PressKind::Inert;
		m_Press.Screen = input.Mouse;
		m_Press.Canvas = m_View.ToCanvas(input.Mouse);
		m_Press.MaxDistance = 0.0f;
		m_Press.Hit = m_Hover;
		m_Press.Ctrl = input.Ctrl;
		m_Press.Shift = input.Shift;
		m_Press.Alt = input.Alt;
		m_Press.DecideOnRelease = false;
		m_Press.PinLinked = false;
		if (button == 2)
		{
			StartPan(2, input.Mouse);
			return;
		}
		if (button == 1)
		{
			m_Press.Kind = PressKind::Right;
			if (m_Hover.Kind == Kind::Pin)
			{
				KeepPressedPin();
			}
			return;
		}
		if (m_SpaceHeld)
		{
			// Space + left drag pans; the press neither selects nor starts a marquee.
			StartPan(0, input.Mouse);
			return;
		}
		if (m_Hover.Kind == Kind::Minimap)
		{
			StartMinimapDrag(input.Mouse);
			return;
		}
		if (input.DoubleClicked[0] && HandleDoubleClick(input, intents))
		{
			return;
		}
		switch (m_Hover.Kind)
		{
			case Kind::Pin:
				PressPin(input, intents);
				return;
			case Kind::Badge:
			case Kind::NodeHeader:
			case Kind::Node:
			case Kind::Reroute:
			case Kind::CommentTitle:
				PressElement(m_Hover.Element, input);
				m_Press.Kind = PressKind::Element;
				return;
			case Kind::CommentEdge:
				PressElement(m_Hover.Element, input);
				m_Press.Kind = PressKind::CommentEdge;
				return;
			case Kind::Link:
				m_Press.Kind = input.Alt ? PressKind::AltLink : PressKind::Background;
				return;
			case Kind::None:
			case Kind::Background:
				m_Press.Kind = PressKind::Background;
				return;
			case Kind::Minimap:
				return;
		}
	}

	bool VisualScriptEditorModel::HandleDoubleClick(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		VisualScriptHit const& hit = m_Hover;
		switch (hit.Kind)
		{
			case Kind::Link:
			{
				// A reroute where the wire was clicked.
				if (IsReadOnly() || !hit.Link)
				{
					return false;
				}
				if (m_Library == nullptr)
				{
					m_RefusedCount++;
					return true;
				}
				std::optional<uint32_t> const link = FindCanvasLink(*hit.Link);
				if (!link)
				{
					return false;
				}
				std::array<glm::vec2, 4> const points = VisualScriptGeometry::GetLinkControlPoints(m_Canvas, *link, GetNodePreview());
				std::array<glm::vec2, 4> const screen = {m_View.ToScreen(points[0]), m_View.ToScreen(points[1]), m_View.ToScreen(points[2]),
				                                         m_View.ToScreen(points[3])};
				VisualScriptGeometry::WireFlattening const flattening =
					VisualScriptGeometry::GetWireFlattening(m_View.GetDetailLevel(), m_CurveTolerance);
				VisualScriptGeometry::FlattenWire(screen, flattening.Tolerance, flattening.SegmentCap, m_Polyline);
				glm::vec2 const center = m_View.ToCanvas(VisualScriptGeometry::GetPolylinePoint(m_Polyline, hit.ArcFraction));
				glm::vec2 position = center - glm::vec2(Metrics::RerouteRadius);
				if (m_Settings.IsSnapToGrid() != input.Alt)
				{
					position = SnapToGrid(position);
				}
				VisualScriptInsertRerouteIntent reroute;
				reroute.Link = *hit.Link;
				reroute.Position = position;
				intents.emplace_back(std::move(reroute));
				return true;
			}
			case Kind::NodeHeader:
			{
				// Opens the node: a function call shows its function, a sender goes to its event, the others are the controller's
				// (a variable's Get or Set selects the variable in My Script, the rest show the node in Details).
				VisualScriptNode const* node = FindDrawnNode(hit.Element);
				if (node == nullptr)
				{
					return false;
				}
				if (node->Type == "Function.Call")
				{
					std::optional<uint32_t> const function = ReadIdSetting(*node, "Function");
					if (function && *function != 0 && HasFunction(*function))
					{
						intents.emplace_back(VisualScriptOpenFunctionIntent{*function});
						return true;
					}
				}
				if (node->Type == "Event.Send" && GoToCustomEvent(*node))
				{
					return true;
				}
				MakePrimary(hit.Element);
				VisualScriptActionIntent open;
				open.Action = Action::OpenNode;
				open.Selection = m_Selection;
				open.Position = m_View.ToCanvas(input.Mouse);
				intents.emplace_back(std::move(open));
				return true;
			}
			case Kind::Node:
			{
				// The node comment's bubble: its text is edited in place.
				std::optional<uint32_t> const index = m_Canvas.FindNode(hit.Element);
				if (!index || IsReadOnly())
				{
					return false;
				}
				VisualScriptRect const bubble = VisualScriptGeometry::GetBubbleRect(m_Canvas.GetNodes()[*index]);
				if (VisualScriptGeometry::IsEmpty(bubble) || !VisualScriptGeometry::Contains(bubble, m_View.ToCanvas(input.Mouse)))
				{
					return false;
				}
				MakePrimary(hit.Element);
				VisualScriptActionIntent edit;
				edit.Action = Action::EditNodeComment;
				edit.Selection = m_Selection;
				edit.Position = m_View.ToCanvas(input.Mouse);
				intents.emplace_back(std::move(edit));
				return true;
			}
			case Kind::CommentTitle:
			{
				if (IsReadOnly())
				{
					return false;
				}
				MakePrimary(hit.Element);
				VisualScriptActionIntent rename;
				rename.Action = Action::Rename;
				rename.Selection = m_Selection;
				rename.Position = m_View.ToCanvas(input.Mouse);
				intents.emplace_back(std::move(rename));
				return true;
			}
			case Kind::None:
			case Kind::Minimap:
			case Kind::Pin:
			case Kind::Badge:
			case Kind::Reroute:
			case Kind::CommentEdge:
			case Kind::Background:
				return false;
		}
		return false;
	}

	void VisualScriptEditorModel::PressElement(uint32_t element, VisualScriptCanvasInput const& input)
	{
		if (m_Canvas.FindNode(element))
		{
			Raise(element);
		}
		m_Press.DecideOnRelease = false;
		if (!IsSelected(element))
		{
			// An element joins the selection at the press, so a drag moves it with the others.
			uint32_t const elements[] = {element};
			Select(elements, input.Ctrl    ? VisualScriptSelectMode::Toggle
			                 : input.Shift ? VisualScriptSelectMode::Add
			                               : VisualScriptSelectMode::Replace);
			return;
		}
		// A selected element: a drag keeps the selection, a click without Shift decides on release.
		MakePrimary(element);
		m_Press.DecideOnRelease = !input.Shift;
	}

	bool VisualScriptEditorModel::KeepPressedPin()
	{
		std::optional<uint32_t> const nodeIndex = m_Canvas.FindNode(m_Hover.Element);
		std::span<VisualScriptPinVisual const> const allPins = m_Canvas.GetPins();
		if (!nodeIndex || m_Hover.Pin >= allPins.size())
		{
			return false;
		}
		VisualScriptPinVisual const& pin = allPins[m_Hover.Pin];
		m_Press.PinNode = m_Hover.Element;
		m_Press.Pin.assign(m_Canvas.GetText(pin.Name));
		m_Press.Direction = pin.Direction;
		m_Press.PinLinked = pin.HasFlag(VisualScriptPinFlags::Linked);
		return true;
	}

	void VisualScriptEditorModel::PressPin(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		m_Press.Kind = PressKind::Inert;
		if (IsReadOnly() || !KeepPressedPin())
		{
			return;
		}
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		VisualScriptNodeVisual const& node = nodes[*m_Canvas.FindNode(m_Hover.Element)];
		VisualScriptPinVisual const& pin = m_Canvas.GetPins()[m_Hover.Pin];
		if (input.Alt)
		{
			m_Press.Kind = PressKind::AltPin;
			return;
		}
		if (IsCollapsedBox(node))
		{
			// The anchor stands for every pin of its side: with more than one, the user picks which the wire starts from.
			std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins(node);
			size_t const side = static_cast<size_t>(std::count_if(pins.begin(), pins.end(),
			                                                      [&pin](VisualScriptPinVisual const& candidate)
			                                                      {
																	  return candidate.Direction == pin.Direction &&
				                                                             !candidate.HasFlag(VisualScriptPinFlags::Orphan);
																  }));
			if (side > 1)
			{
				if (m_Library == nullptr)
				{
					m_RefusedCount++;
					return;
				}
				VisualScriptPinMenuIntent menu;
				menu.Node = node.ID;
				menu.Direction = pin.Direction;
				menu.ScreenPosition = input.Mouse;
				intents.emplace_back(menu);
				return;
			}
		}
		m_Press.Kind = PressKind::Pin;
	}

	void VisualScriptEditorModel::ContinuePress(VisualScriptCanvasInput const& input, double deltaTime,
	                                            std::vector<VisualScriptEditorIntent>& intents)
	{
		size_t const button = static_cast<size_t>(m_Press.Button);
		m_Press.MaxDistance = std::max(m_Press.MaxDistance, glm::length(input.Mouse - m_Press.Screen));
		if (m_Press.MaxDistance > Metrics::ClickDragThresholdPixels)
		{
			StartDrag(input);
			if (m_Gesture != VisualScriptGesture::Idle)
			{
				ContinueGesture(input, deltaTime, intents);
				return;
			}
		}
		if (input.Released[button])
		{
			if (m_Press.MaxDistance <= Metrics::ClickDragThresholdPixels)
			{
				Click(input, intents);
			}
			m_Press.Active = false;
		}
		else if (!input.Down[button])
		{
			// The button went up unseen (the window lost the mouse): no click.
			m_Press.Active = false;
		}
	}

	void VisualScriptEditorModel::StartDrag(VisualScriptCanvasInput const& input)
	{
		switch (m_Press.Kind)
		{
			case PressKind::Inert:
			case PressKind::AltPin:
			case PressKind::AltLink:
				m_Press.Kind = PressKind::Inert;
				return;
			case PressKind::Background:
				m_Gesture = VisualScriptGesture::Marquee;
				m_GestureButton = 0;
				m_GestureCanvas = m_Press.Canvas;
				m_Press.Active = false;
				return;
			case PressKind::Element:
				if (IsReadOnly())
				{
					m_Press.Kind = PressKind::Inert;
					return;
				}
				StartMove(input);
				return;
			case PressKind::CommentEdge:
				if (IsReadOnly())
				{
					m_Press.Kind = PressKind::Inert;
					return;
				}
				StartResize();
				return;
			case PressKind::Pin:
			{
				if (m_Library == nullptr)
				{
					// Wiring needs the pins' types, which only the scripting runtime has.
					m_RefusedCount++;
					m_Press.Kind = PressKind::Inert;
					return;
				}
				if (m_Press.Ctrl && m_Press.PinLinked)
				{
					StartRelink();
					if (m_Gesture == VisualScriptGesture::Relinking)
					{
						return;
					}
				}
				PinKey key;
				key.Node = m_Press.PinNode;
				key.Pin = m_Press.Pin;
				key.Direction = m_Press.Direction;
				if (!BeginWire(key, false))
				{
					m_Press.Kind = PressKind::Inert;
				}
				return;
			}
			case PressKind::Right:
				if (m_Press.Ctrl && !IsReadOnly())
				{
					m_Gesture = VisualScriptGesture::Cutting;
					m_GestureButton = 1;
					m_GestureCanvas = m_Press.Canvas;
					m_Press.Active = false;
					return;
				}
				StartPan(1, m_Press.Screen);
				return;
		}
	}

	void VisualScriptEditorModel::Click(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		switch (m_Press.Kind)
		{
			case PressKind::Background:
				if (!m_Press.Shift && !m_Press.Ctrl)
				{
					SetSelection({});
				}
				return;
			case PressKind::Element:
				if (m_Press.DecideOnRelease)
				{
					uint32_t const elements[] = {m_Press.Hit.Element};
					Select(elements, m_Press.Ctrl ? VisualScriptSelectMode::Toggle : VisualScriptSelectMode::Replace);
				}
				return;
			case PressKind::AltPin:
			{
				std::optional<uint32_t> const nodeIndex = m_Canvas.FindNode(m_Press.PinNode);
				if (IsReadOnly() || !nodeIndex)
				{
					return;
				}
				std::optional<uint32_t> const pinIndex = m_Canvas.FindPin(*nodeIndex, m_Press.Pin, m_Press.Direction);
				if (!pinIndex)
				{
					return;
				}
				VisualScriptDisconnectIntent disconnect;
				disconnect.Links = GetPinLinks(*nodeIndex, *pinIndex, IsCollapsedBox(m_Canvas.GetNodes()[*nodeIndex]));
				if (!disconnect.Links.empty())
				{
					intents.emplace_back(std::move(disconnect));
				}
				return;
			}
			case PressKind::AltLink:
				if (!IsReadOnly() && m_Press.Hit.Link)
				{
					VisualScriptDisconnectIntent disconnect;
					disconnect.Links.push_back(*m_Press.Hit.Link);
					intents.emplace_back(std::move(disconnect));
				}
				return;
			case PressKind::Right:
				RightClick(input, intents);
				return;
			case PressKind::Inert:
			case PressKind::CommentEdge:
			case PressKind::Pin:
				return;
		}
	}

	void VisualScriptEditorModel::RightClick(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		// The menu is of what was under the mouse at the press, though the release may be a few pixels away.
		VisualScriptHit const target = m_Press.Hit;
		switch (target.Kind)
		{
			case Kind::Badge:
			case Kind::NodeHeader:
			case Kind::Node:
			case Kind::Reroute:
			case Kind::CommentTitle:
			case Kind::CommentEdge:
				if (!IsSelected(target.Element))
				{
					// A right click on an unselected element makes it the selection; on a selected one the menu acts on all.
					uint32_t const elements[] = {target.Element};
					Select(elements, VisualScriptSelectMode::Replace);
				}
				[[fallthrough]];
			case Kind::Link:
			{
				VisualScriptContextMenuIntent menu;
				menu.Target = target;
				menu.ScreenPosition = input.Mouse;
				intents.emplace_back(std::move(menu));
				return;
			}
			case Kind::Pin:
			{
				// The pin pressed, found again by name: a layout since the press may have moved pin indices. A pin that is gone
				// has no menu.
				std::optional<std::pair<uint32_t, uint32_t>> const pin =
					FindCanvasPin(PinKey{m_Press.PinNode, m_Press.Pin, m_Press.Direction});
				if (!pin)
				{
					return;
				}
				std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
				VisualScriptContextMenuIntent menu;
				menu.Target = target;
				menu.Target.Pin = pin->second;
				menu.ScreenPosition = input.Mouse;
				menu.Pin = VisualScriptPinReference{m_Press.PinNode, m_Press.Pin};
				menu.PinDirection = m_Press.Direction;
				menu.WholeSide = IsCollapsedBox(nodes[pin->first]);
				intents.emplace_back(std::move(menu));
				return;
			}
			case Kind::Background:
				OpenPalette(m_Press.Canvas, {}, intents);
				return;
			case Kind::None:
			case Kind::Minimap:
				return;
		}
	}

	// --- Gestures ---

	void VisualScriptEditorModel::ContinueGesture(VisualScriptCanvasInput const& input, double deltaTime,
	                                              std::vector<VisualScriptEditorIntent>& intents)
	{
		size_t const button = static_cast<size_t>(m_GestureButton);
		switch (m_Gesture)
		{
			case VisualScriptGesture::Panning:
				m_View.Pan(input.Mouse - m_PanLast);
				m_PanLast = input.Mouse;
				if (input.Released[button] || !input.Down[button])
				{
					EndGesture();
				}
				return;
			case VisualScriptGesture::MinimapDrag:
				CenterOnMinimap(input.Mouse);
				if (input.Released[button] || !input.Down[button])
				{
					EndGesture();
				}
				return;
			case VisualScriptGesture::Marquee:
				ContinueMarquee(input, deltaTime);
				return;
			case VisualScriptGesture::MovingElements:
				ContinueMove(input, deltaTime, intents);
				return;
			case VisualScriptGesture::ResizingComment:
				ContinueResize(input, deltaTime, intents);
				return;
			case VisualScriptGesture::Wiring:
				ContinueWire(input, deltaTime, intents);
				return;
			case VisualScriptGesture::Relinking:
				ContinueRelink(input, deltaTime, intents);
				return;
			case VisualScriptGesture::Cutting:
				ContinueCut(input, intents);
				return;
			case VisualScriptGesture::Idle:
				return;
		}
	}

	void VisualScriptEditorModel::StartPan(int button, glm::vec2 from)
	{
		m_Gesture = VisualScriptGesture::Panning;
		m_GestureButton = button;
		m_PanLast = from;
		m_Press.Active = false;
	}

	void VisualScriptEditorModel::StartMinimapDrag(glm::vec2 mouse)
	{
		m_Gesture = VisualScriptGesture::MinimapDrag;
		m_GestureButton = 0;
		m_Press.Active = false;
		CenterOnMinimap(mouse);
	}

	void VisualScriptEditorModel::CenterOnMinimap(glm::vec2 mouse)
	{
		if (!(m_Minimap.Transform.Scale > 0.0f))
		{
			return;
		}
		VisualScriptViewState view = m_View.GetState();
		view.Center = m_Minimap.Transform.ToCanvas(mouse);
		m_View.SetState(view);
	}

	void VisualScriptEditorModel::ContinueMarquee(VisualScriptCanvasInput const& input, double deltaTime)
	{
		if (!input.Down[0] && !input.Released[0])
		{
			CancelGesture();
			return;
		}
		AutoPan(input, deltaTime);
		m_GestureCanvas = m_View.ToCanvas(input.Mouse);
		// Nodes the marquee touches, comments it holds entirely (so one started inside a comment never takes that comment).
		VisualScriptRect const rect = MakeRect(m_Press.Canvas, m_GestureCanvas);
		m_MarqueeElements.clear();
		m_Index.Query(rect, VisualScriptSpatialIndex::NodeMask | VisualScriptSpatialIndex::CommentMask, m_Query);
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptCommentVisual const> const comments = m_Canvas.GetComments();
		for (uint32_t const item : m_Query)
		{
			uint32_t const index = VisualScriptSpatialIndex::GetIndex(item);
			if (VisualScriptSpatialIndex::GetKind(item) == VisualScriptSpatialIndex::ItemKind::Node)
			{
				if (VisualScriptGeometry::Intersects(VisualScriptGeometry::GetNodeRect(nodes[index]), rect))
				{
					m_MarqueeElements.push_back(nodes[index].ID);
				}
			}
			else if (ContainsRect(rect, comments[index].Rect))
			{
				m_MarqueeElements.push_back(comments[index].ID);
			}
		}
		std::sort(m_MarqueeElements.begin(), m_MarqueeElements.end());
		if (input.Released[0])
		{
			if (input.Ctrl)
			{
				Select(m_MarqueeElements, VisualScriptSelectMode::Remove);
			}
			else if (input.Shift)
			{
				Select(m_MarqueeElements, VisualScriptSelectMode::Add);
			}
			else
			{
				std::vector<uint32_t> selection = m_MarqueeElements;
				if (!m_Selection.empty())
				{
					// The primary element stays primary when the marquee holds it.
					auto const primary = std::find(selection.begin(), selection.end(), m_Selection.front());
					if (primary != selection.end())
					{
						std::rotate(selection.begin(), primary, primary + 1);
					}
				}
				SetSelection(std::move(selection));
			}
			EndGesture();
		}
	}

	void VisualScriptEditorModel::StartMove(VisualScriptCanvasInput const& input)
	{
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptCommentVisual const> const comments = m_Canvas.GetComments();
		m_Move.Elements = m_Selection;
		m_Move.Nodes.clear();
		m_Move.Comments = 0;
		m_Move.Offset = glm::vec2(0.0f);
		m_Move.SpliceCandidate.reset();
		m_Move.SpliceTarget.reset();
		std::vector<uint8_t> nodeMoved(nodes.size(), 0);
		std::vector<uint8_t> commentMoved(comments.size(), 0);
		for (uint32_t const element : m_Move.Elements)
		{
			if (std::optional<uint32_t> const node = m_Canvas.FindNode(element))
			{
				nodeMoved[*node] = 1;
			}
			else if (std::optional<uint32_t> const comment = m_Canvas.FindComment(element))
			{
				commentMoved[*comment] = 1;
			}
		}
		// A moved comment takes what lies inside it when the drag starts (nested comments with theirs), unless the user's
		// toggle says otherwise; Alt inverts the toggle for this drag.
		if (m_Settings.IsMoveCommentContents() != m_Press.Alt)
		{
			for (size_t element = 0; element < m_Move.Elements.size(); element++)
			{
				std::optional<uint32_t> const comment = m_Canvas.FindComment(m_Move.Elements[element]);
				if (!comment)
				{
					continue;
				}
				VisualScriptRect const frame = comments[*comment].Rect;
				m_Index.Query(frame, VisualScriptSpatialIndex::NodeMask | VisualScriptSpatialIndex::CommentMask, m_Query);
				for (uint32_t const item : m_Query)
				{
					uint32_t const index = VisualScriptSpatialIndex::GetIndex(item);
					if (VisualScriptSpatialIndex::GetKind(item) == VisualScriptSpatialIndex::ItemKind::Node)
					{
						if (!nodeMoved[index] && ContainsRect(frame, VisualScriptGeometry::GetNodeRect(nodes[index])))
						{
							nodeMoved[index] = 1;
							m_Move.Elements.push_back(nodes[index].ID);
						}
					}
					else if (!commentMoved[index] && ContainsRect(frame, comments[index].Rect))
					{
						commentMoved[index] = 1;
						m_Move.Elements.push_back(comments[index].ID);
					}
				}
			}
		}
		for (uint32_t const element : m_Move.Elements)
		{
			if (m_Canvas.FindNode(element))
			{
				m_Move.Nodes.push_back(element);
			}
			else
			{
				m_Move.Comments++;
			}
		}
		m_Move.PrimaryOrigin = m_Press.Canvas;
		if (!m_Selection.empty())
		{
			if (std::optional<uint32_t> const node = m_Canvas.FindNode(m_Selection.front()))
			{
				m_Move.PrimaryOrigin = nodes[*node].Position;
			}
			else if (std::optional<uint32_t> const comment = m_Canvas.FindComment(m_Selection.front()))
			{
				m_Move.PrimaryOrigin = comments[*comment].Rect.Min;
			}
		}
		m_Move.Snap = m_Settings.IsSnapToGrid() != input.Alt;
		m_Gesture = VisualScriptGesture::MovingElements;
		m_GestureButton = 0;
		m_Press.Active = false;
		RebuildDynamic();
	}

	void VisualScriptEditorModel::ContinueMove(VisualScriptCanvasInput const& input, double deltaTime,
	                                           std::vector<VisualScriptEditorIntent>& intents)
	{
		if (!input.Down[0] && !input.Released[0])
		{
			CancelGesture();
			return;
		}
		AutoPan(input, deltaTime);
		glm::vec2 const delta = m_View.ToCanvas(input.Mouse) - m_Press.Canvas;
		// Snap to Grid, inverted while Alt is held: the primary element's top-left lands on the grid.
		m_Move.Snap = m_Settings.IsSnapToGrid() != input.Alt;
		m_Move.Offset = m_Move.Snap ? SnapToGrid(m_Move.PrimaryOrigin + delta) - m_Move.PrimaryOrigin : delta;
		if (!IsFinite(m_Move.Offset))
		{
			m_Move.Offset = glm::vec2(0.0f);
		}
		FindSpliceTarget(input);
		if (!input.Released[0])
		{
			return;
		}
		if (m_Move.SpliceTarget)
		{
			VisualScriptSpliceIntent splice;
			splice.Node = m_Move.Nodes.front();
			splice.Link = *m_Move.SpliceTarget;
			splice.InputPin = m_Move.SpliceInput;
			splice.OutputPin = m_Move.SpliceOutput;
			splice.Offset = m_Move.Offset;
			intents.emplace_back(std::move(splice));
		}
		else if (m_Move.Offset != glm::vec2(0.0f) && !m_Move.Elements.empty())
		{
			VisualScriptMoveIntent move;
			move.Elements = m_Move.Elements;
			move.Offset = m_Move.Offset;
			move.Snap = m_Move.Snap;
			intents.emplace_back(std::move(move));
		}
		EndGesture();
	}

	void VisualScriptEditorModel::FindSpliceTarget(VisualScriptCanvasInput const& input)
	{
		// One unlinked node, dragged without Alt, resting HoverExpandSeconds over a wire it fits into: an input the wire's
		// source connects to and an output that connects to its target.
		uint32_t nodeIndex = 0;
		bool eligible = m_Move.Nodes.size() == 1 && m_Move.Comments == 0 && m_Library != nullptr && !input.Alt;
		if (eligible)
		{
			std::optional<uint32_t> const found = m_Canvas.FindNode(m_Move.Nodes.front());
			eligible = found.has_value() && m_Canvas.GetIncidentLinks(*found).empty();
			nodeIndex = found.value_or(0);
		}
		VisualScriptHit const hit = eligible ? HitTest(input.Mouse, true) : VisualScriptHit{};
		if (hit.Kind != Kind::Link || !hit.Link)
		{
			m_Move.SpliceCandidate.reset();
			m_Move.SpliceTarget.reset();
			return;
		}
		if (!m_Move.SpliceCandidate || !(*m_Move.SpliceCandidate == *hit.Link))
		{
			m_Move.SpliceCandidate = *hit.Link;
			m_Move.SpliceSince = input.Time;
			m_Move.SpliceTarget.reset();
			return;
		}
		if (m_Move.SpliceTarget || input.Time - m_Move.SpliceSince < Metrics::HoverExpandSeconds)
		{
			return;
		}
		VisualScriptLink const& link = *hit.Link;
		VisualScriptPin const* source = FindPinInfo(link.From.Node, link.From.Pin, VisualScriptPinDirection::Out);
		VisualScriptPin const* target = FindPinInfo(link.To.Node, link.To.Pin, VisualScriptPinDirection::In);
		if (source == nullptr || target == nullptr)
		{
			return;
		}
		// The node's first input the wire's source connects to and first output that connects to its target, exec pins first.
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		VisualScriptNodeVisual const& node = nodes[nodeIndex];
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins(node);
		std::optional<std::string> inputPin;
		std::optional<std::string> outputPin;
		for (int pass = 0; pass < 2; pass++)
		{
			bool const exec = pass == 0;
			for (VisualScriptPinVisual const& pin : pins)
			{
				std::string_view const name = m_Canvas.GetText(pin.Name);
				VisualScriptPin const* info = FindPinInfo(node.ID, name, pin.Direction);
				if (info == nullptr || IsExecPin(info) != exec)
				{
					continue;
				}
				if (pin.Direction == VisualScriptPinDirection::In && !inputPin && m_Library->CanConnect(*source, *info))
				{
					inputPin = std::string(name);
				}
				else if (pin.Direction == VisualScriptPinDirection::Out && !outputPin && m_Library->CanConnect(*info, *target))
				{
					outputPin = std::string(name);
				}
			}
		}
		if (inputPin && outputPin)
		{
			m_Move.SpliceTarget = link;
			m_Move.SpliceInput = *inputPin;
			m_Move.SpliceOutput = *outputPin;
		}
	}

	void VisualScriptEditorModel::StartResize()
	{
		std::optional<uint32_t> const comment = m_Canvas.FindComment(m_Press.Hit.Element);
		if (!comment || m_Press.Hit.Edges == 0)
		{
			m_Press.Kind = PressKind::Inert;
			return;
		}
		m_Resize.Comment = m_Press.Hit.Element;
		m_Resize.Original = m_Canvas.GetComments()[*comment].Rect;
		m_Resize.Rect = m_Resize.Original;
		m_Resize.Edges = m_Press.Hit.Edges;
		m_Gesture = VisualScriptGesture::ResizingComment;
		m_GestureButton = 0;
		m_Press.Active = false;
		RebuildDynamic();
	}

	void VisualScriptEditorModel::ContinueResize(VisualScriptCanvasInput const& input, double deltaTime,
	                                             std::vector<VisualScriptEditorIntent>& intents)
	{
		if (!input.Down[0] && !input.Released[0])
		{
			CancelGesture();
			return;
		}
		AutoPan(input, deltaTime);
		glm::vec2 const delta = m_View.ToCanvas(input.Mouse) - m_Press.Canvas;
		bool const snap = m_Settings.IsSnapToGrid() != input.Alt;
		auto const place = [snap](float value)
		{
			return snap ? SnapToGrid(value) : value;
		};
		VisualScriptRect rect = m_Resize.Original;
		uint8_t const edges = m_Resize.Edges;
		if ((edges & VisualScriptGeometry::LeftEdge) != 0)
		{
			rect.Min.x = place(rect.Min.x + delta.x);
		}
		if ((edges & VisualScriptGeometry::RightEdge) != 0)
		{
			rect.Max.x = place(rect.Max.x + delta.x);
		}
		if ((edges & VisualScriptGeometry::TopEdge) != 0)
		{
			rect.Min.y = place(rect.Min.y + delta.y);
		}
		if ((edges & VisualScriptGeometry::BottomEdge) != 0)
		{
			rect.Max.y = place(rect.Max.y + delta.y);
		}
		// The canvas never makes a comment smaller than its minimum: the dragged edge stops there.
		if (rect.Max.x - rect.Min.x < Metrics::CommentMinWidth)
		{
			if ((edges & VisualScriptGeometry::LeftEdge) != 0)
			{
				rect.Min.x = rect.Max.x - Metrics::CommentMinWidth;
			}
			else
			{
				rect.Max.x = rect.Min.x + Metrics::CommentMinWidth;
			}
		}
		if (rect.Max.y - rect.Min.y < Metrics::CommentMinHeight)
		{
			if ((edges & VisualScriptGeometry::TopEdge) != 0)
			{
				rect.Min.y = rect.Max.y - Metrics::CommentMinHeight;
			}
			else
			{
				rect.Max.y = rect.Min.y + Metrics::CommentMinHeight;
			}
		}
		if (IsFinite(rect.Min) && IsFinite(rect.Max))
		{
			m_Resize.Rect = rect;
		}
		if (!input.Released[0])
		{
			return;
		}
		if (!(m_Resize.Rect == m_Resize.Original))
		{
			VisualScriptResizeCommentIntent resize;
			resize.Comment = m_Resize.Comment;
			resize.Rect = m_Resize.Rect;
			intents.emplace_back(resize);
		}
		EndGesture();
	}

	bool VisualScriptEditorModel::BeginWire(PinKey const& source, bool endsOnPress)
	{
		if (IsReadOnly())
		{
			return false;
		}
		if (m_Library != nullptr)
		{
			// The wire is classed against analyzed pins: a document changed since the last analysis is analyzed now.
			ForceAnalysis();
		}
		if (m_Library == nullptr)
		{
			m_RefusedCount++;
			return false;
		}
		VisualScriptPin const* info = FindPinInfo(source);
		if (!FindCanvasPin(source) || info == nullptr)
		{
			return false;
		}
		m_Wire.Source = source;
		m_Wire.SourceReference = {source.Node, source.Pin};
		m_Wire.SourceInfo = info;
		m_Wire.EndsOnPress = endsOnPress;
		m_Wire.CanvasRevision = m_CanvasRevision;
		m_Wire.Classes.assign(m_Canvas.GetPins().size(), Unclassified);
		m_Wire.Shown.assign(m_Canvas.GetPins().size(), static_cast<uint8_t>(VisualScriptPinClass::None));
		m_Wire.Snapped.reset();
		m_Wire.Resting.reset();
		m_Wire.Expanded.reset();
		m_Wire.Invalid = false;
		m_Gesture = VisualScriptGesture::Wiring;
		m_GestureButton = 0;
		m_Press.Active = false;
		return true;
	}

	void VisualScriptEditorModel::ContinueWire(VisualScriptCanvasInput const& input, double deltaTime,
	                                           std::vector<VisualScriptEditorIntent>& intents)
	{
		// Escape and a right click cancel; a dragged wire whose button went up unseen too.
		if (input.Pressed[1] || (!m_Wire.EndsOnPress && !input.Down[0] && !input.Released[0]))
		{
			CancelGesture();
			return;
		}
		if (!m_Wire.EndsOnPress)
		{
			AutoPan(input, deltaTime);
		}
		if (!FindCanvasPin(m_Wire.Source))
		{
			CancelGesture();
			return;
		}
		if (m_Wire.CanvasRevision != m_CanvasRevision)
		{
			// Laid out anew (the fonts changed): the pins are classed again by their new indices.
			m_Wire.Classes.assign(m_Canvas.GetPins().size(), Unclassified);
			m_Wire.Shown.assign(m_Canvas.GetPins().size(), static_cast<uint8_t>(VisualScriptPinClass::None));
			m_Wire.CanvasRevision = m_CanvasRevision;
			m_Wire.Snapped.reset();
		}
		UpdateExpandedNode();
		ClassifyVisiblePins();
		SetWireSnap(FindSnapPin(input.Mouse, false));
		UpdateGhostWire(input);
		bool const finished = m_Wire.EndsOnPress ? input.Pressed[0] && input.Hovered : input.Released[0];
		if (finished)
		{
			FinishWire(input, intents);
			EndGesture();
		}
	}

	void VisualScriptEditorModel::UpdateExpandedNode()
	{
		// A collapsed node under the dragged wire, other than the wire's own: after resting HoverExpandSeconds over it, it is
		// drawn and hit expanded until the wire leaves it.
		std::optional<uint32_t> over;
		if (IsNodeHit(m_Hover.Kind) && m_Hover.Element != m_Wire.Source.Node)
		{
			if (std::optional<uint32_t> const index = m_Canvas.FindNode(m_Hover.Element))
			{
				if (IsCollapsedBox(m_Canvas.GetNodes()[*index]))
				{
					over = m_Hover.Element;
				}
			}
		}
		if (m_Wire.Expanded && over != m_Wire.Expanded)
		{
			m_Wire.Expanded.reset();
			RebuildDynamic();
		}
		if (!over || m_Wire.Expanded)
		{
			m_Wire.Resting.reset();
			return;
		}
		if (m_Wire.Resting != over)
		{
			m_Wire.Resting = over;
			m_Wire.RestingSince = m_Time;
		}
		else if (m_Time - m_Wire.RestingSince >= Metrics::HoverExpandSeconds)
		{
			m_Wire.Expanded = over;
			m_Wire.Resting.reset();
			RebuildDynamic();
		}
	}

	void VisualScriptEditorModel::UpdateGhostWire(VisualScriptCanvasInput const& input)
	{
		std::optional<std::pair<uint32_t, uint32_t>> const source = FindCanvasPin(m_Wire.Source);
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		VisualScriptPinVisual const& sourcePin = pins[source->second];
		glm::vec2 const anchor = VisualScriptGeometry::GetPinAnchor(nodes[source->first], sourcePin);
		glm::vec2 end = m_View.ToCanvas(input.Mouse);
		uint32_t targetColor = sourcePin.Color;
		m_Wire.Invalid = false;
		VisualScriptNodePreview const preview = GetNodePreview();
		if (m_Wire.Snapped)
		{
			// The pin's node, as drawn (the expanded one with its rows).
			if (std::optional<uint32_t> const node = FindPinNode(*m_Wire.Snapped))
			{
				VisualScriptNodeVisual const drawn =
					preview.Expanded == node ? VisualScriptGeometry::GetExpandedNode(nodes[*node]) : nodes[*node];
				end = VisualScriptGeometry::GetPinAnchor(drawn, pins[*m_Wire.Snapped]);
			}
			targetColor = pins[*m_Wire.Snapped].Color;
		}
		else if (m_Hover.Kind == Kind::Pin && m_Hover.Pin < pins.size())
		{
			// Over a pin it does not connect to: invalid, unless a Convert bridges the two.
			std::optional<uint32_t> const nodeIndex = m_Canvas.FindNode(m_Hover.Element);
			if (nodeIndex && ClassifyPin(*nodeIndex, m_Hover.Pin) == static_cast<uint8_t>(VisualScriptPinClass::Incompatible))
			{
				VisualScriptPinVisual const& over = pins[m_Hover.Pin];
				VisualScriptPin const* info = FindPinInfo(m_Hover.Element, m_Canvas.GetText(over.Name), over.Direction);
				bool const out = m_Wire.Source.Direction == VisualScriptPinDirection::Out;
				std::optional<std::string> const bridge =
					info != nullptr && m_Hover.Element != m_Wire.Source.Node && over.Direction != m_Wire.Source.Direction
						? FindConvertBridge(*m_Library, out ? *m_Wire.SourceInfo : *info, out ? *info : *m_Wire.SourceInfo)
						: std::nullopt;
				m_Wire.Invalid = !bridge;
				if (bridge)
				{
					targetColor = over.Color;
				}
			}
		}
		if (m_Wire.Source.Direction == VisualScriptPinDirection::Out)
		{
			m_Wire.Ghost = VisualScriptGeometry::GetWireControlPoints(anchor, end, {});
			m_Wire.ColorFrom = sourcePin.Color;
			m_Wire.ColorTo = targetColor;
		}
		else
		{
			m_Wire.Ghost = VisualScriptGeometry::GetWireControlPoints(end, anchor, {});
			m_Wire.ColorFrom = targetColor;
			m_Wire.ColorTo = sourcePin.Color;
		}
	}

	void VisualScriptEditorModel::FinishWire(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		PinKey const& source = m_Wire.Source;
		bool const out = source.Direction == VisualScriptPinDirection::Out;
		auto const connect = [&](uint32_t nodeIndex, uint32_t pinIndex, std::optional<std::string> convertTo)
		{
			PinKey const target = MakePinKey(nodeIndex, pinIndex);
			VisualScriptConnectIntent connection;
			connection.Link.From =
				out ? VisualScriptPinReference{source.Node, source.Pin} : VisualScriptPinReference{target.Node, target.Pin};
			connection.Link.To =
				out ? VisualScriptPinReference{target.Node, target.Pin} : VisualScriptPinReference{source.Node, source.Pin};
			connection.InsertConvert = convertTo.has_value();
			connection.ConvertTo = convertTo.value_or(std::string());
			intents.emplace_back(std::move(connection));
		};
		if (m_Wire.Snapped)
		{
			if (std::optional<uint32_t> const node = FindPinNode(*m_Wire.Snapped))
			{
				connect(*node, *m_Wire.Snapped, std::nullopt);
			}
			return;
		}
		VisualScriptHit const& hit = m_Hover;
		std::optional<uint32_t> const nodeIndex = IsNodeHit(hit.Kind) ? m_Canvas.FindNode(hit.Element) : std::nullopt;
		switch (hit.Kind)
		{
			case Kind::Pin:
			{
				// A pin the wire did not snap to: connected when it can be, through a Convert when one bridges the two.
				std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
				if (!nodeIndex || hit.Element == source.Node || hit.Pin >= pins.size())
				{
					return;
				}
				VisualScriptPinVisual const& pin = pins[hit.Pin];
				VisualScriptPin const* info = FindPinInfo(hit.Element, m_Canvas.GetText(pin.Name), pin.Direction);
				if (pin.Direction == source.Direction || info == nullptr)
				{
					return;
				}
				uint8_t const pinClass = ClassifyPin(*nodeIndex, hit.Pin);
				if (pinClass != static_cast<uint8_t>(VisualScriptPinClass::Incompatible))
				{
					connect(*nodeIndex, hit.Pin, std::nullopt);
					return;
				}
				std::optional<std::string> const bridge =
					FindConvertBridge(*m_Library, out ? *m_Wire.SourceInfo : *info, out ? *info : *m_Wire.SourceInfo);
				if (bridge)
				{
					connect(*nodeIndex, hit.Pin, bridge);
				}
				return;
			}
			case Kind::Badge:
			case Kind::NodeHeader:
			case Kind::Node:
			case Kind::Reroute:
			{
				// Over a node's body (or a reroute's dot): its first pin the wire connects to.
				if (!nodeIndex || hit.Element == source.Node)
				{
					return;
				}
				if (std::optional<uint32_t> const pin = FindFirstPin(*nodeIndex, false))
				{
					connect(*nodeIndex, *pin, std::nullopt);
				}
				return;
			}
			case Kind::None:
			case Kind::Minimap:
				// Outside the canvas, or over the minimap: the wire is dropped.
				return;
			case Kind::Background:
			case Kind::Link:
			case Kind::CommentEdge:
			case Kind::CommentTitle:
			{
				// Empty canvas: the palette lists what connects to the pin, and links what it adds.
				VisualScriptPaletteIntent palette;
				palette.Position = m_View.ToCanvas(input.Mouse);
				palette.From = VisualScriptPinReference{source.Node, source.Pin};
				palette.Direction = source.Direction;
				intents.emplace_back(std::move(palette));
				return;
			}
		}
	}

	uint8_t VisualScriptEditorModel::ClassifyPin(uint32_t nodeIndex, uint32_t pinIndex)
	{
		if (pinIndex >= m_Wire.Classes.size())
		{
			return static_cast<uint8_t>(VisualScriptPinClass::Incompatible);
		}
		uint8_t& stored = m_Wire.Classes[pinIndex];
		if (stored != Unclassified)
		{
			return stored;
		}
		auto const classify = [this, nodeIndex, pinIndex]()
		{
			std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
			std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
			VisualScriptNodeVisual const& node = nodes[nodeIndex];
			VisualScriptPinVisual const& pin = pins[pinIndex];
			if (node.ID == m_Wire.Source.Node || pin.Direction == m_Wire.Source.Direction || pin.HasFlag(VisualScriptPinFlags::Orphan) ||
			    m_Library == nullptr || m_Wire.SourceInfo == nullptr)
			{
				return VisualScriptPinClass::Incompatible;
			}
			VisualScriptPin const* info = FindPinInfo(node.ID, m_Canvas.GetText(pin.Name), pin.Direction);
			if (info == nullptr)
			{
				return VisualScriptPinClass::Incompatible;
			}
			bool const out = m_Wire.Source.Direction == VisualScriptPinDirection::Out;
			VisualScriptPin const& output = out ? *m_Wire.SourceInfo : *info;
			VisualScriptPin const& input = out ? *info : *m_Wire.SourceInfo;
			if (!m_Library->CanConnect(output, input))
			{
				return VisualScriptPinClass::Incompatible;
			}
			// Converting: the analyzed types differ and neither is a wildcard, so the link converts implicitly (drawn as a
			// gradient).
			bool const same =
				output.Type == input.Type || output.Type == VisualScriptWildcardType || input.Type == VisualScriptWildcardType;
			return same ? VisualScriptPinClass::Compatible : VisualScriptPinClass::Converting;
		};
		stored = static_cast<uint8_t>(classify());
		if (m_Wire.Snapped != pinIndex && pinIndex < m_Wire.Shown.size())
		{
			m_Wire.Shown[pinIndex] = stored;
		}
		return stored;
	}

	void VisualScriptEditorModel::SetWireSnap(std::optional<uint32_t> pinIndex)
	{
		if (pinIndex == m_Wire.Snapped)
		{
			return;
		}
		if (m_Wire.Snapped && *m_Wire.Snapped < m_Wire.Shown.size())
		{
			uint8_t const stored = m_Wire.Classes[*m_Wire.Snapped];
			m_Wire.Shown[*m_Wire.Snapped] = stored == Unclassified ? static_cast<uint8_t>(VisualScriptPinClass::None) : stored;
		}
		m_Wire.Snapped = pinIndex;
		if (pinIndex && *pinIndex < m_Wire.Shown.size())
		{
			m_Wire.Shown[*pinIndex] = static_cast<uint8_t>(VisualScriptPinClass::Snapped);
		}
	}

	void VisualScriptEditorModel::ClassifyVisiblePins()
	{
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		m_Index.Query(m_View.GetVisibleRect(), VisualScriptSpatialIndex::NodeMask, m_Query);
		auto const classify = [this, &nodes](uint32_t node)
		{
			for (uint32_t pin = nodes[node].FirstPin; pin < nodes[node].FirstPin + nodes[node].PinCount; pin++)
			{
				ClassifyPin(node, pin);
			}
		};
		for (uint32_t const item : m_Query)
		{
			classify(VisualScriptSpatialIndex::GetIndex(item));
		}
		if (std::optional<uint32_t> const expanded = GetNodePreview().Expanded)
		{
			classify(*expanded);
		}
	}

	bool VisualScriptEditorModel::Accepts(bool relink, uint32_t nodeIndex, uint32_t pinIndex)
	{
		if (!relink)
		{
			uint8_t const pinClass = ClassifyPin(nodeIndex, pinIndex);
			return pinClass == static_cast<uint8_t>(VisualScriptPinClass::Compatible) ||
			       pinClass == static_cast<uint8_t>(VisualScriptPinClass::Converting);
		}
		for (size_t link = 0; link < m_Relink.Links.size(); link++)
		{
			if (CanRelinkTo(link, nodeIndex, pinIndex))
			{
				return true;
			}
		}
		return false;
	}

	int VisualScriptEditorModel::RankPin(bool relink, uint32_t nodeIndex, uint32_t pinIndex)
	{
		if (relink)
		{
			return 0;
		}
		// Exact types before conversions, then (for flow) exec pins before the rest.
		bool const converting = ClassifyPin(nodeIndex, pinIndex) == static_cast<uint8_t>(VisualScriptPinClass::Converting);
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		bool const execTarget = pins[pinIndex].Type == VisualScriptExecType;
		bool const execSource = IsExecPin(m_Wire.SourceInfo);
		return (converting ? 2 : 0) + (execSource && !execTarget ? 1 : 0);
	}

	std::optional<uint32_t> VisualScriptEditorModel::FindSnapPin(glm::vec2 mouse, bool relink)
	{
		float const pixelsPerUnit = m_View.GetPixelsPerUnit();
		if (!(pixelsPerUnit > 0.0f) || m_View.GetDetailLevel() == VisualScriptDetailLevel::Far)
		{
			// Pins are neither drawn nor hit at Far.
			return std::nullopt;
		}
		if (!IsInCanvas(mouse) || m_Hover.Kind == Kind::Minimap)
		{
			// Pins are snapped to only where the canvas draws them.
			return std::nullopt;
		}
		float const radius = Metrics::WireSnapLogicalPixels * m_View.GetUIScale();
		float const slop = Metrics::PinSlopLogicalPixels * m_View.GetUIScale() / pixelsPerUnit;
		// A pin's row counts only where its label is drawn, as hit testing has it.
		bool const labelRows = VisualScriptGeometry::ArePinLabelsDrawn(m_View.GetDetailLevel(), m_View.GetState().Zoom);
		glm::vec2 const point = m_View.ToCanvas(mouse);
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		VisualScriptNodePreview const preview = GetNodePreview();
		m_Index.QueryPoint(point, radius / pixelsPerUnit, VisualScriptSpatialIndex::NodeMask, m_Query);
		if (preview.Expanded)
		{
			m_Query.push_back(VisualScriptSpatialIndex::MakeItem(VisualScriptSpatialIndex::ItemKind::Node, *preview.Expanded));
		}
		std::optional<uint32_t> nearest;
		std::tuple<float, int, uint32_t> nearestKey{std::numeric_limits<float>::infinity(), 0, 0};
		std::optional<uint32_t> row;
		std::tuple<int, uint32_t> rowKey{0, 0};
		for (uint32_t const item : m_Query)
		{
			uint32_t const nodeIndex = VisualScriptSpatialIndex::GetIndex(item);
			VisualScriptNodeVisual const node =
				preview.Expanded == nodeIndex ? VisualScriptGeometry::GetExpandedNode(nodes[nodeIndex]) : nodes[nodeIndex];
			if (node.Shape == VisualScriptNodeShape::Reroute)
			{
				continue;
			}
			for (uint32_t pinIndex = node.FirstPin; pinIndex < node.FirstPin + node.PinCount; pinIndex++)
			{
				if (!Accepts(relink, nodeIndex, pinIndex))
				{
					continue;
				}
				int const rank = RankPin(relink, nodeIndex, pinIndex);
				glm::vec2 const anchor = m_View.ToScreen(VisualScriptGeometry::GetPinAnchor(node, pins[pinIndex]));
				float const distance = glm::length(anchor - mouse);
				if (distance <= radius && IsInCanvas(anchor))
				{
					std::tuple<float, int, uint32_t> const key{distance, rank, pinIndex};
					if (!nearest || key < nearestKey)
					{
						nearest = pinIndex;
						nearestKey = key;
					}
				}
				else if (VisualScriptGeometry::Contains(VisualScriptGeometry::GetPinHitRect(node, pins[pinIndex], slop, {}, labelRows),
				                                        point))
				{
					std::tuple<int, uint32_t> const key{rank, pinIndex};
					if (!row || key < rowKey)
					{
						row = pinIndex;
						rowKey = key;
					}
				}
			}
		}
		return nearest ? nearest : row;
	}

	std::optional<uint32_t> VisualScriptEditorModel::FindFirstPin(uint32_t nodeIndex, bool relink)
	{
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		VisualScriptNodeVisual const& node = nodes[nodeIndex];
		std::optional<uint32_t> best;
		int bestRank = 0;
		for (uint32_t pinIndex = node.FirstPin; pinIndex < node.FirstPin + node.PinCount; pinIndex++)
		{
			if (!Accepts(relink, nodeIndex, pinIndex))
			{
				continue;
			}
			int const rank = RankPin(relink, nodeIndex, pinIndex);
			if (!best || rank < bestRank)
			{
				best = pinIndex;
				bestRank = rank;
			}
		}
		return best;
	}

	void VisualScriptEditorModel::StartRelink()
	{
		PinKey key;
		key.Node = m_Press.PinNode;
		key.Pin = m_Press.Pin;
		key.Direction = m_Press.Direction;
		ForceAnalysis();
		std::optional<std::pair<uint32_t, uint32_t>> const pin = FindCanvasPin(key);
		if (!pin || m_Library == nullptr)
		{
			return;
		}
		m_Relink.Pin = key;
		m_Relink.Links.clear();
		m_Relink.FarEnds.clear();
		m_Relink.Wires.clear();
		m_Relink.Snapped.reset();
		std::span<VisualScriptLinkVisual const> const links = m_Canvas.GetLinks();
		for (uint32_t const link : m_Canvas.GetIncidentLinks(pin->first))
		{
			VisualScriptLinkVisual const& visual = links[link];
			bool const atPin = key.Direction == VisualScriptPinDirection::Out
			                       ? visual.FromNode == pin->first && visual.FromPin == pin->second
			                       : visual.ToNode == pin->first && visual.ToPin == pin->second;
			if (!atPin)
			{
				continue;
			}
			m_Relink.Links.push_back(m_Canvas.GetDocumentLink(link));
			m_Relink.FarEnds.push_back(key.Direction == VisualScriptPinDirection::Out ? MakePinKey(visual.ToNode, visual.ToPin)
			                                                                          : MakePinKey(visual.FromNode, visual.FromPin));
		}
		if (m_Relink.Links.empty())
		{
			return;
		}
		m_Gesture = VisualScriptGesture::Relinking;
		m_GestureButton = 0;
		m_Press.Active = false;
	}

	bool VisualScriptEditorModel::CanRelinkTo(size_t link, uint32_t nodeIndex, uint32_t pinIndex) const
	{
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		VisualScriptNodeVisual const& node = nodes[nodeIndex];
		VisualScriptPinVisual const& pin = pins[pinIndex];
		PinKey const& farEnd = m_Relink.FarEnds[link];
		if (m_Library == nullptr || pin.Direction != m_Relink.Pin.Direction || node.ID == farEnd.Node ||
		    pin.HasFlag(VisualScriptPinFlags::Orphan))
		{
			return false;
		}
		VisualScriptPin const* target = FindPinInfo(node.ID, m_Canvas.GetText(pin.Name), pin.Direction);
		VisualScriptPin const* farInfo = FindPinInfo(farEnd);
		if (target == nullptr || farInfo == nullptr)
		{
			return false;
		}
		return pin.Direction == VisualScriptPinDirection::Out ? m_Library->CanConnect(*target, *farInfo)
		                                                      : m_Library->CanConnect(*farInfo, *target);
	}

	void VisualScriptEditorModel::ContinueRelink(VisualScriptCanvasInput const& input, double deltaTime,
	                                             std::vector<VisualScriptEditorIntent>& intents)
	{
		if (input.Pressed[1] || (!input.Down[0] && !input.Released[0]))
		{
			CancelGesture();
			return;
		}
		AutoPan(input, deltaTime);
		std::optional<std::pair<uint32_t, uint32_t>> const pin = FindCanvasPin(m_Relink.Pin);
		if (!pin)
		{
			CancelGesture();
			return;
		}
		m_Relink.Snapped = FindSnapPin(input.Mouse, true);
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		glm::vec2 end = m_View.ToCanvas(input.Mouse);
		if (m_Relink.Snapped)
		{
			if (std::optional<uint32_t> const node = FindPinNode(*m_Relink.Snapped))
			{
				end = VisualScriptGeometry::GetPinAnchor(nodes[*node], pins[*m_Relink.Snapped]);
			}
		}
		// Each picked link from its far end to the mouse: the near end follows it.
		m_Relink.Wires.clear();
		for (PinKey const& farEnd : m_Relink.FarEnds)
		{
			std::optional<std::pair<uint32_t, uint32_t>> const farPin = FindCanvasPin(farEnd);
			if (!farPin)
			{
				continue;
			}
			glm::vec2 const anchor = VisualScriptGeometry::GetPinAnchor(nodes[farPin->first], pins[farPin->second]);
			m_Relink.Wires.push_back(m_Relink.Pin.Direction == VisualScriptPinDirection::Out
			                             ? VisualScriptGeometry::GetWireControlPoints(end, anchor, {})
			                             : VisualScriptGeometry::GetWireControlPoints(anchor, end, {}));
		}
		if (input.Released[0])
		{
			FinishRelink(intents);
			EndGesture();
		}
	}

	void VisualScriptEditorModel::FinishRelink(std::vector<VisualScriptEditorIntent>& intents)
	{
		std::optional<std::pair<uint32_t, uint32_t>> const pickedPin = FindCanvasPin(m_Relink.Pin);
		std::optional<std::pair<uint32_t, uint32_t>> target;
		if (m_Relink.Snapped)
		{
			if (std::optional<uint32_t> const node = FindPinNode(*m_Relink.Snapped))
			{
				target = std::make_pair(*node, *m_Relink.Snapped);
			}
		}
		else if (IsNodeHit(m_Hover.Kind))
		{
			std::optional<uint32_t> const nodeIndex = m_Canvas.FindNode(m_Hover.Element);
			if (nodeIndex && m_Hover.Kind == Kind::Pin && Accepts(true, *nodeIndex, m_Hover.Pin))
			{
				target = std::make_pair(*nodeIndex, m_Hover.Pin);
			}
			else if (nodeIndex && m_Hover.Kind != Kind::Pin)
			{
				if (std::optional<uint32_t> const pin = FindFirstPin(*nodeIndex, true))
				{
					target = std::make_pair(*nodeIndex, *pin);
				}
			}
		}
		else if (m_Hover.Kind == Kind::Background || m_Hover.Kind == Kind::Link || m_Hover.Kind == Kind::CommentEdge ||
		         m_Hover.Kind == Kind::CommentTitle)
		{
			// Dropped on the empty canvas: the links are removed. Outside the canvas or over the minimap nothing changes.
			VisualScriptRelinkIntent relink;
			relink.Remove = m_Relink.Links;
			intents.emplace_back(std::move(relink));
			return;
		}
		if (!target || target == pickedPin)
		{
			return;
		}
		// Every picked link whose far end connects with the pin moves to it; a pin that takes one link takes the first; the
		// others are dropped.
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		VisualScriptPinVisual const& pin = pins[target->second];
		PinKey const key = MakePinKey(target->first, target->second);
		VisualScriptPin const* targetInfo = FindPinInfo(key);
		VisualScriptRelinkIntent relink;
		relink.Remove = m_Relink.Links;
		for (size_t link = 0; link < m_Relink.Links.size(); link++)
		{
			if (!CanRelinkTo(link, target->first, target->second))
			{
				continue;
			}
			PinKey const& farEnd = m_Relink.FarEnds[link];
			bool const exec = IsExecPin(targetInfo) || IsExecPin(FindPinInfo(farEnd));
			bool const single = pin.Direction == VisualScriptPinDirection::In ? !exec : exec;
			VisualScriptLink moved;
			moved.From = pin.Direction == VisualScriptPinDirection::Out ? VisualScriptPinReference{key.Node, key.Pin}
			                                                            : VisualScriptPinReference{farEnd.Node, farEnd.Pin};
			moved.To = pin.Direction == VisualScriptPinDirection::Out ? VisualScriptPinReference{farEnd.Node, farEnd.Pin}
			                                                          : VisualScriptPinReference{key.Node, key.Pin};
			relink.Add.push_back(std::move(moved));
			if (single)
			{
				break;
			}
		}
		if (!relink.Add.empty())
		{
			intents.emplace_back(std::move(relink));
		}
	}

	void VisualScriptEditorModel::ContinueCut(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents)
	{
		if (!input.Down[1] && !input.Released[1])
		{
			CancelGesture();
			return;
		}
		m_GestureCanvas = m_View.ToCanvas(input.Mouse);
		// The wires the line crosses where they are drawn.
		glm::vec2 const from = m_View.ToScreen(m_Press.Canvas);
		glm::vec2 const to = m_View.ToScreen(m_GestureCanvas);
		VisualScriptGeometry::WireFlattening const flattening =
			VisualScriptGeometry::GetWireFlattening(m_View.GetDetailLevel(), m_CurveTolerance);
		m_Index.Query(MakeRect(m_Press.Canvas, m_GestureCanvas), VisualScriptSpatialIndex::LinkMask, m_Query);
		std::sort(m_Query.begin(), m_Query.end());
		m_CutLinks.clear();
		for (uint32_t const item : m_Query)
		{
			uint32_t const link = VisualScriptSpatialIndex::GetIndex(item);
			std::array<glm::vec2, 4> const points = VisualScriptGeometry::GetLinkControlPoints(m_Canvas, link);
			std::array<glm::vec2, 4> const screen = {m_View.ToScreen(points[0]), m_View.ToScreen(points[1]), m_View.ToScreen(points[2]),
			                                         m_View.ToScreen(points[3])};
			VisualScriptGeometry::FlattenWire(screen, flattening.Tolerance, flattening.SegmentCap, m_Polyline);
			if (VisualScriptGeometry::SegmentCrossesPolyline(from, to, m_Polyline))
			{
				m_CutLinks.push_back(m_Canvas.GetDocumentLink(link));
			}
		}
		if (!input.Released[1])
		{
			return;
		}
		if (!m_CutLinks.empty())
		{
			VisualScriptDisconnectIntent disconnect;
			disconnect.Links = m_CutLinks;
			intents.emplace_back(std::move(disconnect));
		}
		EndGesture();
	}

	// --- Selection, raising, sub-graphs, navigation ---

	bool VisualScriptEditorModel::IsSelected(uint32_t element) const
	{
		return std::find(m_Selection.begin(), m_Selection.end(), element) != m_Selection.end();
	}

	void VisualScriptEditorModel::SetSelection(std::vector<uint32_t> selection)
	{
		// Each element once, in the order given.
		std::vector<uint32_t> seen = selection;
		std::sort(seen.begin(), seen.end());
		if (std::adjacent_find(seen.begin(), seen.end()) != seen.end())
		{
			std::vector<uint32_t> unique;
			unique.reserve(selection.size());
			std::vector<uint32_t> taken;
			for (uint32_t const element : selection)
			{
				auto const position = std::lower_bound(taken.begin(), taken.end(), element);
				if (position == taken.end() || *position != element)
				{
					taken.insert(position, element);
					unique.push_back(element);
				}
			}
			selection = std::move(unique);
		}
		m_Selection = std::move(selection);
		RebuildSelectionMasks();
	}

	void VisualScriptEditorModel::MakePrimary(uint32_t element)
	{
		auto const found = std::find(m_Selection.begin(), m_Selection.end(), element);
		if (found != m_Selection.end())
		{
			std::rotate(m_Selection.begin(), found, found + 1);
		}
	}

	void VisualScriptEditorModel::Raise(uint32_t node)
	{
		m_RaiseCounter++;
		auto const found = std::lower_bound(m_Raised.begin(), m_Raised.end(), node,
		                                    [](IdIndex const& entry, uint32_t value)
		                                    {
												return entry.first < value;
											});
		if (found != m_Raised.end() && found->first == node)
		{
			found->second = m_RaiseCounter;
		}
		else
		{
			m_Raised.insert(found, IdIndex{node, m_RaiseCounter});
		}
		if (std::optional<uint32_t> const index = m_Canvas.FindNode(node); index && *index < m_NodeRaise.size())
		{
			m_NodeRaise[*index] = m_RaiseCounter;
		}
	}

	void VisualScriptEditorModel::RebuildSelectionMasks()
	{
		m_NodeSelected.assign(m_Canvas.GetNodes().size(), 0);
		m_CommentSelected.assign(m_Canvas.GetComments().size(), 0);
		for (uint32_t const element : m_Selection)
		{
			if (std::optional<uint32_t> const node = m_Canvas.FindNode(element))
			{
				m_NodeSelected[*node] = 1;
			}
			else if (std::optional<uint32_t> const comment = m_Canvas.FindComment(element))
			{
				m_CommentSelected[*comment] = 1;
			}
		}
	}

	void VisualScriptEditorModel::RebuildRaise()
	{
		m_NodeRaise.assign(m_Canvas.GetNodes().size(), 0);
		for (IdIndex const& raised : m_Raised)
		{
			if (std::optional<uint32_t> const node = m_Canvas.FindNode(raised.first))
			{
				m_NodeRaise[*node] = raised.second;
			}
		}
	}

	void VisualScriptEditorModel::RebuildDynamic()
	{
		m_DynamicNodes.clear();
		m_DynamicComments.clear();
		m_DynamicLinks.clear();
		m_NodeMoved.clear();
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptLinkVisual const> const links = m_Canvas.GetLinks();
		if (m_Gesture == VisualScriptGesture::MovingElements)
		{
			// The moved nodes and comments, tested at their preview places, so the drag never rebuilds the index.
			m_NodeMoved.assign(nodes.size(), 0);
			for (uint32_t const element : m_Move.Elements)
			{
				if (std::optional<uint32_t> const node = m_Canvas.FindNode(element))
				{
					m_NodeMoved[*node] = 1;
					m_DynamicNodes.push_back(*node);
				}
				else if (std::optional<uint32_t> const comment = m_Canvas.FindComment(element))
				{
					m_DynamicComments.push_back(*comment);
				}
			}
		}
		else if (m_Gesture == VisualScriptGesture::ResizingComment)
		{
			if (std::optional<uint32_t> const comment = m_Canvas.FindComment(m_Resize.Comment))
			{
				m_DynamicComments.push_back(*comment);
			}
		}
		else if (m_Gesture == VisualScriptGesture::Wiring && m_Wire.Expanded)
		{
			// Drawn expanded, its pins anchor on its rows.
			if (std::optional<uint32_t> const node = m_Canvas.FindNode(*m_Wire.Expanded))
			{
				m_DynamicNodes.push_back(*node);
			}
		}
		for (uint32_t const node : m_DynamicNodes)
		{
			for (uint32_t const link : m_Canvas.GetIncidentLinks(node))
			{
				m_DynamicLinks.push_back(link);
				// A reroute faces its first target: when that target is drawn elsewhere the reroute may turn, which changes
				// the shape of every wire of the reroute. (A reroute's own anchor is its dot whichever way it faces, so this
				// goes one reroute deep.)
				VisualScriptLinkVisual const& visual = links[link];
				uint32_t const reroute = visual.FromNode;
				if (visual.ToNode == node && reroute != node && nodes[reroute].Shape == VisualScriptNodeShape::Reroute &&
				    m_Canvas.GetFirstOutgoingLink(reroute) == link)
				{
					for (uint32_t const rerouteLink : m_Canvas.GetIncidentLinks(reroute))
					{
						m_DynamicLinks.push_back(rerouteLink);
					}
				}
			}
		}
		std::sort(m_DynamicLinks.begin(), m_DynamicLinks.end());
		m_DynamicLinks.erase(std::unique(m_DynamicLinks.begin(), m_DynamicLinks.end()), m_DynamicLinks.end());
		m_Index.SetDynamic(m_DynamicNodes, m_DynamicComments, m_DynamicLinks);
	}

	void VisualScriptEditorModel::QueryDrawn(VisualScriptRect const& rect, uint8_t kindMask, std::vector<uint32_t>& out,
	                                         VisualScriptIndexStats* stats) const
	{
		using ItemKind = VisualScriptSpatialIndex::ItemKind;
		m_Index.Query(rect, kindMask, out, stats);
		VisualScriptNodePreview const preview = GetNodePreview();
		if ((kindMask & VisualScriptSpatialIndex::NodeMask) != 0)
		{
			std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
			for (uint32_t const node : m_DynamicNodes)
			{
				VisualScriptNodeVisual const& visual = nodes[node];
				glm::vec2 const offset = preview.GetOffset(node);
				VisualScriptRect bounds = VisualScriptGeometry::Translate(visual.VisualBounds, offset);
				if (preview.Expanded == node)
				{
					VisualScriptNodeVisual const expanded = VisualScriptGeometry::GetExpandedNode(visual);
					bounds = VisualScriptGeometry::Unite(bounds, VisualScriptGeometry::GetNodeRect(expanded, offset));
				}
				if (VisualScriptGeometry::Intersects(bounds, rect))
				{
					out.push_back(VisualScriptSpatialIndex::MakeItem(ItemKind::Node, node));
				}
			}
		}
		if ((kindMask & VisualScriptSpatialIndex::CommentMask) != 0)
		{
			std::span<VisualScriptCommentVisual const> const comments = m_Canvas.GetComments();
			for (uint32_t const comment : m_DynamicComments)
			{
				VisualScriptCommentVisual const& visual = comments[comment];
				bool const resized = m_Gesture == VisualScriptGesture::ResizingComment && visual.ID == m_Resize.Comment;
				VisualScriptRect const bounds = resized ? m_Resize.Rect : VisualScriptGeometry::Translate(visual.Rect, m_Move.Offset);
				if (VisualScriptGeometry::Intersects(bounds, rect))
				{
					out.push_back(VisualScriptSpatialIndex::MakeItem(ItemKind::Comment, comment));
				}
			}
		}
		if ((kindMask & VisualScriptSpatialIndex::LinkMask) != 0)
		{
			for (uint32_t const link : m_DynamicLinks)
			{
				if (VisualScriptGeometry::Intersects(VisualScriptGeometry::GetLinkBounds(m_Canvas, link, preview), rect))
				{
					out.push_back(VisualScriptSpatialIndex::MakeItem(ItemKind::Link, link));
				}
			}
		}
	}

	void VisualScriptEditorModel::PruneToCanvas()
	{
		std::erase_if(m_Selection,
		              [this](uint32_t element)
		              {
						  return !m_Canvas.FindNode(element) && !m_Canvas.FindComment(element);
					  });
		std::erase_if(m_Raised,
		              [this](IdIndex const& raised)
		              {
						  return !m_Canvas.FindNode(raised.first);
					  });
	}

	std::vector<uint32_t> VisualScriptEditorModel::GetAllElements() const
	{
		std::vector<uint32_t> elements;
		VisualScriptGraph const* graph = m_Display != nullptr ? FindGraph(GetDrawnDocument(), m_Function) : nullptr;
		if (graph == nullptr)
		{
			return elements;
		}
		elements.reserve(graph->Nodes.size() + graph->Comments.size());
		for (VisualScriptNode const& node : graph->Nodes)
		{
			if (m_Canvas.FindNode(node.ID))
			{
				elements.push_back(node.ID);
			}
		}
		for (VisualScriptComment const& comment : graph->Comments)
		{
			if (m_Canvas.FindComment(comment.ID))
			{
				elements.push_back(comment.ID);
			}
		}
		return elements;
	}

	void VisualScriptEditorModel::Frame(std::span<uint32_t const> elements, double time)
	{
		std::optional<VisualScriptRect> const bounds = elements.empty() ? m_ContentBounds : m_Canvas.GetBounds(elements);
		m_View.Frame(bounds, time, GetMoveDuration());
	}

	void VisualScriptEditorModel::SwitchFunction(uint32_t function)
	{
		CancelGesture();
		if (HasFunction(m_Function))
		{
			// The sub-graph left keeps its view and selection.
			auto const kept = std::find_if(m_SubGraphs.begin(), m_SubGraphs.end(),
			                               [this](SubGraphState const& state)
			                               {
											   return state.Function == m_Function;
										   });
			SubGraphState& state = kept != m_SubGraphs.end() ? *kept : m_SubGraphs.emplace_back();
			state.Function = m_Function;
			state.View = m_View.GetTargetState();
			state.Selection = m_Selection;
			m_Settings.SetView(m_Graph, state.View);
		}
		std::erase_if(m_SubGraphs,
		              [this](SubGraphState const& state)
		              {
						  return !HasFunction(state.Function);
					  });
		m_Function = function;
		m_Settings.SetShown(m_Graph, function);
		m_Hover = {};
		m_NeedsFrame = false;
		auto const kept = std::find_if(m_SubGraphs.begin(), m_SubGraphs.end(),
		                               [function](SubGraphState const& state)
		                               {
										   return state.Function == function;
									   });
		VisualScriptGraphViews const* views = m_Settings.FindGraph(m_Graph);
		VisualScriptViewState const* stored = views != nullptr ? views->FindView(function) : nullptr;
		if (kept != m_SubGraphs.end())
		{
			m_View.SetState(kept->View);
			m_Selection = kept->Selection;
		}
		else if (stored != nullptr)
		{
			m_View.SetState(*stored);
			m_Selection.clear();
		}
		else
		{
			VisualScriptViewState initial;
			initial.Function = function;
			m_View.SetState(initial);
			m_Selection.clear();
			m_NeedsFrame = true;
		}
		m_StoredView = m_View.GetState();
		// The masks follow once the canvas shows the sub-graph (the next Sync).
		RebuildSelectionMasks();
	}

	void VisualScriptEditorModel::PushHistory(VisualScriptViewState const& view)
	{
		m_Back.push_back(view);
		if (m_Back.size() > MaxHistory)
		{
			m_Back.erase(m_Back.begin());
		}
		m_Forward.clear();
	}

	void VisualScriptEditorModel::GoBack(double time)
	{
		while (!m_Back.empty())
		{
			VisualScriptViewState const target = m_Back.back();
			m_Back.pop_back();
			if (!HasFunction(target.Function))
			{
				// A function deleted since: its views are skipped.
				continue;
			}
			m_Forward.push_back(m_View.GetTargetState());
			GoTo(target, time);
			return;
		}
	}

	void VisualScriptEditorModel::GoForward(double time)
	{
		while (!m_Forward.empty())
		{
			VisualScriptViewState const target = m_Forward.back();
			m_Forward.pop_back();
			if (!HasFunction(target.Function))
			{
				continue;
			}
			m_Back.push_back(m_View.GetTargetState());
			GoTo(target, time);
			return;
		}
	}

	void VisualScriptEditorModel::GoTo(VisualScriptViewState const& view, double time)
	{
		if (view.Function != m_Function)
		{
			// Another sub-graph shows at once, at the view kept.
			SwitchFunction(view.Function);
			m_View.SetState(view);
			m_StoredView.reset();
			m_NeedsFrame = false;
			return;
		}
		m_View.AnimateTo(view, time, GetMoveDuration());
	}

	void VisualScriptEditorModel::StoreBookmark(uint8_t slot)
	{
		// Named after the innermost comment under the view's centre.
		VisualScriptViewState const view = m_View.GetTargetState();
		std::string name;
		std::span<VisualScriptCommentVisual const> const comments = m_Canvas.GetComments();
		for (size_t index = comments.size(); index-- > 0;)
		{
			if (VisualScriptGeometry::Contains(comments[index].Rect, view.Center))
			{
				name = std::string(m_Canvas.GetText(comments[index].Title));
				break;
			}
		}
		VisualScriptBookmark bookmark;
		bookmark.Slot = slot;
		bookmark.Name = name.empty() ? "Bookmark " + std::to_string(slot) : std::move(name);
		bookmark.View = view;
		m_Settings.SetBookmark(m_Graph, std::move(bookmark));
	}

	void VisualScriptEditorModel::GoToBookmark(uint8_t slot, double time)
	{
		VisualScriptGraphViews const* views = m_Settings.FindGraph(m_Graph);
		VisualScriptBookmark const* bookmark = views != nullptr ? views->FindBookmark(slot) : nullptr;
		if (bookmark == nullptr)
		{
			return;
		}
		// Copied: switching sub-graphs updates the settings the bookmark lives in.
		VisualScriptViewState const target = bookmark->View;
		if (!HasFunction(target.Function) || target == m_View.GetTargetState())
		{
			return;
		}
		PushHistory(m_View.GetTargetState());
		GoTo(target, time);
	}

	bool VisualScriptEditorModel::GoToCustomEvent(VisualScriptNode const& sender)
	{
		std::optional<std::string> const name = ReadStringSetting(sender, "Name");
		if (!name || m_Display == nullptr)
		{
			return false;
		}
		// Custom events live in the event graph.
		for (VisualScriptNode const& node : GetDrawnDocument().Graph.Nodes)
		{
			if (node.Type == "Event.Custom" && ReadStringSetting(node, "Name") == name)
			{
				VisualScriptOpenRequest request;
				request.Graph = m_Graph;
				request.Function = 0;
				request.Select = {node.ID};
				request.Frame = VisualScriptFrame::Selection;
				ApplyOpenRequest(request);
				if (!m_PendingOpen->Pushed)
				{
					PushHistory(m_View.GetTargetState());
					m_PendingOpen->Pushed = true;
				}
				return true;
			}
		}
		return false;
	}

	std::optional<std::pair<uint32_t, uint32_t>> VisualScriptEditorModel::FindCanvasPin(PinKey const& key) const
	{
		std::optional<uint32_t> const node = m_Canvas.FindNode(key.Node);
		if (!node)
		{
			return std::nullopt;
		}
		std::optional<uint32_t> const pin = m_Canvas.FindPin(*node, key.Pin, key.Direction);
		if (!pin)
		{
			return std::nullopt;
		}
		return std::make_pair(*node, *pin);
	}

	std::optional<uint32_t> VisualScriptEditorModel::FindPinNode(uint32_t pinIndex) const
	{
		// The visual model appends each node's pins after the previous node's, so FirstPin ascends in node order: the node is
		// the last one starting at or before the pin.
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		auto const after = std::upper_bound(nodes.begin(), nodes.end(), pinIndex,
		                                    [](uint32_t pin, VisualScriptNodeVisual const& node)
		                                    {
												return pin < node.FirstPin;
											});
		if (after == nodes.begin())
		{
			return std::nullopt;
		}
		VisualScriptNodeVisual const& node = *(after - 1);
		if (pinIndex >= node.FirstPin + node.PinCount)
		{
			return std::nullopt;
		}
		return static_cast<uint32_t>(after - 1 - nodes.begin());
	}

	std::optional<uint32_t> VisualScriptEditorModel::FindCanvasLink(VisualScriptLink const& link) const
	{
		std::optional<uint32_t> const node = m_Canvas.FindNode(link.From.Node);
		if (!node)
		{
			return std::nullopt;
		}
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptLinkVisual const> const links = m_Canvas.GetLinks();
		for (uint32_t const candidate : m_Canvas.GetIncidentLinks(*node))
		{
			VisualScriptLinkVisual const& visual = links[candidate];
			if (nodes[visual.FromNode].ID == link.From.Node && nodes[visual.ToNode].ID == link.To.Node &&
			    m_Canvas.GetText(visual.FromName) == link.From.Pin && m_Canvas.GetText(visual.ToName) == link.To.Pin)
			{
				return candidate;
			}
		}
		return std::nullopt;
	}

	VisualScriptEditorModel::PinKey VisualScriptEditorModel::MakePinKey(uint32_t nodeIndex, uint32_t pinIndex) const
	{
		std::span<VisualScriptNodeVisual const> const nodes = m_Canvas.GetNodes();
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		VisualScriptPinVisual const& pin = pins[pinIndex];
		PinKey key;
		key.Node = nodes[nodeIndex].ID;
		key.Pin = std::string(m_Canvas.GetText(pin.Name));
		key.Direction = pin.Direction;
		return key;
	}

	std::vector<VisualScriptLink> VisualScriptEditorModel::GetPinLinks(uint32_t nodeIndex, uint32_t pinIndex, bool wholeSide) const
	{
		std::vector<VisualScriptLink> result;
		std::span<VisualScriptLinkVisual const> const links = m_Canvas.GetLinks();
		std::span<VisualScriptPinVisual const> const pins = m_Canvas.GetPins();
		VisualScriptPinDirection const direction = pins[pinIndex].Direction;
		for (uint32_t const link : m_Canvas.GetIncidentLinks(nodeIndex))
		{
			VisualScriptLinkVisual const& visual = links[link];
			bool const out = direction == VisualScriptPinDirection::Out;
			uint32_t const end = out ? visual.FromPin : visual.ToPin;
			bool const atNode = out ? visual.FromNode == nodeIndex : visual.ToNode == nodeIndex;
			if (atNode && (end == pinIndex || (wholeSide && pins[end].Direction == direction)))
			{
				result.push_back(m_Canvas.GetDocumentLink(link));
			}
		}
		return result;
	}
}
