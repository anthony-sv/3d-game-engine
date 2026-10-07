#pragma once

#include "Strada/Core/Base.h"
#include "Strada/RHI/EmbeddedShaders.h"

#include <nvrhi/nvrhi.h>

#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// Creates NVRHI shader objects from the embedded SPIR-V blobs on first use and caches them. Main thread only;
	// requires an initialized GraphicsDevice.
	class ShaderLibrary
	{
	public:
		static void Init();
		static void Shutdown();
		static bool IsInitialized();

		// Returns null (and logs an error) if no embedded shader has this name.
		static nvrhi::ShaderHandle Get(std::string_view name);
		static bool Contains(std::string_view name);
		static std::vector<std::string> GetNames();
	};
}
