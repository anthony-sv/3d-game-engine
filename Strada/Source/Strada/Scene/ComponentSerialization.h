#pragma once

#include "Strada/Scene/ComponentTraits.h"
#include "Strada/Serialization/StructSerialization.h"

namespace Strada
{
	// Writes every field of the component.
	template<RegisteredComponent T>
	Json SerializeComponent(T const& component)
	{
		return SerializeFields<ComponentTraits<T>>(component);
	}

	// Applies the fields present in the JSON object (missing fields keep their current values, so this also serves as a
	// partial update). The component is only modified when every field is valid.
	template<RegisteredComponent T>
	[[nodiscard]] Result<void> DeserializeComponent(Json const& json, T& component, DeserializationContext const& context)
	{
		return DeserializeFields<ComponentTraits<T>>(json, component, context, "component");
	}
}
