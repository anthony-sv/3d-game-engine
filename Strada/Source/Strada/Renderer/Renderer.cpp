#include "stpch.h"
#include "Strada/Renderer/Renderer.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Renderer/EnvironmentMap.h"
#include "Strada/Renderer/TextureMips.h"

#include "RendererInterop.h"

#include <array>
#include <map>
#include <unordered_map>

namespace Strada
{
	static_assert(sizeof(ShaderInterop::MaterialConstants) % 16 == 0);

	namespace
	{
		struct MeshEntry
		{
			std::weak_ptr<MeshSource> Source;
			GpuMesh Mesh;
		};

		struct TextureEntry
		{
			std::weak_ptr<TextureAsset> Source;
			nvrhi::TextureHandle Texture;
		};

		struct FontEntry
		{
			std::weak_ptr<FontAsset> Source;
			GpuFont Font;
		};

		struct EnvironmentEntry
		{
			std::weak_ptr<EnvironmentAsset> Source;
			GpuEnvironment Environment;
		};

		struct MaterialEntry
		{
			std::weak_ptr<MaterialAsset> Source;
			uint64_t Version = 0;
			std::array<nvrhi::ITexture*, 5> Textures{};
			nvrhi::BufferHandle Constants;
			nvrhi::BindingSetHandle BindingSet;
		};

		struct RendererData
		{
			nvrhi::SamplerHandle MaterialSampler;
			nvrhi::SamplerHandle LinearClampSampler;
			nvrhi::BindingLayoutHandle MaterialLayout;
			nvrhi::CommandListHandle UploadCommandList;
			EnvironmentProcessor Environments;
			nvrhi::TextureHandle BrdfLut;
			nvrhi::TextureHandle FallbackCube;

			std::unordered_map<MeshSource const*, MeshEntry> Meshes;
			// Keyed by (texture, sRGB).
			std::map<std::pair<TextureAsset const*, bool>, TextureEntry> Textures;
			std::unordered_map<MaterialAsset const*, MaterialEntry> Materials;
			std::unordered_map<EnvironmentAsset const*, EnvironmentEntry> EnvironmentMaps;
			// Node-based: GpuFont pointers stay valid until their entry is collected.
			std::unordered_map<FontAsset const*, FontEntry> Fonts;
			// Assets that failed to upload (reported once; retried when the asset object is replaced).
			std::unordered_map<Asset const*, std::weak_ptr<Asset const>> Failures;
		};

		Scope<RendererData> s_Data;

		RendererData& GetData()
		{
			ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
			return *s_Data;
		}

		bool HasFailed(Ref<Asset const> const& asset)
		{
			RendererData const& data = GetData();
			auto const it = data.Failures.find(asset.get());
			return it != data.Failures.end() && it->second.lock() == asset;
		}

		void ReportFailure(Ref<Asset const> const& asset, std::string const& message)
		{
			GetData().Failures[asset.get()] = asset;
			ST_CORE_ERROR("{}", message);
		}

		nvrhi::TextureHandle UploadTexture(Ref<TextureAsset> const& textureAsset, bool srgb)
		{
			TextureAsset const& asset = *textureAsset;
			Result<Image> image = asset.Decode();
			if (!image)
			{
				ReportFailure(textureAsset, fmt::format("Texture {} could not be decoded: {}", asset.Handle, image.GetError()));
				return nullptr;
			}

			std::vector<Image> const levels = GenerateMipChain(image.GetValue(), srgb);
			nvrhi::IDevice* device = GraphicsDevice::GetDevice();
			nvrhi::TextureDesc desc;
			desc.width = image.GetValue().GetWidth();
			desc.height = image.GetValue().GetHeight();
			desc.mipLevels = static_cast<uint32_t>(levels.size());
			desc.format = srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = fmt::format("Texture {}", asset.Handle);
			nvrhi::TextureHandle texture = device->createTexture(desc);
			if (!texture)
			{
				ReportFailure(textureAsset, fmt::format("Failed to create the GPU texture of {}", asset.Handle));
				return nullptr;
			}

			nvrhi::ICommandList* commandList = GetData().UploadCommandList;
			commandList->open();
			for (uint32_t level = 0; level < levels.size(); level++)
			{
				commandList->writeTexture(texture, 0, level, levels[level].GetPixels().data(),
				                          static_cast<size_t>(levels[level].GetWidth()) * 4);
			}
			commandList->close();
			device->executeCommandList(commandList);
			return texture;
		}

		nvrhi::ITexture* GetBuiltInTexture(AssetHandle handle, bool srgb)
		{
			return Renderer::GetTexture(handle, srgb, AssetHandle());
		}
	}

