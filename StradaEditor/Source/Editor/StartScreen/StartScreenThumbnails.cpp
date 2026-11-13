#include "Editor/StartScreen/StartScreenThumbnails.h"

#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Log.h"
#include "Strada/RHI/GraphicsDevice.h"

#include <imgui.h>

#include <utility>

namespace Strada
{
	namespace
	{
		// The thumbnail's pixels (RGBA), or why there are none; the size is checked before the pixels are decoded.
		Result<Image> ReadThumbnail(std::filesystem::path const& file)
		{
			Result<Buffer> bytes = FileSystem::ReadBinaryFile(file);
			if (!bytes)
			{
				return Error{bytes.GetError()};
			}
			Buffer const& data = bytes.GetValue();
			Result<ImageInfo> info = Image::ReadInfo(data.GetSpan());
			if (!info)
			{
				return Error{info.GetError()};
			}
			if (info.GetValue().IsHdr || info.GetValue().Width == 0 || info.GetValue().Height == 0 ||
			    info.GetValue().Width > StartScreenThumbnails::MaxSide || info.GetValue().Height > StartScreenThumbnails::MaxSide)
			{
				return MakeError("{} x {} is not the size of a thumbnail", info.GetValue().Width, info.GetValue().Height);
			}
			return Image::LoadFromMemory(data.GetSpan(), 4);
		}
	}

	nvrhi::ITexture* StartScreenThumbnails::Get(std::filesystem::path const& file)
	{
		if (file.empty())
		{
			return nullptr;
		}
		if (auto const found = m_Textures.find(file); found != m_Textures.end())
		{
			return found->second.Get();
		}
		int const frame = ImGui::GetFrameCount();
		if (!GraphicsDevice::IsInitialized() || frame == m_LastReadFrame)
		{
			return nullptr;
		}
		m_LastReadFrame = frame;
		nvrhi::TextureHandle& texture = m_Textures[file];
		Result<Image> image = ReadThumbnail(file);
		if (!image)
		{
			ST_WARN("The thumbnail '{}' cannot be shown: {}", FileSystem::PathToUtf8(file), image.GetError());
			return nullptr;
		}
		Image const& pixels = image.GetValue();
		nvrhi::IDevice* const device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = pixels.GetWidth();
		desc.height = pixels.GetHeight();
		// The swapchain is 8-bit UNORM: the PNG's sRGB values are drawn as they are, as asset thumbnails are.
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Start screen thumbnail";
		nvrhi::TextureHandle created = device->createTexture(desc);
		if (!created)
		{
			ST_WARN("The thumbnail '{}' cannot be shown: no texture was made for it", FileSystem::PathToUtf8(file));
			return nullptr;
		}
		if (!m_CommandList)
		{
			m_CommandList = device->createCommandList();
		}
		m_CommandList->open();
		m_CommandList->writeTexture(created, 0, 0, pixels.GetPixels().data(), static_cast<size_t>(pixels.GetWidth()) * 4);
		m_CommandList->close();
		device->executeCommandList(m_CommandList);
		texture = std::move(created);
		return texture.Get();
	}

	void StartScreenThumbnails::Release()
	{
		m_Textures.clear();
		m_CommandList = nullptr;
		m_LastReadFrame = -1;
	}

	size_t StartScreenThumbnails::GetCount() const
	{
		size_t count = 0;
		for (auto const& entry : m_Textures)
		{
			count += entry.second ? 1 : 0;
		}
		return count;
	}
}
