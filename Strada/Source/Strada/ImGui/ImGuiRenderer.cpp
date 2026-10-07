#include "stpch.h"
#include "Strada/ImGui/ImGuiRenderer.h"

#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/ShaderLibrary.h"
#include "Strada/RHI/Swapchain.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstddef>
#include <cstring>
#include <vector>

namespace Strada
{
	namespace
	{
		// Binding sets for textures not drawn for this many frames are released.
		constexpr uint64_t BindingSetRetentionFrames = 4;

		struct ImGuiPushConstants
		{
			float Scale[2];
			float Translate[2];
		};

		nvrhi::Format const IndexFormat = sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT;

		// Registered as PlatformIO::DrawCallback_ResetRenderState. Render state is re-applied before every draw, so
		// recognizing the callback is all that is needed.
		void ResetRenderStateCallback(ImDrawList const* drawList, ImDrawCmd const* command)
		{
			(void)drawList;
			(void)command;
		}

		glm::uvec2 GetFramebufferSize(ImGuiViewport* viewport)
		{
			int width = 0;
			int height = 0;
			glfwGetFramebufferSize(static_cast<GLFWwindow*>(viewport->PlatformHandle), &width, &height);
			return {static_cast<uint32_t>(std::max(0, width)), static_cast<uint32_t>(std::max(0, height))};
		}
	}

	ImGuiRenderer::ImGuiRenderer() = default;

	ImGuiRenderer::~ImGuiRenderer()
	{
		ST_CORE_ASSERT(!m_Initialized, "ImGuiRenderer::Shutdown must be called before destruction");
	}

