#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	// The state of a graph window's document, as each Sync finds its asset.
	enum class VisualScriptDocumentState : uint8_t
	{
		Editable = 0,
		// A memory asset: no file to write.
		ReadOnly,
		// A file without write permission.
		ReadOnlyFile,
		// The asset's file is missing.
		Missing,
		// The file does not load.
		Unloadable,
		// The handle is no longer registered (the asset was deleted): the window closes.
		Gone
	};

	// What a Sync found.
	struct VisualScriptSyncResult
	{
		VisualScriptDocumentState State = VisualScriptDocumentState::Editable;
		// The document drawn changed: an own commit, an outside change, a widget preview.
		bool DocumentChanged = false;
		// The analysis changed: a new one, or the last one kept without its problems.
		bool AnalysisChanged = false;
		// The visual model's geometry changed and the spatial index was rebuilt.
		bool GeometryChanged = false;
	};

	// The model of one graph window: which document it shows and how (the asset's document, with its unplaced nodes placed in
	// memory), the analysis policy, the visual model and its spatial index. Pure (no ImGui): the panel calls Sync every frame
	// before it reads input and draws. Main thread only.
	//
	// Documents are told apart by identity. The window's own commits come back as the very object the commit wrote
	// (OnCommitted); any other new document is an outside change (undo, redo, automation, another window, a reload), which
	// drops a widget preview and records the elements it changed for the renderer to flash.
	//
	// Analysis is a synchronous round trip to the scripting runtime, so it runs only when the program changes
	// (HaveSameVisualScriptProgram: moves, comments and collapsing never analyze) or the library changes, at most once per
	// frame, at most every AnalysisInterval while a widget is being used, and never without the runtime. Nodes changed since
	// the last analysis take their library template's pins until the next one.
	class VisualScriptEditorModel
	{
	public:
		// While a widget is being used, the analysis runs at most this often (the previous one keeps drawing).
		static constexpr double AnalysisInterval = 0.25;

		// settings is the per-user view settings the window's views are kept in; it must outlive the model.
		VisualScriptEditorModel(AssetHandle graph, VisualScriptViewSettings& settings);

		// Looks the asset up (never keeping it across frames as the document's source), adopts a changed document, analyzes
		// as the policy says, and brings the visual model and the index up to date. time is in seconds (ImGui's time): two
		// Syncs given the same time are one frame.
		VisualScriptSyncResult Sync(VisualScriptSemantics& semantics, VisualScriptTextMeasurer const& measure,
		                            VisualScriptThemeAnchors const& theme, bool widgetActive, double time);
		// The document the panel's commit just wrote (the request's document, the object the asset now holds). Call Sync again
		// in the same frame: it then adopts it as the window's own change.
		void OnCommitted(Ref<VisualScriptDocument const> committed);
		// The document as a widget's uncommitted value makes it, drawn (and analyzed at the policy's pace) until the widget
		// commits or ClearWidgetPreview; edits are still computed on GetDisplayDocument.
		void SetWidgetPreview(VisualScriptDocument preview);
		void ClearWidgetPreview();
		// Checks again, at the next Sync, whether the graph's file can be written: when the window gains focus, after a write
		// failed and on the read-only banner's Retry. (Sync checks by itself when the window opens and when the version
		// changes.)
		void CheckWritable();

		AssetHandle GetGraph() const { return m_Graph; }
		// The sub-graph shown: 0 for the event graph, else a function's ID. A function deleted while shown shows the event graph.
		uint32_t GetFunction() const { return m_Function; }
		VisualScriptDocumentState GetDocumentState() const { return m_State; }
		// In every state but Editable the model emits no edit intents; viewing, selecting and copying still work.
		bool IsReadOnly() const { return m_State != VisualScriptDocumentState::Editable; }
		VisualScriptCanvasModel const& GetCanvas() const { return m_Canvas; }
		VisualScriptSpatialIndex const& GetIndex() const { return m_Index; }
		// The asset's document with its unplaced nodes placed: what edits are computed on. Empty before the first document.
		VisualScriptDocument const& GetDisplayDocument() const;
		// The asset's own document object, as last seen; null before the first.
		Ref<VisualScriptDocument const> const& GetStoredDocument() const { return m_Document; }
		// Null without the scripting runtime (the last analysis is then kept for its pins only) and before the first analysis.
		VisualScriptAnalysis const* GetAnalysis() const;
		// The type of a pin of any node of the display document, as the canvas knows it (§6.4: the analysis while the node is
		// what it analyzed, else the library's template, else "*" for the pins its links and literals name); nothing for a pin
		// the node does not have (an orphan included). Valid until the next Sync.
		VisualScriptPinTypeLookup MakePinTypeLookup() const;
		// The nodes and comments of the shown graph that the last Sync's outside change added or changed (their stored data).
		std::span<uint32_t const> ChangedElements() const { return m_ChangedElements; }
		// How many analyses ran: for tests of the policy.
		uint64_t GetAnalysisCount() const { return m_AnalysisCount; }
		// Bytes the visual model, the index and the model's own lookups have reserved: an idle frame does not grow it.
		size_t GetCapacityBytes() const;

	private:
		using IdIndex = std::pair<uint32_t, uint32_t>;

		void LookUpAsset(VisualScriptSyncResult& result);
		void AdoptDocument(Ref<VisualScriptDocument const> document, VisualScriptSyncResult& result);
		void RecordChangedElements(VisualScriptDocument const& before, VisualScriptDocument const& after);
		void RefreshAnalysis(VisualScriptSemantics& semantics, bool widgetActive, double time, VisualScriptSyncResult& result);
		void IndexAnalysis();
		void IndexDisplay();
		VisualScriptDocument const& GetDrawnDocument() const;
		Ref<VisualScriptDocument const> const& GetDrawnDocumentRef() const;
		std::optional<std::string> FindPinType(uint32_t node, std::string_view pin, VisualScriptPinDirection direction) const;

		AssetHandle m_Graph;
		uint32_t m_Function = 0;
		VisualScriptDocumentState m_State = VisualScriptDocumentState::Editable;

		// What the last Sync saw (§6.2): the asset object and its version, its shared document, the display copy, and what
		// the panel's last commit wrote until Sync sees it.
		Ref<VisualScriptAsset> m_SeenAsset;
		uint64_t m_SeenVersion = 0;
		Ref<VisualScriptDocument const> m_Document;
		Ref<VisualScriptDocument const> m_Display;
		Ref<VisualScriptDocument const> m_OwnDocument;
		Ref<VisualScriptDocument const> m_Preview;
		bool m_PreviewChanged = false;
		// The time of the frame whose Syncs report m_ChangedElements.
		double m_LastSyncTime = -1.0e30;
		bool m_IsFileAsset = false;
		bool m_CheckWritable = true;
		bool m_Writable = true;

		// The analysis (§6.3): the library it was made with, the document it was made from, a document the runtime failed on.
		Ref<VisualScriptLibrary const> m_Library;
		Ref<VisualScriptAnalysis const> m_Analysis;
		Ref<VisualScriptDocument const> m_AnalyzedDocument;
		Ref<VisualScriptDocument const> m_FailedDocument;
		// The runtime stopped: the analysis is kept for its pins, without its problems.
		bool m_AnalysisStale = false;
		bool m_AnalysisPending = true;
		double m_LastAnalysisTime = -1.0e30;
		bool m_HasAnalyzed = false;
		uint64_t m_AnalysisCount = 0;

		// What the visual model was last synced with.
		bool m_CanvasStale = true;
		VisualScriptThemeAnchors m_Theme;
		uint64_t m_MeasureSignature = 0;
		uint64_t m_AssetVersion = 0;
		uint32_t m_CanvasFunction = 0;

		VisualScriptCanvasModel m_Canvas;
		VisualScriptSpatialIndex m_Index;
		std::vector<uint32_t> m_ChangedElements;
		// By ID: the analysis' nodes; the analyzed document's nodes and the drawn document's nodes, as (function, index) packed
		// in 64 bits.
		std::vector<IdIndex> m_AnalysisNodes;
		std::vector<std::pair<uint32_t, uint64_t>> m_AnalyzedNodes;
		std::vector<std::pair<uint32_t, uint64_t>> m_DrawnNodes;
	};
}
