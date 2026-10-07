#pragma once

#include "Strada/Core/PlatformDetection.h"

#include <cstdint>
#include <memory>
#include <utility>

// Build configuration. Exactly one of ST_DEBUG, ST_RELEASE or ST_DIST is defined by the build system.
#if !defined(ST_DEBUG) && !defined(ST_RELEASE) && !defined(ST_DIST)
#error "No Strada build configuration defined (expected ST_DEBUG, ST_RELEASE or ST_DIST)."
#endif

#if defined(ST_DEBUG) || defined(ST_RELEASE)
#define ST_ENABLE_ASSERTS
#endif

#define ST_EXPAND_MACRO(x) x
#define ST_STRINGIFY_MACRO(x) #x

#define ST_BIT(x) (1u << (x))

// Binds a member function as a callback, forwarding all arguments: ST_BIND_EVENT_FN(Application::OnEvent).
#define ST_BIND_EVENT_FN(fn)                                    \
	[this](auto&&... args) -> decltype(auto)                    \
	{                                                           \
		return this->fn(std::forward<decltype(args)>(args)...); \
	}

namespace Strada
{
	template<typename T>
	using Scope = std::unique_ptr<T>;

	template<typename T, typename... Args>
	[[nodiscard]] constexpr Scope<T> CreateScope(Args&&... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template<typename T>
	using Ref = std::shared_ptr<T>;

	template<typename T, typename... Args>
	[[nodiscard]] constexpr Ref<T> CreateRef(Args&&... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}
}
