// Lets structure definitions be shared between HLSL and C++. In C++ the HLSL vector and matrix types map to glm
// (column-major, like HLSL constant buffers compiled with -fvk-use-dx-layout); every structure is static_assert-checked
// on the C++ side.
#ifndef STRADA_SHADER_INTEROP_H
#define STRADA_SHADER_INTEROP_H

#if defined(__cplusplus)
#include <glm/glm.hpp>

#include <cstdint>

#define ST_SHADER_NAMESPACE_BEGIN   \
	namespace Strada::ShaderInterop \
	{                               \
		using float2 = glm::vec2;   \
		using float3 = glm::vec3;   \
		using float4 = glm::vec4;   \
		using float4x4 = glm::mat4; \
		using uint = uint32_t;      \
		using uint2 = glm::uvec2;
#define ST_SHADER_NAMESPACE_END }
#else
#define ST_SHADER_NAMESPACE_BEGIN
#define ST_SHADER_NAMESPACE_END
#endif

#endif
