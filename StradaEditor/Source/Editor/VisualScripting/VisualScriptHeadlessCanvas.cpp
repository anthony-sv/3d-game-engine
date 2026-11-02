#include "Editor/VisualScripting/VisualScriptHeadlessCanvas.h"

#include "Editor/VisualScripting/VisualScriptTextEstimate.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"

#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Core/Assert.h"

#include <utility>

namespace Strada
{
	Ref<VisualScriptDocument const> VisualScriptHeadlessCanvas::MakeDisplay(VisualScriptDocument const& document)
	{
		VisualScriptDocument display = document;
		PlaceUnplacedNodes(display);
		return CreateRef<VisualScriptDocument const>(std::move(display));
	}

	void VisualScriptHeadlessCanvas::Sync(VisualScriptDocument const& document, uint32_t function, VisualScriptAnalysis const* analysis,
	                                      VisualScriptDocument const* analyzedDocument, VisualScriptLibrary const* library)
	{
		Sync(MakeDisplay(document), function, analysis, analyzedDocument, library);
	}

	void VisualScriptHeadlessCanvas::Sync(Ref<VisualScriptDocument const> display, uint32_t function, VisualScriptAnalysis const* analysis,
	                                      VisualScriptDocument const* analyzedDocument, VisualScriptLibrary const* library)
	{
		ST_CORE_ASSERT(display != nullptr, "A headless canvas shows a document");
		m_Display = std::move(display);
		m_Analysis = analysis;
		m_Library = library;

		VisualScriptCanvasSource source;
		source.Display = m_Display.get();
		source.Function = function;
		source.Analysis = analysis;
		source.AnalyzedDocument = analyzedDocument;
		source.Library = library;
		if (library != nullptr)
		{
			source.EnumLookup = [library](std::string_view typeId)
			{
				return library->FindEnum(typeId);
			};
		}
		// Value chips have fixed widths, so assets' names (which a window shows) would change no size: handles stay decimal.
		static_cast<void>(m_Canvas.Sync(source, MakeEstimatingVisualScriptTextMeasurer(), VisualScriptThemeAnchors{}));
	}

	VisualScriptNodeExtents VisualScriptHeadlessCanvas::GetExtents() const
	{
		return [this](uint32_t node)
		{
			return m_Canvas.GetExtents(node);
		};
	}

	VisualScriptNodePins VisualScriptHeadlessCanvas::GetPins(std::optional<VisualScriptNewNodePins> newNode) const
	{
		return MakeVisualScriptCanvasNodePins(m_Canvas, m_Analysis, m_Display.get(), m_Library, newNode);
	}
}
