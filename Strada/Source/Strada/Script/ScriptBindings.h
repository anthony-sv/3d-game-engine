#pragma once

// Internal to the Script module: the native functions scripts call.

#include <cstdint>
#include <span>

namespace Strada
{
	// A native function and the name of the Strada.Interop.InternalCalls field it is assigned to (the layout of
	// Strada.Interop.Host.NativeBinding).
	struct ScriptBinding
	{
		char const* Name = nullptr;
		int32_t NameLength = 0;
		void (*Function)() = nullptr;
	};

	// Every binding; ScriptEngine::Init hands them to the scripting runtime, which fails on any mismatch.
	std::span<ScriptBinding const> GetScriptBindings();
}
