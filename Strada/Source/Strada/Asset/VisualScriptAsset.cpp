#include "stpch.h"
#include "Strada/Asset/VisualScriptAsset.h"

#include <array>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<std::pair<VisualScriptNodeEnabled, std::string_view>, 3> s_EnabledNames = {{
			{VisualScriptNodeEnabled::Enabled, "Enabled"},
			{VisualScriptNodeEnabled::Disabled, "Disabled"},
			{VisualScriptNodeEnabled::DevelopmentOnly, "DevelopmentOnly"},
		}};
	}

	std::string_view VisualScriptNodeEnabledToString(VisualScriptNodeEnabled enabled)
	{
		for (auto const& [value, name] : s_EnabledNames)
		{
			if (value == enabled)
			{
				return name;
			}
		}
		return {};
	}

	std::optional<VisualScriptNodeEnabled> VisualScriptNodeEnabledFromString(std::string_view text)
	{
		for (auto const& [value, name] : s_EnabledNames)
		{
			if (text == name)
			{
				return value;
			}
		}
		return std::nullopt;
	}

	VisualScriptAsset::VisualScriptAsset(VisualScriptDocument document)
		: m_Document(CreateRef<VisualScriptDocument const>(std::move(document)))
	{
	}

	void VisualScriptAsset::SetDocument(VisualScriptDocument document)
	{
		SetDocument(CreateRef<VisualScriptDocument const>(std::move(document)));
	}

	void VisualScriptAsset::SetDocument(Ref<VisualScriptDocument const> document)
	{
		ST_CORE_ASSERT(document != nullptr, "A visual script's document is never null");
		if (document == nullptr)
		{
			return;
		}
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
