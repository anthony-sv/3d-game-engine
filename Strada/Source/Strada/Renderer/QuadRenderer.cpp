#include "stpch.h"
#include "Strada/Renderer/QuadRenderer.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Core/Hash.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"
#include "Strada/Renderer/FontAtlas.h"
#include "Strada/Renderer/Renderer.h"

#include "RendererInterop.h"

#include <algorithm>
#include <bit>
#include <cstddef>

namespace Strada
{
	static_assert(sizeof(ShaderInterop::QuadConstants) == 96);

	namespace
	{
		// Binding sets unused for this many frames are released.
		constexpr uint64_t BindingSetRetentionFrames = 4;
		constexpr uint32_t MinimumQuadCapacity = 1024;

		// Maps local quad coordinates to vertex positions: world positions, or clip-space positions of screen quads.
		class QuadPlacement
		{
		public:
			QuadPlacement(glm::mat4 const& transform, bool screenSpace, glm::uvec2 const& viewportSize)
				: m_Transform(transform),
				  m_ScreenSpace(screenSpace),
				  m_ViewportSize(glm::max(glm::vec2(viewportSize), glm::vec2(1.0f)))
			{
			}

			glm::vec4 Place(glm::vec2 const& local) const
			{
				if (!m_ScreenSpace)
				{
					return m_Transform * glm::vec4(local, 0.0f, 1.0f);
				}
				// The transform's X and Y axes, projected onto the screen (whose y points down), carry the scale in pixels and
				// the rotation around Z.
				glm::vec2 const origin = glm::vec2(m_Transform[3]) * m_ViewportSize;
				glm::vec2 const axisX(m_Transform[0].x, -m_Transform[0].y);
				glm::vec2 const axisY(m_Transform[1].x, -m_Transform[1].y);
				glm::vec2 const pixel = origin + axisX * local.x + axisY * local.y;
				return glm::vec4(pixel.x / m_ViewportSize.x * 2.0f - 1.0f, 1.0f - pixel.y / m_ViewportSize.y * 2.0f, 0.0f, 1.0f);
			}

		private:
			glm::mat4 m_Transform;
			bool m_ScreenSpace;
			glm::vec2 m_ViewportSize;
		};

		void EnableAlphaBlending(nvrhi::GraphicsPipelineDesc& desc)
		{
			nvrhi::BlendState::RenderTarget& target = desc.renderState.blendState.targets[0];
			target.blendEnable = true;
			target.srcBlend = nvrhi::BlendFactor::SrcAlpha;
			target.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
			target.srcBlendAlpha = nvrhi::BlendFactor::Zero;
			target.destBlendAlpha = nvrhi::BlendFactor::One;
		}
	}

