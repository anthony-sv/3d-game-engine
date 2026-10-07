#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Strada
{
	enum class ShaderStage : uint8_t
	{
		Vertex = 0,
		Pixel,
		Compute
	};

	// SPIR-V blob compiled from Strada/Shaders at build time.
	struct EmbeddedShader
	{
		char const* Name;
		ShaderStage Stage;
		char const* EntryPoint;
		uint8_t const* Data;
		size_t Size;
	};

	// Defined in the build-generated EmbeddedShaders.cpp (see cmake/StradaShaders.cmake).
	std::span<EmbeddedShader const> GetEmbeddedShaders();
}
