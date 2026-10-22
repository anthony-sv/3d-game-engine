#pragma once

#include "Strada/Script/VisualScriptVariables.h"

#include <optional>

namespace Strada
{
	// The enum lookup the inspector gives the public variables of visual scripts (InspectorPanel::SetVisualScriptEnumLookup),
	// kept in step with the scripting runtime without reading its node library, which takes a moment: the lookup asks the
	// library when it is asked about a type (VisualScriptEngine::FindEnum), so the library is read only once a variable that
	// may be an enum is shown, and again only then after a game-assembly change dropped it. While the runtime is not running
	// or its library cannot be read, the lookup is empty, so such variables read as needing the runtime. Main thread only.
	class VisualScriptEnumLookupSource
	{
	public:
		// The lookup to give the inspector now, or nothing while the one given last still holds. Reads nothing, so the
		// editor asks every frame.
		[[nodiscard]] std::optional<VisualScriptEnumLookup> Update();

	private:
		// Whether the lookup given last asks the library; nothing before the first.
		std::optional<bool> m_AsksLibrary;
	};
}
