#include "stpch.h"
#include "Strada/Asset/VisualScriptAsset.h"

namespace Strada
{
	void VisualScriptAsset::SetDocument(VisualScriptDocument document)
	{
		m_Document = std::move(document);
		m_Version++;
	}

	VisualScriptDocument MakeVisualScriptTemplate(VisualScriptTemplate kind)
	{
		VisualScriptDocument document;
		switch (kind)
		{
			case VisualScriptTemplate::Events:
			{
				VisualScriptNode create;
				create.ID = 1;
				create.Type = "Event.Create";
				create.Position = glm::vec2(0.0f, 0.0f);
				VisualScriptNode update;
				update.ID = 2;
				update.Type = "Event.Update";
				update.Position = glm::vec2(0.0f, 160.0f);
				document.Graph.Nodes.push_back(std::move(create));
				document.Graph.Nodes.push_back(std::move(update));
				break;
			}
			case VisualScriptTemplate::Empty:
				break;
		}
		return document;
	}
}