	size_t QuadRenderer::BindingKeyHash::operator()(std::pair<nvrhi::ITexture*, nvrhi::ISampler*> const& key) const
	{
		uint64_t const texture = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key.first));
		uint64_t const sampler = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key.second));
		return static_cast<size_t>(Hash::Combine(texture, sampler));
	}

	QuadRenderer::QuadRenderer()
	{
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::VertexAttributeDesc const attributes[] = {
			nvrhi::VertexAttributeDesc()
				.setName("POSITION")
				.setFormat(nvrhi::Format::RGBA32_FLOAT)
				.setOffset(offsetof(Vertex, Position))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("COLOR")
				.setFormat(nvrhi::Format::RGBA32_FLOAT)
				.setOffset(offsetof(Vertex, Color))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("TEXCOORD")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(offsetof(Vertex, TexCoord))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("ENTITYID")
				.setFormat(nvrhi::Format::R32_UINT)
				.setOffset(offsetof(Vertex, EntityId))
				.setElementStride(sizeof(Vertex)),
		};
		m_InputLayout = device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), ShaderLibrary::Get("Quad_VS"));

		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::All;
		layout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShaderInterop::QuadConstants)),
		};
		m_BindingLayout = device->createBindingLayout(layout);
		if (!m_InputLayout || !m_BindingLayout)
		{
			ST_CORE_ERROR("Failed to create the sprite and text layouts");
		}
	}

	QuadRenderer::~QuadRenderer() = default;

	void QuadRenderer::SubmitSprite(SpriteSubmission const& sprite, uint32_t entityId)
	{
		m_Sprites.emplace_back(sprite, entityId);
	}

	void QuadRenderer::SubmitText(TextSubmission text, uint32_t entityId)
	{
		if (!text.Text.empty())
		{
			m_Texts.emplace_back(std::move(text), entityId);
		}
	}

	void QuadRenderer::Prepare(glm::vec3 const& cameraPosition, glm::uvec2 const& viewportSize)
	{
		m_Frame++;
		std::erase_if(m_BindingSets,
		              [this](auto const& entry)
		              {
						  return m_Frame - entry.second.LastUsedFrame > BindingSetRetentionFrames;
					  });
		m_Vertices.clear();
		m_WorldBatches.clear();
		m_ScreenBatches.clear();
		m_QuadCount = 0;

		// Layout adds glyphs to the font atlases, so every text is laid out before any atlas is uploaded.
		m_Layouts.assign(m_Texts.size(), TextLayout());
		m_TextFonts.assign(m_Texts.size(), nullptr);
		Ref<FontAsset> defaultFont;
		for (size_t i = 0; i < m_Texts.size(); i++)
		{
			TextSubmission& text = m_Texts[i].first;
			if (!text.Font)
			{
				if (!defaultFont && AssetManager::IsInitialized())
				{
					defaultFont = AssetManager::GetAsset<FontAsset>(GetBuiltInHandle(BuiltInAsset::DefaultFont));
				}
				text.Font = defaultFont;
			}
			if (GpuFont* font = Renderer::GetFont(text.Font))
			{
				m_Layouts[i] = LayoutText(*font->Atlas, text.Text, text.Layout);
				m_TextFonts[i] = font;
			}
		}

		// World items back to front; screen items by layer (translation z), the higher on top.
		std::vector<Item> world;
		std::vector<Item> screen;
		auto const addItem = [&](uint32_t index, bool text, glm::mat4 const& transform, bool screenSpace)
		{
			glm::vec3 const position(transform[3]);
			if (screenSpace)
			{
				screen.push_back({index, text, position.z});
			}
			else
			{
				world.push_back({index, text, glm::dot(position - cameraPosition, position - cameraPosition)});
			}
		};
		for (size_t i = 0; i < m_Sprites.size(); i++)
		{
			SpriteSubmission const& sprite = m_Sprites[i].first;
			addItem(static_cast<uint32_t>(i), false, sprite.Transform, sprite.ScreenSpace);
		}
		for (size_t i = 0; i < m_Texts.size(); i++)
		{
			if (m_TextFonts[i] != nullptr && !m_Layouts[i].Glyphs.empty())
			{
				TextSubmission const& text = m_Texts[i].first;
				addItem(static_cast<uint32_t>(i), true, text.Transform, text.ScreenSpace);
			}
		}
		std::stable_sort(world.begin(), world.end(),
		                 [](Item const& a, Item const& b)
		                 {
							 return a.SortKey > b.SortKey;
						 });
		std::stable_sort(screen.begin(), screen.end(),
		                 [](Item const& a, Item const& b)
		                 {
							 return a.SortKey < b.SortKey;
						 });
		for (Item const& item : world)
		{
			BuildItem(item, m_WorldBatches, viewportSize);
		}
		for (Item const& item : screen)
		{
			BuildItem(item, m_ScreenBatches, viewportSize);
		}
		m_QuadCount = static_cast<uint32_t>(m_Vertices.size() / 4);
	}

	void QuadRenderer::BuildItem(Item const& item, std::vector<Batch>& batches, glm::uvec2 const& viewportSize)
	{
		if (!item.Text)
		{
			auto const& [sprite, entityId] = m_Sprites[item.Index];
			AssetHandle const white = GetBuiltInHandle(BuiltInAsset::WhiteTexture);
			nvrhi::ITexture* texture = Renderer::GetTexture(sprite.Texture.IsValid() ? sprite.Texture : white, true, white);
			if (texture == nullptr)
			{
				return;
			}
			QuadPlacement const placement(sprite.Transform, sprite.ScreenSpace, viewportSize);
			glm::vec4 const corners[4] = {
				placement.Place(glm::vec2(-0.5f, -0.5f)),
				placement.Place(glm::vec2(0.5f, -0.5f)),
				placement.Place(glm::vec2(0.5f, 0.5f)),
				placement.Place(glm::vec2(-0.5f, 0.5f)),
			};
			AddQuad(batches, corners, glm::vec2(0.0f), glm::vec2(sprite.Tiling), sprite.Color, entityId, texture,
			        Renderer::GetMaterialSampler(), ShaderInterop::QuadModeSprite, glm::vec2(1.0f));
			return;
		}

		auto const& [text, entityId] = m_Texts[item.Index];
		GpuFont* font = m_TextFonts[item.Index];
		nvrhi::ITexture* texture = Renderer::UpdateFontTexture(*font);
		if (texture == nullptr)
		{
			return;
		}
		glm::vec2 const atlasSize(static_cast<float>(font->Atlas->GetWidth()), static_cast<float>(font->Atlas->GetHeight()));
		QuadPlacement const placement(text.Transform, text.ScreenSpace, viewportSize);
		float const size = text.FontSize;
		for (TextGlyphQuad const& glyph : m_Layouts[item.Index].Glyphs)
		{
			// Layout coordinates have y down; the local plane has y up.
			glm::vec4 const corners[4] = {
				placement.Place(glm::vec2(glyph.Min.x, -glyph.Max.y) * size),
				placement.Place(glm::vec2(glyph.Max.x, -glyph.Max.y) * size),
				placement.Place(glm::vec2(glyph.Max.x, -glyph.Min.y) * size),
				placement.Place(glm::vec2(glyph.Min.x, -glyph.Min.y) * size),
			};
			AddQuad(batches, corners, glm::vec2(glyph.AtlasMin) / atlasSize, glm::vec2(glyph.AtlasMax) / atlasSize, text.Color, entityId,
			        texture, Renderer::GetLinearClampSampler(), ShaderInterop::QuadModeText, atlasSize);
		}
	}

	void QuadRenderer::AddQuad(std::vector<Batch>& batches, glm::vec4 const (&corners)[4], glm::vec2 const& uvMin, glm::vec2 const& uvMax,
	                           glm::vec4 const& color, uint32_t entityId, nvrhi::ITexture* texture, nvrhi::ISampler* sampler, uint32_t mode,
	                           glm::vec2 const& atlasSize)
	{
		uint32_t const quad = static_cast<uint32_t>(m_Vertices.size() / 4);
		// Texture rows go down while the corners go counterclockwise from the bottom-left.
		glm::vec2 const texCoords[4] = {
			glm::vec2(uvMin.x, uvMax.y),
			glm::vec2(uvMax.x, uvMax.y),
			glm::vec2(uvMax.x, uvMin.y),
			glm::vec2(uvMin.x, uvMin.y),
		};
		for (uint32_t i = 0; i < 4; i++)
		{
			m_Vertices.push_back({corners[i], color, texCoords[i], entityId, 0});
		}
		if (!batches.empty())
		{
			Batch& last = batches.back();
			if (last.Texture == texture && last.Sampler == sampler && last.Mode == mode && last.FirstQuad + last.QuadCount == quad)
			{
				last.QuadCount++;
				return;
			}
		}
		batches.push_back({quad, 1, texture, sampler, mode, atlasSize});
	}

	void QuadRenderer::Upload(nvrhi::ICommandList* commandList)
	{
		m_Uploaded = false;
		if (m_Vertices.empty() || !m_InputLayout || !m_BindingLayout)
		{
			return;
		}
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		uint64_t const vertexBytes = m_Vertices.size() * sizeof(Vertex);
		if (!m_VertexBuffer || m_VertexBuffer->getDesc().byteSize < vertexBytes)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = std::bit_ceil(std::max<uint64_t>(vertexBytes, MinimumQuadCapacity * 4 * sizeof(Vertex)));
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "Sprite and text vertices";
			m_VertexBuffer = device->createBuffer(desc);
		}
		if (!m_IndexBuffer || m_IndexBufferQuads < m_QuadCount)
		{
			// Every quad uses the same six indices; draws offset them with their base vertex.
			uint32_t const capacity = std::bit_ceil(std::max(m_QuadCount, MinimumQuadCapacity));
			std::vector<uint32_t> indices;
			indices.reserve(static_cast<size_t>(capacity) * 6);
			for (uint32_t quad = 0; quad < capacity; quad++)
			{
				uint32_t const first = quad * 4;
				indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
			}
			nvrhi::BufferDesc desc;
			desc.byteSize = indices.size() * sizeof(uint32_t);
			desc.isIndexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "Sprite and text indices";
			m_IndexBuffer = device->createBuffer(desc);
			m_IndexBufferQuads = m_IndexBuffer ? capacity : 0;
			if (m_IndexBuffer)
			{
				commandList->writeBuffer(m_IndexBuffer, indices.data(), indices.size() * sizeof(uint32_t));
			}
		}
		if (!m_VertexBuffer || !m_IndexBuffer)
		{
			ST_CORE_ERROR("Failed to create the sprite and text buffers");
			return;
		}
		commandList->writeBuffer(m_VertexBuffer, m_Vertices.data(), vertexBytes);
		m_Uploaded = true;
	}

	void QuadRenderer::RenderWorld(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, glm::mat4 const& viewProjection)
	{
		if (!m_WorldBatches.empty())
		{
			DrawBatches(commandList, m_WorldBatches, GetPipeline(m_WorldPipeline, framebuffer, false, true), framebuffer, viewProjection,
			            false);
		}
	}

	void QuadRenderer::RenderScreen(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer)
	{
		if (!m_ScreenBatches.empty())
		{
			DrawBatches(commandList, m_ScreenBatches, GetPipeline(m_ScreenPipeline, framebuffer, false, false), framebuffer,
			            glm::mat4(1.0f), true);
		}
	}

	void QuadRenderer::RenderEntityIds(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, glm::mat4 const& viewProjection)
	{
		if (!m_WorldBatches.empty())
		{
			DrawBatches(commandList, m_WorldBatches, GetPipeline(m_WorldEntityIdPipeline, framebuffer, true, true), framebuffer,
			            viewProjection, false);
		}
		// Screen-space items are on top of everything.
		if (!m_ScreenBatches.empty())
		{
			DrawBatches(commandList, m_ScreenBatches, GetPipeline(m_ScreenEntityIdPipeline, framebuffer, true, false), framebuffer,
			            glm::mat4(1.0f), false);
		}
	}

	void QuadRenderer::Clear()
	{
		m_Sprites.clear();
		m_Texts.clear();
		m_Layouts.clear();
		m_TextFonts.clear();
		m_Vertices.clear();
		m_WorldBatches.clear();
		m_ScreenBatches.clear();
	}

	nvrhi::IBindingSet* QuadRenderer::GetBindingSet(nvrhi::ITexture* texture, nvrhi::ISampler* sampler)
	{
		std::pair<nvrhi::ITexture*, nvrhi::ISampler*> const key(texture, sampler);
		if (auto const it = m_BindingSets.find(key); it != m_BindingSets.end())
		{
			it->second.LastUsedFrame = m_Frame;
			return it->second.BindingSet;
		}
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, texture),
			nvrhi::BindingSetItem::Sampler(0, sampler),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(ShaderInterop::QuadConstants)),
		};
		nvrhi::BindingSetHandle bindingSet = GraphicsDevice::GetDevice()->createBindingSet(desc, m_BindingLayout);
		if (!bindingSet)
		{
			ST_CORE_ERROR("Failed to create a sprite or text binding set");
			return nullptr;
		}
		m_BindingSets[key] = CachedBindingSet{texture, bindingSet, m_Frame};
		return bindingSet;
	}

	nvrhi::IGraphicsPipeline* QuadRenderer::GetPipeline(nvrhi::GraphicsPipelineHandle& pipeline, nvrhi::IFramebuffer* framebuffer,
	                                                    bool entityIds, bool depthTest)
	{
		if (pipeline)
		{
			return pipeline;
		}
		nvrhi::GraphicsPipelineDesc desc;
		desc.inputLayout = m_InputLayout;
		desc.VS = ShaderLibrary::Get("Quad_VS");
		desc.PS = ShaderLibrary::Get(entityIds ? "QuadEntityId_PS" : "Quad_PS");
		desc.bindingLayouts = {m_BindingLayout};
		desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
		// Reversed Z; quads never write depth (they are transparent, sorted back to front).
		desc.renderState.depthStencilState.depthTestEnable = depthTest;
		desc.renderState.depthStencilState.depthWriteEnable = false;
		desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;
		if (!entityIds)
		{
			EnableAlphaBlending(desc);
		}
		pipeline = GraphicsDevice::GetDevice()->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo());
		if (!pipeline)
		{
			ST_CORE_ERROR("Failed to create a sprite and text pipeline");
		}
		return pipeline;
	}

	void QuadRenderer::DrawBatches(nvrhi::ICommandList* commandList, std::vector<Batch> const& batches, nvrhi::IGraphicsPipeline* pipeline,
	                               nvrhi::IFramebuffer* framebuffer, glm::mat4 const& transform, bool encodeSrgb)
	{
		if (pipeline == nullptr || !m_Uploaded)
		{
			return;
		}
		nvrhi::FramebufferInfoEx const& info = framebuffer->getFramebufferInfo();
		nvrhi::Viewport const viewport(static_cast<float>(info.width), static_cast<float>(info.height));
		for (Batch const& batch : batches)
		{
			nvrhi::IBindingSet* bindings = GetBindingSet(batch.Texture, batch.Sampler);
			if (bindings == nullptr)
			{
				continue;
			}
			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = {bindings};
			state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(m_VertexBuffer).setSlot(0).setOffset(0)};
			state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(m_IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
			commandList->setGraphicsState(state);

			ShaderInterop::QuadConstants constants{};
			constants.Transform = transform;
			constants.Mode = batch.Mode;
			constants.EncodeSrgb = encodeSrgb ? 1u : 0u;
			constants.DistanceScale = FontAtlas::GetDistanceScale();
			constants.EdgeSample = FontAtlas::GetEdgeSample();
			constants.AtlasSize = batch.AtlasSize;
			commandList->setPushConstants(&constants, sizeof(constants));

			nvrhi::DrawArguments arguments;
			arguments.vertexCount = batch.QuadCount * 6;
			arguments.startVertexLocation = batch.FirstQuad * 4;
			commandList->drawIndexed(arguments);
		}
	}
}
