#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <string>
#include <vector>

namespace Strada::Testing
{
	// The strict check commits made before VisualScriptSerializer::Validate replaced it, kept as Validate's oracle: the
	// file's Json, read back as automation reads documents (unknown keys are issues), must give the document itself, with no
	// repair and every value alike (HaveSameVisualScriptProgram). Appends the loader's repairs to issues.
	inline bool LoadsBackAsWritten(VisualScriptDocument const& document, std::vector<VisualScriptIssue>& issues)
	{
		std::vector<std::string> warnings;
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		context.Warnings = &warnings;
		Result<VisualScriptDocument> const read =
			VisualScriptSerializer::Deserialize(VisualScriptSerializer::Serialize(document), context, &issues);
		return read.IsOk() && issues.empty() && read.GetValue() == document && HaveSameVisualScriptProgram(read.GetValue(), document);
	}
}
