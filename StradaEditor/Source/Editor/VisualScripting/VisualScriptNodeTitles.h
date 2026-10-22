#pragma once

#include "Strada/Asset/VisualScriptAsset.h"

#include <string>

namespace Strada
{
	// The title of a node without the scripting runtime's analysis (the canvas without a node library, find), derived from
	// the stored document alone as the runtime titles nodes; the analysis' titles win wherever there is one:
	// - members of the scripting API by their name in words: "Call:Strada.Input.IsKeyDown(Strada.KeyCode)" is "Is Key
	//   Down", "Get:..."/"Set:..." are "Get Translation"/"Set Translation", and "New:Strada.Vector3(float,float,float)" is
	//   "Make Vector3";
	// - Variable.Get/Set as "Get <variable>"/"Set <variable>" (a variable of the document, or a local of the function whose
	//   graph holds the node), Function.Call as its function's name, Event.Custom as its Name and Event.Send as "Send
	//   <Name>"; when the setting names none of them, as the node library titles the kind ("Get Variable", "Set Variable",
	//   "Call Function", "Custom Event", "Send Event");
	// - every other type by the words of its last segment: "Flow.ForLoop" is "For Loop", "IsValid" is "Is Valid", and
	//   types the runtime does not know likewise.
	// Words start at a capital after a lowercase letter or a digit and at the last capital of an acronym followed by a
	// lowercase letter ("UIButton" is "UI Button", "Deg2Rad" is "Deg2 Rad"), and '_' separates them.
	[[nodiscard]] std::string DeriveVisualScriptNodeTitle(VisualScriptNode const& node, VisualScriptDocument const& document);
}
