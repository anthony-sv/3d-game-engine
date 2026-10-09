#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace Strada
{
	// Files compiled into the engine binary (strada_embed_resources in cmake/StradaResources.cmake), so the engine and
	// exported games need no data files for them. Thread-safe (the data is immutable).
	namespace EmbeddedResources
	{
		// The default font of text rendering (Roboto Medium, Apache License 2.0).
		inline constexpr std::string_view DefaultFont = "Fonts/Roboto-Medium.ttf";

		// The contents of an embedded file; empty when there is none with that name.
		std::span<uint8_t const> Get(std::string_view name);
	}
}
