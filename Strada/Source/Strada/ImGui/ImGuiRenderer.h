#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <unordered_map>

namespace Strada
{
	class Swapchain;

	// Dear ImGui renderer backend on NVRHI. Supports ImGui's dynamic texture system (ImGuiBackendFlags_RendererHasTextures)
	// and multi-viewports (each platform window gets its own Swapchain). Requires an ImGui context and an initialized
	// GraphicsDevice; main thread only.
	class ImGuiRenderer
	{
	public:
		ImGuiRenderer();
		~ImGuiRenderer();

		ImGuiRenderer(ImGuiRenderer const&) = delete;
		ImGuiRenderer& operator=(ImGuiRenderer const&) = delete;

		[[nodiscard]] Result<void> Init();
		void Shutdown();

		// Services texture create/update/destroy requests from ImGui. Call once per frame before rendering.
		void UpdateTextures();
		// Renders a viewport's draw data into the framebuffer, optionally clearing it first.
		void RenderDrawData(ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer, bool clear);

		// Texture identifier for ImGui::Image. The texture must be in a shader-readable format.
		static ImTextureID GetTextureID(nvrhi::ITexture* texture);

	private:
		struct ViewportData
		{
			Scope<Swapchain> WindowSwapchain;
			nvrhi::BufferHandle VertexBuffer;
			nvrhi::BufferHandle IndexBuffer;
			size_t VertexCapacity = 0;
			size_t IndexCapacity = 0;
			nvrhi::CommandListHandle CommandList;
			bool FrameAcquired = false;
		};

		struct CachedBindingSet
		{
			nvrhi::TextureHandle Texture;
			nvrhi::BindingSetHandle BindingSet;
			uint64_t LastUsedFrame = 0;
		};

		ViewportData& CreateViewportData(ImGuiViewport* viewport);
		nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* framebuffer);
		nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture);
		void CreateTexture(ImTextureData* texture);
		void UpdateTexture(ImTextureData* texture);
		void DestroyTexture(ImTextureData* texture);

		static ImGuiRenderer* GetRenderer();
		static void OnCreateWindow(ImGuiViewport* viewport);
		static void OnDestroyWindow(ImGuiViewport* viewport);
		static void OnSetWindowSize(ImGuiViewport* viewport, ImVec2 size);
		static void OnRenderWindow(ImGuiViewport* viewport, void* renderArgument);
		static void OnSwapBuffers(ImGuiViewport* viewport, void* renderArgument);

		bool m_Initialized = false;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::CommandListHandle m_UploadCommandList;
		std::unordered_map<nvrhi::Format, nvrhi::GraphicsPipelineHandle> m_Pipelines;
		std::unordered_map<nvrhi::ITexture*, CachedBindingSet> m_BindingSets;
		std::unordered_map<ImTextureData*, nvrhi::TextureHandle> m_Textures;
		std::unordered_map<ImGuiViewport*, Scope<ViewportData>> m_Viewports;
		uint64_t m_FrameIndex = 0;
	};
}