	Result<void> ImGuiRenderer::Init()
	{
		ST_CORE_ASSERT(!m_Initialized, "ImGuiRenderer is already initialized");
		ST_CORE_ASSERT(ImGui::GetCurrentContext() != nullptr, "ImGuiRenderer requires an ImGui context");

		ImGuiIO& io = ImGui::GetIO();
		ST_CORE_ASSERT(io.BackendRendererUserData == nullptr, "An ImGui renderer backend is already installed");

		nvrhi::IDevice* device = GraphicsDevice::GetDevice();

		m_VertexShader = ShaderLibrary::Get("ImGui_VS");
		m_PixelShader = ShaderLibrary::Get("ImGui_PS");
		if (!m_VertexShader || !m_PixelShader)
		{
			return Error{"ImGui shaders are missing from the shader library"};
		}

		nvrhi::VertexAttributeDesc const attributes[] = {
			nvrhi::VertexAttributeDesc()
				.setName("POSITION")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(offsetof(ImDrawVert, pos))
				.setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc()
				.setName("TEXCOORD")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(offsetof(ImDrawVert, uv))
				.setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc()
				.setName("COLOR")
				.setFormat(nvrhi::Format::RGBA8_UNORM)
				.setOffset(offsetof(ImDrawVert, col))
				.setElementStride(sizeof(ImDrawVert)),
		};
		m_InputLayout = device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), m_VertexShader);

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ImGuiPushConstants)),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
		};
		m_BindingLayout = device->createBindingLayout(layoutDesc);

		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_Sampler = device->createSampler(samplerDesc);

		m_UploadCommandList = device->createCommandList();
		if (!m_InputLayout || !m_BindingLayout || !m_Sampler || !m_UploadCommandList)
		{
			return Error{"Failed to create ImGui renderer resources"};
		}

		io.BackendRendererUserData = this;
		io.BackendRendererName = "Strada NVRHI";
		io.BackendFlags |=
			ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports;

		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Renderer_TextureMaxWidth = 16384;
		platformIO.Renderer_TextureMaxHeight = 16384;
		platformIO.Renderer_CreateWindow = OnCreateWindow;
		platformIO.Renderer_DestroyWindow = OnDestroyWindow;
		platformIO.Renderer_SetWindowSize = OnSetWindowSize;
		platformIO.Renderer_RenderWindow = OnRenderWindow;
		platformIO.Renderer_SwapBuffers = OnSwapBuffers;
		platformIO.DrawCallback_ResetRenderState = ResetRenderStateCallback;

		// The main viewport renders into the application's swapchain, so it only needs buffers.
		ImGuiViewport* mainViewport = ImGui::GetMainViewport();
		mainViewport->RendererUserData = &CreateViewportData(mainViewport);

		m_Initialized = true;
		return {};
	}

	void ImGuiRenderer::Shutdown()
	{
		if (!m_Initialized)
		{
			return;
		}

		GraphicsDevice::WaitForIdle();

		// Release textures owned by this backend (font atlas etc.) so ImGui can recreate them if needed.
		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
		{
			if (texture->RefCount == 1)
			{
				DestroyTexture(texture);
			}
		}

		ImGui::DestroyPlatformWindows();
		ImGui::GetMainViewport()->RendererUserData = nullptr;
		m_Viewports.clear();

		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Renderer_CreateWindow = nullptr;
		platformIO.Renderer_DestroyWindow = nullptr;
		platformIO.Renderer_SetWindowSize = nullptr;
		platformIO.Renderer_RenderWindow = nullptr;
		platformIO.Renderer_SwapBuffers = nullptr;
		platformIO.DrawCallback_ResetRenderState = nullptr;

		ImGuiIO& io = ImGui::GetIO();
		io.BackendRendererUserData = nullptr;
		io.BackendRendererName = nullptr;
		io.BackendFlags &=
			~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);

		m_BindingSets.clear();
		m_Textures.clear();
		m_Pipelines.clear();
		m_UploadCommandList = nullptr;
		m_Sampler = nullptr;
		m_BindingLayout = nullptr;
		m_InputLayout = nullptr;
		m_PixelShader = nullptr;
		m_VertexShader = nullptr;
		m_Initialized = false;
	}

	void ImGuiRenderer::UpdateTextures()
	{
		ST_CORE_ASSERT(m_Initialized, "ImGuiRenderer is not initialized");
		m_FrameIndex++;

		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
		{
			switch (texture->Status)
			{
				case ImTextureStatus_WantCreate:
					CreateTexture(texture);
					break;
				case ImTextureStatus_WantUpdates:
					UpdateTexture(texture);
					break;
				case ImTextureStatus_WantDestroy:
					// NVRHI defers the actual release until the GPU no longer uses the texture.
					DestroyTexture(texture);
					break;
				case ImTextureStatus_OK:
				case ImTextureStatus_Destroyed:
					break;
			}
		}

		for (auto it = m_BindingSets.begin(); it != m_BindingSets.end();)
		{
			if (m_FrameIndex - it->second.LastUsedFrame > BindingSetRetentionFrames)
			{
				it = m_BindingSets.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void ImGuiRenderer::CreateTexture(ImTextureData* texture)
	{
		ST_CORE_ASSERT(texture->Format == ImTextureFormat_RGBA32, "Only RGBA32 ImGui textures are supported");
		nvrhi::IDevice* device = GraphicsDevice::GetDevice();

		nvrhi::TextureDesc desc;
		desc.width = static_cast<uint32_t>(texture->Width);
		desc.height = static_cast<uint32_t>(texture->Height);
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "ImGui texture";
		nvrhi::TextureHandle gpuTexture = device->createTexture(desc);
		if (!gpuTexture)
		{
			ST_CORE_ERROR("Failed to create a {}x{} ImGui texture", texture->Width, texture->Height);
			return;
		}

		m_UploadCommandList->open();
		m_UploadCommandList->writeTexture(gpuTexture, 0, 0, texture->GetPixels(), static_cast<size_t>(texture->GetPitch()));
		m_UploadCommandList->close();
		device->executeCommandList(m_UploadCommandList);

		m_Textures[texture] = gpuTexture;
		texture->SetTexID(GetTextureID(gpuTexture));
		texture->SetStatus(ImTextureStatus_OK);
	}

	void ImGuiRenderer::UpdateTexture(ImTextureData* texture)
	{
		auto const it = m_Textures.find(texture);
		if (it == m_Textures.end())
		{
			texture->SetStatus(ImTextureStatus_Destroyed);
			return;
		}

		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		m_UploadCommandList->open();
		for (ImTextureRect const& rect : texture->Updates)
		{
			if (rect.w == 0 || rect.h == 0)
			{
				continue;
			}

			// Only the changed region is uploaded, through a staging texture of that size.
			nvrhi::TextureDesc stagingDesc;
			stagingDesc.width = rect.w;
			stagingDesc.height = rect.h;
			stagingDesc.format = nvrhi::Format::RGBA8_UNORM;
			stagingDesc.debugName = "ImGui texture update";
			nvrhi::StagingTextureHandle staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Write);
			if (!staging)
			{
				ST_CORE_ERROR("Failed to create an ImGui texture staging buffer");
				continue;
			}

			size_t rowPitch = 0;
			auto* destination =
				static_cast<uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Write, &rowPitch));
			if (destination == nullptr)
			{
				continue;
			}
			size_t const rowBytes = static_cast<size_t>(rect.w) * static_cast<size_t>(texture->BytesPerPixel);
			for (uint32_t row = 0; row < rect.h; row++)
			{
				std::memcpy(destination + row * rowPitch, texture->GetPixelsAt(rect.x, rect.y + static_cast<int>(row)), rowBytes);
			}
			device->unmapStagingTexture(staging);

			nvrhi::TextureSlice destinationSlice;
			destinationSlice.x = rect.x;
			destinationSlice.y = rect.y;
			destinationSlice.width = rect.w;
			destinationSlice.height = rect.h;
			nvrhi::TextureSlice sourceSlice;
			sourceSlice.width = rect.w;
			sourceSlice.height = rect.h;
			m_UploadCommandList->copyTexture(it->second, destinationSlice, staging, sourceSlice);
		}
		m_UploadCommandList->close();
		device->executeCommandList(m_UploadCommandList);
		texture->SetStatus(ImTextureStatus_OK);
	}

	void ImGuiRenderer::DestroyTexture(ImTextureData* texture)
	{
		if (auto const it = m_Textures.find(texture); it != m_Textures.end())
		{
			m_BindingSets.erase(it->second.Get());
			m_Textures.erase(it);
		}
		texture->SetTexID(ImTextureID_Invalid);
		texture->SetStatus(ImTextureStatus_Destroyed);
	}

	ImGuiRenderer::ViewportData& ImGuiRenderer::CreateViewportData(ImGuiViewport* viewport)
	{
		auto data = CreateScope<ViewportData>();
		data->CommandList = GraphicsDevice::GetDevice()->createCommandList();
		ViewportData& reference = *data;
		m_Viewports[viewport] = std::move(data);
		return reference;
	}

	nvrhi::IGraphicsPipeline* ImGuiRenderer::GetPipeline(nvrhi::IFramebuffer* framebuffer)
	{
		nvrhi::FramebufferInfoEx const& info = framebuffer->getFramebufferInfo();
		nvrhi::Format const format = info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0];
		if (auto const it = m_Pipelines.find(format); it != m_Pipelines.end())
		{
			return it->second;
		}

		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.inputLayout = m_InputLayout;
		desc.VS = m_VertexShader;
		desc.PS = m_PixelShader;
		desc.bindingLayouts = {m_BindingLayout};

		nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
		blend.blendEnable = true;
		blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
		blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		blend.blendOp = nvrhi::BlendOp::Add;
		blend.srcBlendAlpha = nvrhi::BlendFactor::One;
		blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
		blend.blendOpAlpha = nvrhi::BlendOp::Add;

		desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
		desc.renderState.rasterState.scissorEnable = true;
		desc.renderState.depthStencilState.depthTestEnable = false;
		desc.renderState.depthStencilState.depthWriteEnable = false;

		nvrhi::GraphicsPipelineHandle pipeline = GraphicsDevice::GetDevice()->createGraphicsPipeline(desc, info);
		if (!pipeline)
		{
			ST_CORE_ERROR("Failed to create the ImGui pipeline");
			return nullptr;
		}
		m_Pipelines[format] = pipeline;
		return pipeline;
	}

	nvrhi::IBindingSet* ImGuiRenderer::GetBindingSet(nvrhi::ITexture* texture)
	{
		if (auto const it = m_BindingSets.find(texture); it != m_BindingSets.end())
		{
			it->second.LastUsedFrame = m_FrameIndex;
			return it->second.BindingSet;
		}

		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::PushConstants(0, sizeof(ImGuiPushConstants)),
			nvrhi::BindingSetItem::Texture_SRV(0, texture),
			nvrhi::BindingSetItem::Sampler(0, m_Sampler),
		};
		nvrhi::BindingSetHandle bindingSet = GraphicsDevice::GetDevice()->createBindingSet(desc, m_BindingLayout);
		if (!bindingSet)
		{
			return nullptr;
		}

		// Holding the texture handle keeps the pointer key valid until the cache entry is evicted.
		m_BindingSets[texture] = CachedBindingSet{texture, bindingSet, m_FrameIndex};
		return bindingSet;
	}

	void ImGuiRenderer::RenderDrawData(ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer, bool clear)
	{
		ST_CORE_ASSERT(m_Initialized, "ImGuiRenderer is not initialized");
		if (drawData == nullptr || framebuffer == nullptr)
		{
			return;
		}

		float const framebufferWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		float const framebufferHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (framebufferWidth <= 0.0f || framebufferHeight <= 0.0f)
		{
			return;
		}

		auto* viewportData = static_cast<ViewportData*>(drawData->OwnerViewport->RendererUserData);
		ST_CORE_ASSERT(viewportData != nullptr, "ImGui viewport has no renderer data");

		nvrhi::IDevice* device = GraphicsDevice::GetDevice();
		nvrhi::IGraphicsPipeline* pipeline = GetPipeline(framebuffer);
		if (pipeline == nullptr)
		{
			return;
		}

		size_t const vertexCount = static_cast<size_t>(drawData->TotalVtxCount);
		size_t const indexCount = static_cast<size_t>(drawData->TotalIdxCount);
		if (vertexCount > viewportData->VertexCapacity)
		{
			viewportData->VertexCapacity = vertexCount + vertexCount / 2 + 1024;
			nvrhi::BufferDesc desc;
			desc.byteSize = viewportData->VertexCapacity * sizeof(ImDrawVert);
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "ImGui vertices";
			viewportData->VertexBuffer = device->createBuffer(desc);
		}
		if (indexCount > viewportData->IndexCapacity)
		{
			viewportData->IndexCapacity = indexCount + indexCount / 2 + 2048;
			nvrhi::BufferDesc desc;
			desc.byteSize = viewportData->IndexCapacity * sizeof(ImDrawIdx);
			desc.isIndexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "ImGui indices";
			viewportData->IndexBuffer = device->createBuffer(desc);
		}

		nvrhi::ICommandList* commandList = viewportData->CommandList;
		commandList->open();

		if (clear)
		{
			commandList->clearTextureFloat(framebuffer->getDesc().colorAttachments[0].texture, nvrhi::AllSubresources,
			                               nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
		}

		if (vertexCount > 0 && indexCount > 0 && viewportData->VertexBuffer && viewportData->IndexBuffer)
		{
			std::vector<ImDrawVert> vertices;
			std::vector<ImDrawIdx> indices;
			vertices.reserve(vertexCount);
			indices.reserve(indexCount);
			for (ImDrawList const* drawList : drawData->CmdLists)
			{
				vertices.insert(vertices.end(), drawList->VtxBuffer.Data, drawList->VtxBuffer.Data + drawList->VtxBuffer.Size);
				indices.insert(indices.end(), drawList->IdxBuffer.Data, drawList->IdxBuffer.Data + drawList->IdxBuffer.Size);
			}
			commandList->writeBuffer(viewportData->VertexBuffer, vertices.data(), vertices.size() * sizeof(ImDrawVert));
			commandList->writeBuffer(viewportData->IndexBuffer, indices.data(), indices.size() * sizeof(ImDrawIdx));

			// Maps ImGui coordinates (origin top-left of DisplayPos) to clip space (D3D conventions, +Y up).
			ImGuiPushConstants constants;
			constants.Scale[0] = 2.0f / drawData->DisplaySize.x;
			constants.Scale[1] = -2.0f / drawData->DisplaySize.y;
			constants.Translate[0] = -1.0f - drawData->DisplayPos.x * constants.Scale[0];
			constants.Translate[1] = 1.0f - drawData->DisplayPos.y * constants.Scale[1];

			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.viewport.addViewport(nvrhi::Viewport(framebufferWidth, framebufferHeight));
			state.viewport.addScissorRect(nvrhi::Rect(static_cast<int>(framebufferWidth), static_cast<int>(framebufferHeight)));
			state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(viewportData->VertexBuffer).setSlot(0).setOffset(0)};
			state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(viewportData->IndexBuffer).setFormat(IndexFormat).setOffset(0);

			ImDrawCallback const resetRenderState = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
			ImVec2 const clipOffset = drawData->DisplayPos;
			ImVec2 const clipScale = drawData->FramebufferScale;
			uint32_t globalVertexOffset = 0;
			uint32_t globalIndexOffset = 0;

			for (ImDrawList const* drawList : drawData->CmdLists)
			{
				for (ImDrawCmd const& command : drawList->CmdBuffer)
				{
					if (command.UserCallback != nullptr)
					{
						if (command.UserCallback != resetRenderState)
						{
							command.UserCallback(drawList, &command);
						}
						continue;
					}

					float const minX = std::max(0.0f, (command.ClipRect.x - clipOffset.x) * clipScale.x);
					float const minY = std::max(0.0f, (command.ClipRect.y - clipOffset.y) * clipScale.y);
					float const maxX = std::min(framebufferWidth, (command.ClipRect.z - clipOffset.x) * clipScale.x);
					float const maxY = std::min(framebufferHeight, (command.ClipRect.w - clipOffset.y) * clipScale.y);
					if (maxX <= minX || maxY <= minY)
					{
						continue;
					}

					auto* texture = reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(command.GetTexID()));
					if (texture == nullptr)
					{
						continue;
					}
					nvrhi::IBindingSet* bindingSet = GetBindingSet(texture);
					if (bindingSet == nullptr)
					{
						continue;
					}

					state.bindings = {bindingSet};
					state.viewport.scissorRects[0] =
						nvrhi::Rect(static_cast<int>(minX), static_cast<int>(maxX), static_cast<int>(minY), static_cast<int>(maxY));
					commandList->setGraphicsState(state);
					commandList->setPushConstants(&constants, sizeof(constants));

					nvrhi::DrawArguments arguments;
					arguments.vertexCount = command.ElemCount;
					arguments.startIndexLocation = command.IdxOffset + globalIndexOffset;
					arguments.startVertexLocation = command.VtxOffset + globalVertexOffset;
					commandList->drawIndexed(arguments);
				}
				globalIndexOffset += static_cast<uint32_t>(drawList->IdxBuffer.Size);
				globalVertexOffset += static_cast<uint32_t>(drawList->VtxBuffer.Size);
			}
		}

		commandList->close();
		device->executeCommandList(commandList);
	}

	ImTextureID ImGuiRenderer::GetTextureID(nvrhi::ITexture* texture)
	{
		return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
	}

	ImGuiRenderer* ImGuiRenderer::GetRenderer()
	{
		return ImGui::GetCurrentContext() != nullptr ? static_cast<ImGuiRenderer*>(ImGui::GetIO().BackendRendererUserData) : nullptr;
	}

	void ImGuiRenderer::OnCreateWindow(ImGuiViewport* viewport)
	{
		ImGuiRenderer* renderer = GetRenderer();
		ST_CORE_ASSERT(renderer != nullptr, "ImGui renderer backend is not installed");

		ViewportData& data = renderer->CreateViewportData(viewport);
		viewport->RendererUserData = &data;

		glm::uvec2 const size = GetFramebufferSize(viewport);
		SwapchainSpecification specification;
		specification.Window = static_cast<GLFWwindow*>(viewport->PlatformHandle);
		specification.Width = size.x;
		specification.Height = size.y;
		// Secondary windows must not block on vsync, otherwise every extra window divides the frame rate.
		specification.VSync = false;

		Result<Scope<Swapchain>> swapchain = Swapchain::Create(specification);
		if (!swapchain)
		{
			ST_CORE_ERROR("Failed to create a swapchain for an ImGui window: {}", swapchain.GetError());
			return;
		}
		data.WindowSwapchain = swapchain.TakeValue();
	}

	void ImGuiRenderer::OnDestroyWindow(ImGuiViewport* viewport)
	{
		ImGuiRenderer* renderer = GetRenderer();
		if (renderer != nullptr && viewport != ImGui::GetMainViewport())
		{
			renderer->m_Viewports.erase(viewport);
		}
		viewport->RendererUserData = nullptr;
	}

	void ImGuiRenderer::OnSetWindowSize(ImGuiViewport* viewport, ImVec2 size)
	{
		(void)size;
		auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
		if (data != nullptr && data->WindowSwapchain)
		{
			// ImGui passes the size in screen coordinates; the swapchain needs pixels.
			glm::uvec2 const framebufferSize = GetFramebufferSize(viewport);
			data->WindowSwapchain->Resize(framebufferSize.x, framebufferSize.y);
		}
	}

	void ImGuiRenderer::OnRenderWindow(ImGuiViewport* viewport, void* renderArgument)
	{
		(void)renderArgument;
		ImGuiRenderer* renderer = GetRenderer();
		auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
		if (renderer == nullptr || data == nullptr || !data->WindowSwapchain)
		{
			return;
		}

		data->FrameAcquired = data->WindowSwapchain->BeginFrame();
		if (data->FrameAcquired)
		{
			// Always cleared (even with ImGuiViewportFlags_NoRendererClear): every acquired image must be written before
			// it is presented.
			renderer->RenderDrawData(viewport->DrawData, data->WindowSwapchain->GetCurrentFramebuffer(), true);
		}
	}

	void ImGuiRenderer::OnSwapBuffers(ImGuiViewport* viewport, void* renderArgument)
	{
		(void)renderArgument;
		auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
		if (data != nullptr && data->WindowSwapchain && data->FrameAcquired)
		{
			data->WindowSwapchain->Present();
			data->FrameAcquired = false;
		}
	}
}
