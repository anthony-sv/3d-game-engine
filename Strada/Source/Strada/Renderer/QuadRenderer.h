#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Renderer/TextLayout.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strada
{
	class FontAsset;
	struct GpuFont;

	// A textured or colored rectangle. World-space sprites are the unit quad (x and y in [-0.5, 0.5], facing +Z)
	// transformed by Transform. Screen-space sprites are drawn over the final image: the translation's x and y are
	// viewport coordinates in [0, 1] from the top-left, the scale is in pixels, only the rotation around Z applies, and the
	// translation's z orders screen-space items (higher values on top).
	struct SpriteSubmission
	{
		glm::mat4 Transform = glm::mat4(1.0f);
		// Linear color with straight alpha, multiplied with the texture.
		glm::vec4 Color = glm::vec4(1.0f);
		// Sampled as sRGB color; none draws the solid color.
		AssetHandle Texture;
		// Texture repetitions across the quad.
		float Tiling = 1.0f;
		bool ScreenSpace = false;
		// Picking ID (0 = not pickable); selected items get the selection outline.
		uint32_t PickingId = 0;
		bool Selected = false;
	};

	// Text drawn with a font's distance-field glyphs. The origin is the top of the first line, where lines start, are
	// centered or end depending on the alignment; lines run along +X and stack along -Y (world space) or downwards on the
	// screen. Screen-space text is placed like screen-space sprites.
	struct TextSubmission
	{
		std::string Text;
		// Null uses the default font.
		Ref<FontAsset> Font;
		glm::mat4 Transform = glm::mat4(1.0f);
		// Linear color with straight alpha.
		glm::vec4 Color = glm::vec4(1.0f);
		// Line height in world units, or in pixels on the screen (scaled by the transform in both cases).
		float FontSize = 1.0f;
		TextLayoutSettings Layout;
		bool ScreenSpace = false;
		uint32_t PickingId = 0;
		bool Selected = false;
	};

	// Batches the sprites and text of one SceneRenderer frame. World-space quads are drawn into the HDR image (unlit:
	// their color is used as is, alpha blended, tested against the scene depth without writing it, back to front);
	// screen-space quads onto the final image; both into the entity ID target for picking. Requires Renderer::Init. Main
	// thread only.
	class QuadRenderer
	{
	public:
		QuadRenderer();
		~QuadRenderer();

		QuadRenderer(QuadRenderer const&) = delete;
		QuadRenderer& operator=(QuadRenderer const&) = delete;

		// entityId: the picking ID with ShaderInterop::EntityIdSelectedBit for selected items, 0 when not pickable.
		void SubmitSprite(SpriteSubmission const& sprite, uint32_t entityId);
		void SubmitText(TextSubmission text, uint32_t entityId);

		// Lays out the text, uploads textures and changed font atlases and builds the vertices of the frame. Uploads
		// submit their own command lists, so this runs before the frame is recorded.
		void Prepare(glm::vec3 const& cameraPosition, glm::uvec2 const& viewportSize);
		// Records the vertex upload; call once per frame before the Render functions.
		void Upload(nvrhi::ICommandList* commandList);
		void RenderWorld(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, glm::mat4 const& viewProjection);
		void RenderScreen(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer);
		void RenderEntityIds(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, glm::mat4 const& viewProjection);
		// Forgets the frame's submissions (they keep their assets alive until then).
		void Clear();

		// Quads and draw calls of the prepared frame (per pass that draws them).
		uint32_t GetQuadCount() const { return m_QuadCount; }
		uint32_t GetBatchCount() const { return static_cast<uint32_t>(m_WorldBatches.size() + m_ScreenBatches.size()); }

	private:
		struct Vertex
		{
			glm::vec4 Position;
			glm::vec4 Color;
			glm::vec2 TexCoord;
			uint32_t EntityId = 0;
			uint32_t Padding = 0;
		};

		// Consecutive quads drawn with the same texture and mode.
		struct Batch
		{
			uint32_t FirstQuad = 0;
			uint32_t QuadCount = 0;
			nvrhi::ITexture* Texture = nullptr;
			nvrhi::ISampler* Sampler = nullptr;
			uint32_t Mode = 0;
			glm::vec2 AtlasSize = glm::vec2(1.0f);
		};

		struct Item
		{
			// Index into m_Sprites or m_Texts.
			uint32_t Index = 0;
			bool Text = false;
			// Back-to-front order of world items, layer order of screen items.
			float SortKey = 0.0f;
		};

		struct CachedBindingSet
		{
			// Holding the texture keeps the pointer key valid until the entry is evicted.
			nvrhi::TextureHandle Texture;
			nvrhi::BindingSetHandle BindingSet;
			uint64_t LastUsedFrame = 0;
		};

		struct BindingKeyHash
		{
			size_t operator()(std::pair<nvrhi::ITexture*, nvrhi::ISampler*> const& key) const;
		};

		void AddQuad(std::vector<Batch>& batches, glm::vec4 const (&corners)[4], glm::vec2 const& uvMin, glm::vec2 const& uvMax,
		             glm::vec4 const& color, uint32_t entityId, nvrhi::ITexture* texture, nvrhi::ISampler* sampler, uint32_t mode,
		             glm::vec2 const& atlasSize);
		void BuildItem(Item const& item, std::vector<Batch>& batches, glm::uvec2 const& viewportSize);
		nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture, nvrhi::ISampler* sampler);
		nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::GraphicsPipelineHandle& pipeline, nvrhi::IFramebuffer* framebuffer, bool entityIds,
		                                      bool depthTest);
		void DrawBatches(nvrhi::ICommandList* commandList, std::vector<Batch> const& batches, nvrhi::IGraphicsPipeline* pipeline,
		                 nvrhi::IFramebuffer* framebuffer, glm::mat4 const& transform, bool encodeSrgb);

		std::vector<std::pair<SpriteSubmission, uint32_t>> m_Sprites;
		std::vector<std::pair<TextSubmission, uint32_t>> m_Texts;
		// Per text: its layout and font (null when the font cannot be used), valid until Clear.
		std::vector<TextLayout> m_Layouts;
		std::vector<GpuFont*> m_TextFonts;

		std::vector<Vertex> m_Vertices;
		std::vector<Batch> m_WorldBatches;
		std::vector<Batch> m_ScreenBatches;
		uint32_t m_QuadCount = 0;
		uint64_t m_Frame = 0;
		// The vertices of the prepared frame are in the vertex buffer.
		bool m_Uploaded = false;

		nvrhi::BufferHandle m_VertexBuffer;
		nvrhi::BufferHandle m_IndexBuffer;
		uint32_t m_IndexBufferQuads = 0;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		std::unordered_map<std::pair<nvrhi::ITexture*, nvrhi::ISampler*>, CachedBindingSet, BindingKeyHash> m_BindingSets;
		nvrhi::GraphicsPipelineHandle m_WorldPipeline;
		nvrhi::GraphicsPipelineHandle m_ScreenPipeline;
		nvrhi::GraphicsPipelineHandle m_WorldEntityIdPipeline;
		nvrhi::GraphicsPipelineHandle m_ScreenEntityIdPipeline;
	};
}
