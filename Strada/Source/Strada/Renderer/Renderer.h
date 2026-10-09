#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Renderer/FontAtlas.h"

#include <nvrhi/nvrhi.h>

namespace Strada
{
	class EnvironmentAsset;
	class FontAsset;
	class MaterialAsset;
	class MeshSource;
	struct GpuEnvironment;

	struct GpuMesh
	{
		nvrhi::BufferHandle VertexBuffer;
		nvrhi::BufferHandle IndexBuffer;
	};

	// A font's glyph atlas with its GPU copy.
	struct GpuFont
	{
		Scope<FontAtlas> Atlas;
		// R8_UNORM copy of the atlas image; null before the first upload.
		nvrhi::TextureHandle Texture;
		// The atlas version the texture holds.
		uint64_t UploadedVersion = 0;
	};

	// Shared GPU state of the scene renderers: samplers, layouts and caches of GPU resources created from assets. Cache
	// entries are keyed by asset object identity, so reloaded assets get new GPU resources, and materials refresh when
	// their version changes. Requires the GraphicsDevice and ShaderLibrary. Main thread only.
	class Renderer
	{
	public:
		[[nodiscard]] static Result<void> Init();
		// Releases every cached GPU resource; call before the GraphicsDevice shuts down.
		static void Shutdown();
		static bool IsInitialized();

		// Trilinear, 16x anisotropic, repeat: material textures.
		static nvrhi::ISampler* GetMaterialSampler();
		static nvrhi::ISampler* GetLinearClampSampler();

		// Layout of material binding sets (set 1): constants b0, textures t0-t4.
		static nvrhi::IBindingLayout* GetMaterialBindingLayout();

		// Null (logged once per asset) when the mesh cannot be uploaded.
		static GpuMesh const* GetMesh(Ref<MeshSource> const& mesh);
		// The texture of an asset with a full mip chain in the requested color space; a built-in fallback when the handle
		// is invalid or the texture cannot be loaded.
		static nvrhi::ITexture* GetTexture(AssetHandle handle, bool srgb, AssetHandle fallback);
		// Binding set for a material (constants and textures); rebuilt when the material or its textures change.
		static nvrhi::IBindingSet* GetMaterialBindingSet(Ref<MaterialAsset> const& material);

		// The glyph atlas of a font, created on first use; null when the font cannot be read (logged once). Valid until
		// CollectGarbage.
		static GpuFont* GetFont(Ref<FontAsset> const& font);
		// Uploads the atlas image when glyphs were added since the last upload (through its own command list, so call it
		// before recording a frame). Returns the texture, or null when it cannot be created.
		static nvrhi::ITexture* UpdateFontTexture(GpuFont& font);

		// Image-based lighting data of an environment (computed on first use); null when it cannot be processed.
		static GpuEnvironment const* GetEnvironment(Ref<EnvironmentAsset> const& environment);
		// Split-sum DFG lookup table (RG = scale, bias).
		static nvrhi::ITexture* GetBrdfLut();
		// 1x1 black cubemap bound when there is no environment.
		static nvrhi::ITexture* GetFallbackCube();

		// Drops cache entries whose assets were destroyed. Called once per frame by scene renderers.
		static void CollectGarbage();
	};
}
