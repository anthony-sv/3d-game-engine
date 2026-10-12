#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Audio/AudioWaveform.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"
#include "Strada/Math/AABB.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Strada
{
	class AudioClipAsset;
	class FontAtlas;
	class SceneRenderer;

	// Where a thumbnail's camera goes to frame bounds: looking at their center from above and in front (yaw and pitch in
	// degrees), far enough for the bounding sphere to fit the vertical field of view.
	struct ThumbnailCamera
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Target = glm::vec3(0.0f);
		float NearPlane = 0.01f;
	};
	ThumbnailCamera FrameThumbnailBounds(AABB const& bounds, float verticalFovRadians, float yawDegrees = 35.0f,
	                                     float pitchDegrees = -25.0f);

	// Previews of assets for the content browser. Textures show themselves; materials (on a sphere), meshes, prefabs and
	// environments (their sky) are rendered into small images, and fonts write "Aa" with their glyphs, at most one per Update
	// so browsing stays smooth. Audio clips get waveforms, decoded on a worker thread one clip at a time. A preview is made
	// again when its asset is reloaded, unloaded because its file changed, or (materials) edited; one that failed because its
	// asset did not load is tried again once the asset manager forgets that (a refresh, which may find the file repaired).
	// Main thread only; images need the Renderer.
	class AssetThumbnails
	{
	public:
		// Pixels of rendered thumbnails (square).
		static constexpr uint32_t Size = 256;
		// Thumbnails kept by default; the least recently shown go first.
		static constexpr size_t MaxThumbnails = 512;
		// Parts of the clips' waveforms.
		static constexpr uint32_t WaveformParts = 96;
		// Waveforms kept by default; the least recently shown go first.
		static constexpr size_t MaxWaveforms = 1024;

		// Keeps at most maxThumbnails thumbnails and maxWaveforms waveforms.
		explicit AssetThumbnails(size_t maxThumbnails = MaxThumbnails, size_t maxWaveforms = MaxWaveforms);
		// Stops the waveform being computed.
		~AssetThumbnails();
		AssetThumbnails(AssetThumbnails const&) = delete;
		AssetThumbnails& operator=(AssetThumbnails const&) = delete;

		// Whether the asset type has thumbnail images.
		static bool Supports(AssetType type);

		// The asset's thumbnail, or null while it is not rendered yet (it is queued) or cannot be (the content browser shows
		// the type's icon then). Textures come back with their own size; rendered thumbnails are Size x Size, and those of
		// fonts are white "Aa" on transparent pixels.
		nvrhi::ITexture* Get(AssetHandle asset);
		// The waveform of an audio clip, or null while it is computed (queued, the most recently requested first) or when
		// the clip cannot be decoded.
		AudioWaveform const* GetWaveform(AssetHandle clip);
		// Why the asset cannot be read, when its thumbnail or waveform failed because the file did not load (or, for audio
		// clips, decode); null otherwise, also before its preview was tried.
		std::string const* GetLoadError(AssetHandle asset) const;
		// Renders the most recently requested thumbnail that is missing or out of date, collects or starts a waveform, and
		// forgets the least recently shown previews beyond the limits. Call once per frame.
		void Update();
		void Clear();

		// Text (UTF-8, one line) written with a font's glyph atlas, white with anti-aliased coverage in the alpha channel
		// (RGBA, straight alpha), its ink centered and filling about three quarters of a square image of the given size.
		// Fails when the size is 0 or the font has no outlines for the text.
		[[nodiscard]] static Result<Image> RenderFontSample(FontAtlas& atlas, std::string_view text, uint32_t size);

	private:
		struct Entry
		{
			nvrhi::TextureHandle Texture;
			// What the thumbnail shows: rendered again when the loaded asset or its version changes.
			std::weak_ptr<Asset> Source;
			// The asset loaded (Source is it); false when its load failed.
			bool Loaded = false;
			uint64_t SourceVersion = 0;
			bool Failed = false;
			// Why the asset did not load when that made the thumbnail fail; empty otherwise.
			std::string LoadError;
			uint64_t LastUse = 0;
		};

		struct WaveformEntry
		{
			std::optional<AudioWaveform> Waveform;
			std::weak_ptr<Asset> Source;
			bool Loaded = false;
			bool Failed = false;
			// The failure was logged.
			bool Reported = false;
			// Why the clip did not load or decode when the waveform failed; empty otherwise.
			std::string LoadError;
			uint64_t LastUse = 0;
		};

		// A waveform computed on a worker thread.
		struct WaveformJob
		{
			AssetHandle Clip;
			Ref<AudioClipAsset> Source;
			std::future<Result<AudioWaveform>> Future;
		};

		// Whether the preview shows the asset as it is now: the same loaded asset (and material version). Previews are made
		// from loaded assets, which stay loaded until their files change: one that is no longer loaded is out of date. A
		// preview of an asset that did not load is current while the asset manager remembers the failed load.
		static bool IsCurrent(std::weak_ptr<Asset> const& source, bool sourceLoaded, uint64_t sourceVersion, bool failed,
		                      AssetHandle asset);
		bool Render(AssetHandle asset, Entry& entry);
		bool RenderFont(AssetHandle asset, Entry& entry);
		bool Upload(Image const& image, Entry& entry);
		void UpdateWaveforms();
		void StopWaveformJob();
		void Evict();

		size_t m_MaxThumbnails;
		size_t m_MaxWaveforms;
		Scope<SceneRenderer> m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		std::unordered_map<AssetHandle, Entry> m_Entries;
		AssetHandle m_Requested;
		std::unordered_map<AssetHandle, WaveformEntry> m_Waveforms;
		AssetHandle m_RequestedWaveform;
		std::optional<WaveformJob> m_WaveformJob;
		std::atomic<bool> m_CancelWaveform = false;
		uint64_t m_UseCounter = 0;
	};
}
