#include "stpch.h"
#include "Strada/Script/SaveSlots.h"

namespace Strada
{
	namespace
	{
		// The slot's field table reads and writes as a struct document.
		[[maybe_unused]] constexpr auto ReadSlotDocument = &ReadStructDocument<SaveSlotData>;
		[[maybe_unused]] constexpr auto WriteSlotDocument = &WriteStructDocument<SaveSlotData>;
	}
}
