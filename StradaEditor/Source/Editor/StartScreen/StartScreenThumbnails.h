#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <map>

namespace Strada
{
	// The images of the start screen's and the New Project form's cards: the PNG thumbnails of the templates and the
	// examples as GPU textures, each file read once. Without a graphics device none is made, and the cards draw without
	// them. Main thread only.
	class StartScreenThumbnails
	{
	public:
		// A thumbnail wider or taller than this is not shown (they are 640 x 360).
		static constexpr uint32_t MaxSide = 2048;

		StartScreenThumbnails() = default;
		StartScreenThumbnails(StartScreenThumbnails const&) = delete;
		StartScreenThumbnails& operator=(StartScreenThumbnails const&) = delete;

		// The image of a thumbnail file, or null while there is none: no graphics device, a file that does not decode, or one
		// not read yet. At most one file is read and uploaded per frame (ImGui's frame count), so a screen of cards appears
		// over a few frames without a pause. A file that failed is not read again until Release.
		nvrhi::ITexture* Get(std::filesystem::path const& file);
		// Releases every texture: before the graphics device shuts down (EditorLayer::OnDetach), never while a frame that
		// draws them is being built.
		void Release();

		// How many images are held.
		size_t GetCount() const;

	private:
		// Null for a file that failed.
		std::map<std::filesystem::path, nvrhi::TextureHandle> m_Textures;
		int m_LastReadFrame = -1;
		nvrhi::CommandListHandle m_CommandList;
	};
}
