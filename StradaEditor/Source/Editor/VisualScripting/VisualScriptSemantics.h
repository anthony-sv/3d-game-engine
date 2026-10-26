#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Script/VisualScriptLibrary.h"

namespace Strada
{
	// Where the visual script editor gets what only the scripting runtime knows: the node library and the analysis of a
	// document (their nodes' pins, titles and problems). The editor's windows use the engine's; tests substitute a fixture
	// library. Main thread only.
	class VisualScriptSemantics
	{
	public:
		virtual ~VisualScriptSemantics() = default;

		// The node library; null without the scripting runtime. A library that is read again (after the game assembly
		// changed) is a new object, so a different pointer means that what was derived from the previous one is stale.
		virtual Ref<VisualScriptLibrary const> GetLibrary() = 0;
		// The analysis of a document. Fails without the scripting runtime, or when the runtime fails.
		[[nodiscard]] virtual Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const& document) = 0;
	};

	// The scripting runtime's library and analysis (VisualScriptEngine) while the runtime runs; a null library and an error
	// when it does not.
	class EngineVisualScriptSemantics final : public VisualScriptSemantics
	{
	public:
		Ref<VisualScriptLibrary const> GetLibrary() override;
		[[nodiscard]] Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const& document) override;
	};
}