	Result<void> Renderer::Init()
	{
		ST_CORE_ASSERT(!s_Data, "Renderer is already initialized");
		ST_CORE_ASSERT(GraphicsDevice::IsInitialized(), "Renderer requires the GraphicsDevice");
		s_Data = CreateScope<RendererData>();
		RendererData& data = *s_Data;
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();

		nvrhi::SamplerDesc materialSampler;
		materialSampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(16.0f);
		data.MaterialSampler = device->createSampler(materialSampler);
		nvrhi::SamplerDesc clampSampler;
		clampSampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		data.LinearClampSampler = device->createSampler(clampSampler);

		nvrhi::BindingLayoutDesc materialLayout;
		materialLayout.visibility = nvrhi::ShaderType::All;
		materialLayout.registerSpace = 1;
		materialLayout.registerSpaceIsDescriptorSet = true;
		materialLayout.bindings = {
			nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),    nvrhi::BindingLayoutItem::Texture_SRV(3), nvrhi::BindingLayoutItem::Texture_SRV(4),
		};
		data.MaterialLayout = device->createBindingLayout(materialLayout);
		data.UploadCommandList = device->createCommandList();

		if (!data.MaterialSampler || !data.LinearClampSampler || !data.MaterialLayout || !data.UploadCommandList)
		{
			s_Data.reset();
			return Error{"Failed to create the renderer's shared GPU resources"};
		}

		if (Result<void> environments = data.Environments.Init(); !environments)
		{
			s_Data.reset();
			return environments;
		}
		Result<nvrhi::TextureHandle> lut = data.Environments.CreateBrdfLut(data.UploadCommandList);
		if (!lut)
		{
			s_Data.reset();
			return Error{lut.GetError()};
		}
		data.BrdfLut = lut.GetValue();

