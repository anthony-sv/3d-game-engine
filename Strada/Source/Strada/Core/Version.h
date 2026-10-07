#pragma once

#include <cstdint>

// ST_VERSION_* are defined by the build system from the CMake project version.
#if !defined(ST_VERSION_MAJOR) || !defined(ST_VERSION_MINOR) || !defined(ST_VERSION_PATCH) || !defined(ST_VERSION_STRING)
#error "Strada version macros are not defined by the build system."
#endif

namespace Strada
{
	struct EngineVersion
	{
		static constexpr uint32_t Major = ST_VERSION_MAJOR;
		static constexpr uint32_t Minor = ST_VERSION_MINOR;
		static constexpr uint32_t Patch = ST_VERSION_PATCH;
		static constexpr char const* String = ST_VERSION_STRING;
	};
}
