#include "stpch.h"
#include "Strada/Script/PlayerSettings.h"

namespace Strada
{
	namespace
	{
		// The settings' field tables read and write as a struct document.
		[[maybe_unused]] constexpr auto ReadSettingsDocument = &ReadStructDocument<PlayerSettings>;
		[[maybe_unused]] constexpr auto WriteSettingsDocument = &WriteStructDocument<PlayerSettings>;
	}
}