		nvrhi::TextureDesc cubeDesc;
		cubeDesc.dimension = nvrhi::TextureDimension::TextureCube;
		cubeDesc.width = 1;
		cubeDesc.height = 1;
		cubeDesc.arraySize = 6;
		cubeDesc.format = nvrhi::Format::RGBA16_FLOAT;
		cubeDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		cubeDesc.keepInitialState = true;
		cubeDesc.debugName = "Fallback cube";
		data.FallbackCube = device->createTexture(cubeDesc);
		if (!data.FallbackCube)
		{
			s_Data.reset();
			return Error{"Failed to create the fallback cubemap"};
		}
		uint16_t const black[4] = {0, 0, 0, 0};
		data.UploadCommandList->open();
		for (uint32_t face = 0; face < 6; face++)
		{
			data.UploadCommandList->writeTexture(data.FallbackCube, face, 0, black, sizeof(black));
		}
		data.UploadCommandList->close();
		device->executeCommandList(data.UploadCommandList);
		return {};
	}

	void Renderer::Shutdown()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		GraphicsDevice::WaitForIdle();
		s_Data.reset();
	}

	bool Renderer::IsInitialized()
	{
		return s_Data != nullptr;
	}

	nvrhi::ISampler* Renderer::GetMaterialSampler()
	{
		return GetData().MaterialSampler;
	}

	nvrhi::ISampler* Renderer::GetLinearClampSampler()
	{
		return GetData().LinearClampSampler;
	}

	nvrhi::IBindingLayout* Renderer::GetMaterialBindingLayout()
	{
		return GetData().MaterialLayout;
	}

	GpuMesh const* Renderer::GetMesh(Ref<MeshSource> const& mesh)
	{
		RendererData& data = GetData();
		if (auto const it = data.Meshes.find(mesh.get()); it != data.Meshes.end() && it->second.Source.lock() == mesh)
		{
			return &it->second.Mesh;
		}
		if (HasFailed(mesh))
		{
			return nullptr;
		}

		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::BufferDesc vertexDesc;
		vertexDesc.byteSize = mesh->GetVertices().size() * sizeof(Vertex);
		vertexDesc.isVertexBuffer = true;
		vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
		vertexDesc.keepInitialState = true;
		vertexDesc.debugName = fmt::format("Mesh {} vertices", mesh->Handle);
		nvrhi::BufferDesc indexDesc;
		indexDesc.byteSize = mesh->GetIndices().size() * sizeof(uint32_t);
		indexDesc.isIndexBuffer = true;
		indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
		indexDesc.keepInitialState = true;
		indexDesc.debugName = fmt::format("Mesh {} indices", mesh->Handle);

		GpuMesh gpuMesh;
		gpuMesh.VertexBuffer = device->createBuffer(vertexDesc);
		gpuMesh.IndexBuffer = device->createBuffer(indexDesc);
		if (!gpuMesh.VertexBuffer || !gpuMesh.IndexBuffer)
		{
			ReportFailure(mesh, fmt::format("Failed to create the GPU buffers of mesh {}", mesh->Handle));
			return nullptr;
		}

		nvrhi::ICommandList* commandList = data.UploadCommandList;
		commandList->open();
		commandList->writeBuffer(gpuMesh.VertexBuffer, mesh->GetVertices().data(), vertexDesc.byteSize);
		commandList->writeBuffer(gpuMesh.IndexBuffer, mesh->GetIndices().data(), indexDesc.byteSize);
		commandList->close();
		device->executeCommandList(commandList);

		MeshEntry& entry = data.Meshes[mesh.get()];
		entry.Source = mesh;
		entry.Mesh = std::move(gpuMesh);
		return &entry.Mesh;
	}

	nvrhi::ITexture* Renderer::GetTexture(AssetHandle handle, bool srgb, AssetHandle fallback)
	{
		RendererData& data = GetData();
		Ref<TextureAsset> const asset = handle.IsValid() ? AssetManager::GetAsset<TextureAsset>(handle) : nullptr;
		if (!asset)
		{
			return fallback.IsValid() ? GetBuiltInTexture(fallback, srgb) : nullptr;
		}

		std::pair<TextureAsset const*, bool> const key(asset.get(), srgb);
		if (auto const it = data.Textures.find(key); it != data.Textures.end() && it->second.Source.lock() == asset)
		{
			return it->second.Texture;
		}
		if (HasFailed(asset))
		{
			return fallback.IsValid() ? GetBuiltInTexture(fallback, srgb) : nullptr;
		}

		nvrhi::TextureHandle texture = UploadTexture(asset, srgb);
		if (!texture)
		{
			return fallback.IsValid() ? GetBuiltInTexture(fallback, srgb) : nullptr;
		}
		TextureEntry& entry = data.Textures[key];
		entry.Source = asset;
		entry.Texture = std::move(texture);
		return entry.Texture;
	}

	nvrhi::IBindingSet* Renderer::GetMaterialBindingSet(Ref<MaterialAsset> const& material)
	{
		RendererData& data = GetData();
		MaterialData const& parameters = material->GetData();
		AssetHandle const white = GetBuiltInHandle(BuiltInAsset::WhiteTexture);
		std::array<nvrhi::ITexture*, 5> const textures = {
			GetTexture(parameters.BaseColorTexture, true, white),
			GetTexture(parameters.NormalTexture, false, GetBuiltInHandle(BuiltInAsset::FlatNormalTexture)),
			GetTexture(parameters.MetallicRoughnessTexture, false, white),
			GetTexture(parameters.OcclusionTexture, false, white),
			GetTexture(parameters.EmissiveTexture, true, white),
		};
		for (nvrhi::ITexture* texture : textures)
		{
			if (texture == nullptr)
			{
				return nullptr;
			}
		}

		MaterialEntry& entry = data.Materials[material.get()];
		bool const current = entry.Source.lock() == material && entry.BindingSet;
		if (current && entry.Version == material->GetVersion() && entry.Textures == textures)
		{
			return entry.BindingSet;
		}

		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		if (entry.Source.lock() != material || !entry.Constants)
		{
			nvrhi::BufferDesc constantsDesc;
			constantsDesc.byteSize = sizeof(ShaderInterop::MaterialConstants);
			constantsDesc.isConstantBuffer = true;
			constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			constantsDesc.keepInitialState = true;
			constantsDesc.debugName = fmt::format("Material {} constants", material->Handle);
			entry.Constants = device->createBuffer(constantsDesc);
			if (!entry.Constants)
			{
				data.Materials.erase(material.get());
				ReportFailure(material, fmt::format("Failed to create the constants of material {}", material->Handle));
				return nullptr;
			}
		}

		ShaderInterop::MaterialConstants constants{};
		constants.BaseColor = parameters.BaseColor;
		constants.Emissive = parameters.EmissiveColor * parameters.EmissiveIntensity;
		constants.Metallic = parameters.Metallic;
		constants.Roughness = parameters.Roughness;
		constants.NormalStrength = parameters.NormalStrength;
		constants.OcclusionStrength = parameters.OcclusionStrength;
		constants.AlphaCutoff = parameters.AlphaCutoff;
		constants.UVTiling = parameters.UVTiling;
		constants.UVOffset = parameters.UVOffset;
		constants.Flags = (parameters.AlphaMode == MaterialAlphaMode::Mask ? ShaderInterop::MaterialFlagAlphaMask : 0u) |
		                  (parameters.NormalTexture.IsValid() ? ShaderInterop::MaterialFlagHasNormalMap : 0u);

		nvrhi::ICommandList* commandList = data.UploadCommandList;
		commandList->open();
		commandList->writeBuffer(entry.Constants, &constants, sizeof(constants));
		commandList->close();
		device->executeCommandList(commandList);

		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, entry.Constants), nvrhi::BindingSetItem::Texture_SRV(0, textures[0]),
			nvrhi::BindingSetItem::Texture_SRV(1, textures[1]),        nvrhi::BindingSetItem::Texture_SRV(2, textures[2]),
			nvrhi::BindingSetItem::Texture_SRV(3, textures[3]),        nvrhi::BindingSetItem::Texture_SRV(4, textures[4]),
		};
		entry.BindingSet = device->createBindingSet(setDesc, data.MaterialLayout);
		entry.Source = material;
		entry.Version = material->GetVersion();
		entry.Textures = textures;
		return entry.BindingSet;
	}

	GpuEnvironment const* Renderer::GetEnvironment(Ref<EnvironmentAsset> const& environment)
	{
		RendererData& data = GetData();
		if (auto const it = data.EnvironmentMaps.find(environment.get());
		    it != data.EnvironmentMaps.end() && it->second.Source.lock() == environment)
		{
			return &it->second.Environment;
		}
		if (HasFailed(environment))
		{
			return nullptr;
		}

		Result<HdrImage> image = environment->Decode();
		if (!image)
		{
			ReportFailure(environment, fmt::format("Environment {} could not be decoded: {}", environment->Handle, image.GetError()));
			return nullptr;
		}
		Result<GpuEnvironment> processed = data.Environments.Process(image.GetValue(), data.UploadCommandList);
		if (!processed)
		{
			ReportFailure(environment, fmt::format("Environment {} could not be processed: {}", environment->Handle, processed.GetError()));
			return nullptr;
		}
		EnvironmentEntry& entry = data.EnvironmentMaps[environment.get()];
		entry.Source = environment;
		entry.Environment = std::move(processed.GetValue());
		return &entry.Environment;
	}

	nvrhi::ITexture* Renderer::GetBrdfLut()
	{
		return GetData().BrdfLut;
	}

	nvrhi::ITexture* Renderer::GetFallbackCube()
	{
		return GetData().FallbackCube;
	}

	GpuFont* Renderer::GetFont(Ref<FontAsset> const& font)
	{
		RendererData& data = GetData();
		if (!font)
		{
			return nullptr;
		}
		if (auto const it = data.Fonts.find(font.get()); it != data.Fonts.end() && it->second.Source.lock() == font)
		{
			return &it->second.Font;
		}
		if (HasFailed(font))
		{
			return nullptr;
		}
		Result<Scope<FontAtlas>> atlas = FontAtlas::Create(font);
		if (!atlas)
		{
			ReportFailure(font, fmt::format("Font {} cannot be used: {}", font->Handle, atlas.GetError()));
			return nullptr;
		}
		FontEntry& entry = data.Fonts[font.get()];
		entry.Source = font;
		entry.Font = GpuFont{atlas.TakeValue(), nullptr, 0};
		return &entry.Font;
	}

	nvrhi::ITexture* Renderer::UpdateFontTexture(GpuFont& font)
	{
		FontAtlas const& atlas = *font.Atlas;
		if (font.Texture && font.UploadedVersion == atlas.GetVersion())
		{
			return font.Texture;
		}
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		if (!font.Texture || font.Texture->getDesc().width != atlas.GetWidth() || font.Texture->getDesc().height != atlas.GetHeight())
		{
			// A grown atlas gets a new texture; frames in flight keep the old one alive.
			nvrhi::TextureDesc desc;
			desc.width = atlas.GetWidth();
			desc.height = atlas.GetHeight();
			desc.format = nvrhi::Format::R8_UNORM;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "Font atlas";
			font.Texture = device->createTexture(desc);
			if (!font.Texture)
			{
				ST_CORE_ERROR("Failed to create a font atlas texture ({}x{})", atlas.GetWidth(), atlas.GetHeight());
				return nullptr;
			}
		}
		nvrhi::ICommandList* commandList = GetData().UploadCommandList;
		commandList->open();
		commandList->writeTexture(font.Texture, 0, 0, atlas.GetPixels().data(), atlas.GetWidth());
		commandList->close();
		device->executeCommandList(commandList);
		font.UploadedVersion = atlas.GetVersion();
		return font.Texture;
	}

	void Renderer::CollectGarbage()
	{
		RendererData& data = GetData();
		std::erase_if(data.Meshes,
		              [](auto const& entry)
		              {
						  return entry.second.Source.expired();
					  });
		std::erase_if(data.Textures,
		              [](auto const& entry)
		              {
						  return entry.second.Source.expired();
					  });
		std::erase_if(data.Materials,
		              [](auto const& entry)
		              {
						  return entry.second.Source.expired();
					  });
		std::erase_if(data.EnvironmentMaps,
		              [](auto const& entry)
		              {
						  return entry.second.Source.expired();
					  });
		std::erase_if(data.Fonts,
		              [](auto const& entry)
		              {
						  return entry.second.Source.expired();
					  });
		std::erase_if(data.Failures,
		              [](auto const& entry)
		              {
						  return entry.second.expired();
					  });
	}
}
