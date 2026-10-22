#include "Editor/VisualScriptEnumLookupSource.h"

#include "Strada/Script/VisualScriptEngine.h"

namespace Strada
{
	std::optional<VisualScriptEnumLookup> VisualScriptEnumLookupSource::Update()
	{
		VisualScriptLibraryState const state = VisualScriptEngine::GetLibraryState();
		bool const asksLibrary = state != VisualScriptLibraryState::NoRuntime && state != VisualScriptLibraryState::Failed;
		if (m_AsksLibrary == asksLibrary)
		{
			return std::nullopt;
		}
		m_AsksLibrary = asksLibrary;
		if (!asksLibrary)
		{
			return VisualScriptEnumLookup();
		}
		return VisualScriptEnumLookup(&VisualScriptEngine::FindEnum);
	}
}
