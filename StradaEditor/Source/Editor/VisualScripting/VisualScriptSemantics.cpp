#include "Editor/VisualScripting/VisualScriptSemantics.h"

#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

namespace Strada
{
	Ref<VisualScriptLibrary const> EngineVisualScriptSemantics::GetLibrary()
	{
		if (!ScriptEngine::IsInitialized())
		{
			return nullptr;
		}
		return VisualScriptEngine::GetLibrary();
	}

	Result<VisualScriptAnalysis> EngineVisualScriptSemantics::Analyze(VisualScriptDocument const& document)
	{
		if (!ScriptEngine::IsInitialized())
		{
			return Error{"the scripting runtime is not running"};
		}
		return VisualScriptEngine::Analyze(document);
	}
}
