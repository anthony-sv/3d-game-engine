#pragma once

#include "Strada/Asset/FontAsset.h"
#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Renderer/FontAtlas.h"

#include <doctest/doctest.h>

namespace Strada::Testing
{
	// The engine's default font (compiled into the engine).
	inline Ref<FontAsset> LoadDefaultFont()
	{
		std::span<uint8_t const> const data = EmbeddedResources::Get(EmbeddedResources::DefaultFont);
		Result<Ref<FontAsset>> font = FontAsset::Create(Buffer::Copy(data.data(), data.size()));
		REQUIRE(font.IsOk());
		return font.TakeValue();
	}

	inline Scope<FontAtlas> CreateDefaultFontAtlas()
	{
		Result<Scope<FontAtlas>> atlas = FontAtlas::Create(LoadDefaultFont());
		REQUIRE(atlas.IsOk());
		return atlas.TakeValue();
	}
}
