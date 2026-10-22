#include "Editor/VisualScripting/VisualScriptEditorModel.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptLayout.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace Strada
{
	namespace
	{
		using PackedIndex = std::pair<uint32_t, uint64_t>;

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

		// A file the editor cannot write: the standard library reports Windows' read-only attribute as no write permission.
		// What cannot be told is taken as writable; a write that then fails shows as one.
		bool IsWritable(std::filesystem::path const& path)
		{
			std::error_code error;
			std::filesystem::file_status const status = std::filesystem::status(path, error);
			if (error)
			{
				return true;
			}
			using Perms = std::filesystem::perms;
			return (status.permissions() & (Perms::owner_write | Perms::group_write | Perms::others_write)) != Perms::none;
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

		template<typename T>
		size_t GetCapacity(std::vector<T> const& vector)
		{
			return vector.capacity() * sizeof(T);
		}
	}

	VisualScriptEditorModel::VisualScriptEditorModel(AssetHandle graph, VisualScriptViewSettings& settings)
		: m_Graph(graph)
	{
		if (VisualScriptGraphViews const* views = settings.FindGraph(graph))
		{
			m_Function = views->Shown;
		}
	}

	VisualScriptSyncResult VisualScriptEditorModel::Sync(VisualScriptSemantics& semantics, VisualScriptTextMeasurer const& measure,
	                                                     VisualScriptThemeAnchors const& theme, bool widgetActive, double time)
	{
		VisualScriptSyncResult result;
		// What changed outside is reported until the next frame's first Sync.
		if (time != m_LastSyncTime)
		{
			m_ChangedElements.clear();
			m_LastSyncTime = time;
		}
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
			return result;
		}
		if (m_Function != 0 && FindGraph(GetDrawnDocument(), m_Function) == nullptr)
		{
			// The function shown was deleted (an undo, automation): the event graph shows.
			m_Function = 0;
		}
		if (result.DocumentChanged)
		{
			IndexNodes(GetDrawnDocument(), m_DrawnNodes);
			m_AnalysisPending = true;
		}
		RefreshAnalysis(semantics, widgetActive, time, result);

		uint64_t const assetVersion = AssetManager::IsInitialized() ? AssetManager::GetVersion() : 0;
		bool const stale = m_CanvasStale || result.DocumentChanged || result.AnalysisChanged || !(theme == m_Theme) ||
		                   measure.Signature != m_MeasureSignature || assetVersion != m_AssetVersion || m_Function != m_CanvasFunction;
		if (stale)
		{
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
			if (m_Canvas.Sync(source, measure, theme))
			{
				m_Index.Build(m_Canvas);
				result.GeometryChanged = true;
			}
			m_Theme = theme;
			m_MeasureSignature = measure.Signature;
			m_AssetVersion = assetVersion;
			m_CanvasFunction = m_Function;
			m_CanvasStale = false;
		}
		return result;
	}

	void VisualScriptEditorModel::OnCommitted(Ref<VisualScriptDocument const> committed)
	{
		m_OwnDocument = std::move(committed);
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
		return m_Canvas.GetCapacityBytes() + m_Index.GetCapacityBytes() + GetCapacity(m_ChangedElements) + GetCapacity(m_AnalysisNodes) +
		       GetCapacity(m_AnalyzedNodes) + GetCapacity(m_DrawnNodes);
	}

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
			m_Writable = IsWritable(AssetManager::GetAbsolutePath(m_Graph));
			m_CheckWritable = false;
		}
		m_State = m_Writable ? VisualScriptDocumentState::Editable : VisualScriptDocumentState::ReadOnlyFile;
	}

	void VisualScriptEditorModel::AdoptDocument(Ref<VisualScriptDocument const> document, VisualScriptSyncResult& result)
	{
		if (document == m_Document)
		{
			return;
		}
		bool const own = m_OwnDocument != nullptr && document == m_OwnDocument;
		// The commit the window waited for is this one, or was replaced before it was seen: either way it is done with, so a
		// later redo of the same step brings the object back as an outside change.
		m_OwnDocument.reset();
		if (!own && m_Document != nullptr && !(*document == *m_Document))
		{
			// An outside change (undo, redo, automation, another window, a reload): a widget's pending value no longer applies.
			RecordChangedElements(*m_Document, *document);
			m_Preview.reset();
		}
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
				if (!found || !((*earlier)[*found] == element))
				{
					m_ChangedElements.push_back(element.ID);
				}
			}
		};
		record(current->Nodes, previous != nullptr ? &previous->Nodes : nullptr);
		record(current->Comments, previous != nullptr ? &previous->Comments : nullptr);
	}

	void VisualScriptEditorModel::RefreshAnalysis(VisualScriptSemantics& semantics, bool widgetActive, double time,
	                                              VisualScriptSyncResult& result)
	{
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
		bool const current = m_AnalyzedDocument != nullptr && !m_AnalysisStale && !libraryChanged &&
		                     (drawn == m_AnalyzedDocument || HaveSameVisualScriptProgram(*drawn, *m_AnalyzedDocument));
		if (current || (!libraryChanged && drawn == m_FailedDocument))
		{
			// The program the analysis describes (only positions, comments or collapsing changed), or the very document the
			// runtime could not analyze.
			m_AnalysisPending = false;
			return;
		}
		if (m_HasAnalyzed && (time == m_LastAnalysisTime || (widgetActive && time - m_LastAnalysisTime < AnalysisInterval)))
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
			m_AnalysisStale = false;
			m_FailedDocument.reset();
			IndexAnalysis();
			result.AnalysisChanged = true;
			return;
		}
		m_FailedDocument = drawn;
		if (m_Analysis != nullptr && !m_AnalysisStale)
		{
			m_AnalysisStale = true;
			result.AnalysisChanged = true;
		}
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

	std::optional<std::string> VisualScriptEditorModel::FindPinType(uint32_t node, std::string_view pin,
	                                                                VisualScriptPinDirection direction) const
	{
		if (m_Display == nullptr)
		{
			return std::nullopt;
		}
		VisualScriptDocument const& document = GetDrawnDocument();
		std::optional<uint64_t> const packed = FindByID(m_DrawnNodes, node);
		VisualScriptGraph const* graph = nullptr;
		VisualScriptNode const* stored = packed ? GetPackedNode(document, *packed, &graph) : nullptr;
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
}
