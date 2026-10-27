#include "stpch.h"
#include "Strada/Asset/VisualScriptReferences.h"

#include "Strada/Core/UUID.h"

#include <string>

namespace Strada
{
	namespace
	{
		constexpr char const* GraphSetting = "Graph";
	}

	std::optional<AssetHandle> ReadVisualScriptGraphSetting(VisualScriptNode const& node)
	{
		if (!node.Settings.is_object())
		{
			return std::nullopt;
		}
		auto const setting = node.Settings.find(GraphSetting);
		if (setting == node.Settings.end() || !setting->is_string())
		{
			return std::nullopt;
		}
		std::optional<UUID> const id = UUID::FromString(setting->get_ref<std::string const&>());
		if (!id || !id->IsValid())
		{
			return std::nullopt;
		}
		return AssetHandle(*id);
	}

	bool IsLocalVisualScriptReference(VisualScriptNode const& node)
	{
		return !node.Settings.is_object() || !node.Settings.contains(GraphSetting);
	}
}
