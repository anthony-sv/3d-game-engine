#pragma once

// Platform detection. Exactly one ST_PLATFORM_* macro is defined for every supported target.
#if defined(_WIN32)
#if defined(_WIN64)
#define ST_PLATFORM_WINDOWS
#else
#error "Strada requires a 64-bit Windows target."
#endif
#elif defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX == 1
#define ST_PLATFORM_MACOS
#else
#error "Unsupported Apple platform: Strada supports macOS only."
#endif
#elif defined(__linux__)
#define ST_PLATFORM_LINUX
#else
#error "Unsupported platform: Strada supports Windows, macOS and Linux."
#endif

// Compiler detection. clang-cl defines both _MSC_VER and __clang__ and is treated as Clang.
#if defined(__clang__)
#define ST_COMPILER_CLANG
#elif defined(_MSC_VER)
#define ST_COMPILER_MSVC
#elif defined(__GNUC__)
#define ST_COMPILER_GCC
#else
#error "Unsupported compiler: Strada supports MSVC, Clang and GCC."
#endif
