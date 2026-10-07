#include "stpch.h"
#include "Strada/RHI/ShaderLibrary.h"

#include "Strada/RHI/GraphicsDevice.h"

#include <unordered_map>

namespace Strada
{
	namespace
	{
		struct ShaderLibraryData
		{
			std::unordered_map<std::string, EmbeddedShader const*> Blobs;
			std::unordered_map<std::string, nvrhi::ShaderHandle> Shaders;
		};

		Scope<ShaderLibraryData> s_Data;

		nvrhi::ShaderType ToShaderType(ShaderStage stage)
		{
			switch (stage)
			{
				case ShaderStage::Vertex:
					return nvrhi::ShaderType::Vertex;
				case ShaderStage::Pixel:
					return nvrhi::ShaderType::Pixel;
				case ShaderStage::Compute:
					return nvrhi::ShaderType::Compute;
			}
			return nvrhi::ShaderType::None;
		}
	}

	void ShaderLibrary::Init()
	{
		ST_CORE_ASSERT(!s_Data, "ShaderLibrary is already initialized");
		ST_CORE_ASSERT(GraphicsDevice::IsInitialized(), "ShaderLibrary requires an initialized GraphicsDevice");

		s_Data = CreateScope<ShaderLibraryData>();
		for (EmbeddedShader const& shader : GetEmbeddedShaders())
		{
			auto const [it, inserted] = s_Data->Blobs.emplace(shader.Name, &shader);
			ST_CORE_ASSERT(inserted, "Duplicate embedded shader name '{}'", shader.Name);
			(void)it;
		}
		ST_CORE_TRACE("Shader library: {} embedded shaders", s_Data->Blobs.size());
	}

	void ShaderLibrary::Shutdown()
	{
		s_Data.reset();
	}

	bool ShaderLibrary::IsInitialized()
	{
		return s_Data != nullptr;
	}

	nvrhi::ShaderHandle ShaderLibrary::Get(std::string_view name)
	{
		ST_CORE_ASSERT(s_Data, "ShaderLibrary is not initialized");

		std::string const key(name);
		if (auto const cached = s_Data->Shaders.find(key); cached != s_Data->Shaders.end())
		{
			return cached->second;
		}

		auto const blob = s_Data->Blobs.find(key);
		if (blob == s_Data->Blobs.end())
		{
			ST_CORE_ERROR("Shader '{}' is not in the shader library", name);
			return nullptr;
		}

		EmbeddedShader const& embedded = *blob->second;
		nvrhi::ShaderDesc description;
		description.shaderType = ToShaderType(embedded.Stage);
		description.debugName = embedded.Name;
		description.entryName = embedded.EntryPoint;

		nvrhi::ShaderHandle shader = GraphicsDevice::GetDevice()->createShader(description, embedded.Data, embedded.Size);
		if (!shader)
		{
			ST_CORE_ERROR("Failed to create shader '{}'", name);
			return nullptr;
		}

		s_Data->Shaders.emplace(key, shader);
		return shader;
	}

	bool ShaderLibrary::Contains(std::string_view name)
	{
		ST_CORE_ASSERT(s_Data, "ShaderLibrary is not initialized");
		return s_Data->Blobs.contains(std::string(name));
	}

	std::vector<std::string> ShaderLibrary::GetNames()
	{
		ST_CORE_ASSERT(s_Data, "ShaderLibrary is not initialized");
		std::vector<std::string> names;
		names.reserve(s_Data->Blobs.size());
		for (auto const& [name, blob] : s_Data->Blobs)
		{
			names.push_back(name);
		}
		std::sort(names.begin(), names.end());
		return names;
	}
}
