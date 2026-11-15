#pragma once

#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Strada
{
	// The Strada icon (the one windows get, EmbeddedResources::DefaultIcon) as an image the editor draws in its title bar.
	// The icon is many times larger than it is drawn and ImGui's images have no smaller levels to sample, so the image is
	// made at the very size it is drawn at, and made again when that size changes (another display scale). Without a
	// graphics device there is none, and the bar draws without it. Main thread only.
	class EditorWindowIcon
	{
	public:
		// The largest side an image is made at.
		static constexpr uint32_t MaxSide = 256;

		EditorWindowIcon() = default;
		EditorWindowIcon(EditorWindowIcon const&) = delete;
		EditorWindowIcon& operator=(EditorWindowIcon const&) = delete;

		// An RGBA image scaled to a square of the given side. Each pixel is the average of the pixels it covers, with the
		// ones it covers in part counted by that part, and colors weigh as much as they are opaque: an image scaled down
		// many times stays smooth, and what is transparent lends no color to its neighbors. Refuses an image that is empty
		// or not RGBA, and a side of 0 or above MaxSide.
		[[nodiscard]] static Result<Image> Scale(Image const& image, uint32_t side);

		// The icon as an image of the given side in pixels, or null while there is none: no graphics device, a side of 0 or
		// above MaxSide, or an icon that was not made (logged once; not tried again until the side changes or Release).
		nvrhi::ITexture* Get(uint32_t side);
		// Releases the image: before the graphics device shuts down (EditorLayer::OnDetach).
		void Release();

	private:
		nvrhi::TextureHandle m_Texture;
		// The side the image was made at or failed at; 0 before the first.
		uint32_t m_Side = 0;
		nvrhi::CommandListHandle m_CommandList;
	};
}
