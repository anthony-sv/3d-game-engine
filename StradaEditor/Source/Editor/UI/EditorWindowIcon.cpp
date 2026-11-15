#include "Editor/UI/EditorWindowIcon.h"

#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/Log.h"
#include "Strada/RHI/GraphicsDevice.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// The source pixels one target pixel covers along an axis, and how much of each.
		struct Coverage
		{
			uint32_t First = 0;
			std::vector<float> Weights;
		};

		// The coverage of each of `count` equal parts of `size` source pixels.
		std::vector<Coverage> FindCoverage(uint32_t size, uint32_t count)
		{
			std::vector<Coverage> coverage(count);
			double const step = static_cast<double>(size) / count;
			for (uint32_t index = 0; index < count; index++)
			{
				double const start = index * step;
				double const end = std::min((index + 1) * step, static_cast<double>(size));
				uint32_t const first = std::min(static_cast<uint32_t>(start), size - 1);
				uint32_t const last = std::min(static_cast<uint32_t>(std::ceil(end)), size);
				coverage[index].First = first;
				for (uint32_t pixel = first; pixel < std::max(last, first + 1); pixel++)
				{
					double const covered = std::min(end, pixel + 1.0) - std::max(start, static_cast<double>(pixel));
					coverage[index].Weights.push_back(static_cast<float>(std::max(covered, 0.0)));
				}
			}
			return coverage;
		}

		uint8_t ToByte(float value)
		{
			return static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
		}
	}

	Result<Image> EditorWindowIcon::Scale(Image const& image, uint32_t side)
	{
		if (image.IsEmpty() || image.GetChannels() != 4)
		{
			return MakeError("the image is not RGBA");
		}
		if (side == 0 || side > MaxSide)
		{
			return MakeError("{} is not a side of 1 to {} pixels", side, MaxSide);
		}
		std::vector<Coverage> const columns = FindCoverage(image.GetWidth(), side);
		std::vector<Coverage> const rows = FindCoverage(image.GetHeight(), side);
		Image scaled(side, side, 4);
		for (uint32_t y = 0; y < side; y++)
		{
			for (uint32_t x = 0; x < side; x++)
			{
				float color[3] = {0.0f, 0.0f, 0.0f};
				float alpha = 0.0f;
				float area = 0.0f;
				for (size_t row = 0; row < rows[y].Weights.size(); row++)
				{
					for (size_t column = 0; column < columns[x].Weights.size(); column++)
					{
						uint8_t const* const pixel =
							image.GetPixel(columns[x].First + static_cast<uint32_t>(column), rows[y].First + static_cast<uint32_t>(row));
						float const weight = rows[y].Weights[row] * columns[x].Weights[column];
						float const opacity = weight * pixel[3];
						color[0] += opacity * pixel[0];
						color[1] += opacity * pixel[1];
						color[2] += opacity * pixel[2];
						alpha += opacity;
						area += weight;
					}
				}
				uint8_t* const target = scaled.GetPixel(x, y);
				for (int channel = 0; channel < 3; channel++)
				{
					target[channel] = alpha > 0.0f ? ToByte(color[channel] / alpha) : uint8_t{0};
				}
				target[3] = area > 0.0f ? ToByte(alpha / area) : uint8_t{0};
			}
		}
		return scaled;
	}

	nvrhi::ITexture* EditorWindowIcon::Get(uint32_t side)
	{
		if (!GraphicsDevice::IsInitialized() || side == 0 || side > MaxSide)
		{
			return nullptr;
		}
		if (side == m_Side)
		{
			return m_Texture.Get();
		}
		// The renderer keeps the image it last drew until the frames that show it are done.
		m_Texture = nullptr;
		m_Side = side;

		Result<Image> const icon = Image::LoadFromMemory(EmbeddedResources::Get(EmbeddedResources::DefaultIcon), 4);
		if (!icon)
		{
			ST_WARN("The title bar has no icon: {}", icon.GetError());
			return nullptr;
		}
		Result<Image> const scaled = Scale(icon.GetValue(), side);
		if (!scaled)
		{
			ST_WARN("The title bar has no icon: {}", scaled.GetError());
			return nullptr;
		}
		nvrhi::IDevice* const device = GraphicsDevice::GetDevice();
		nvrhi::TextureDesc desc;
		desc.width = side;
		desc.height = side;
		// The swapchain is 8-bit UNORM: the icon's sRGB values are drawn as they are, as thumbnails are.
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Title bar icon";
		nvrhi::TextureHandle created = device->createTexture(desc);
		if (!created)
		{
			ST_WARN("The title bar has no icon: no texture was made for it");
			return nullptr;
		}
		if (!m_CommandList)
		{
			m_CommandList = device->createCommandList();
		}
		m_CommandList->open();
		m_CommandList->writeTexture(created, 0, 0, scaled.GetValue().GetPixels().data(), static_cast<size_t>(side) * 4);
		m_CommandList->close();
		device->executeCommandList(m_CommandList);
		m_Texture = std::move(created);
		return m_Texture.Get();
	}

	void EditorWindowIcon::Release()
	{
		m_Texture = nullptr;
		m_CommandList = nullptr;
		m_Side = 0;
	}
}
