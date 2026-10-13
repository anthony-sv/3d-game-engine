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
#include <filesystem>
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
	class TextureAsset;

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

	// Previews of assets for the editor's panels: the editor keeps one set, which the content browser's tiles and the
	// inspector's asset fields, picker rows and material headers share. Textures are shrunk to small images on a worker
	// thread, one texture at a time (decoded and halved there, then uploaded by Update), so showing one never decodes,
	// mip-maps or uploads a whole texture on the main thread; materials (on a sphere), meshes, prefabs (their particles a
	// moment after they start) and environments (their sky) are rendered into small images, and fonts write "Aa" with their
	// glyphs, at most one per Update so browsing stays smooth. Audio clips get waveforms, decoded on a worker thread one
	// clip at a time. Assets loaded only for a preview (with the meshes, materials and textures a rendered preview needs)
	// are unloaded once it is made, so browsing keeps no more in memory than the scene and the panels use. A preview is
	// made again when its asset's file changes, or its asset in use is reloaded or (materials) edited; one that failed
	// because its asset did not load is tried again once the asset manager forgets that (a refresh, which may find the file
	// repaired). Main thread only; images need the Renderer.
	class AssetThumbnails
	{
	public:
		// Pixels of rendered thumbnails (square), and the most either side of a texture's thumbnail has.
		static constexpr uint32_t Size = 256;
		// Thumbnails kept by default; the least recently shown go first.
		static constexpr size_t MaxThumbnails = 512;
		// Parts of the clips' waveforms.
		static constexpr uint32_t WaveformParts = 96;
		// Waveforms kept by default; the least recently shown go first.
		static constexpr size_t MaxWaveforms = 1024;

		// Keeps at most maxThumbnails thumbnails and maxWaveforms waveforms.
		explicit AssetThumbnails(size_t maxThumbnails = MaxThumbnails, size_t maxWaveforms = MaxWaveforms);
		// Stops the waveform and the texture thumbnail being made.
		~AssetThumbnails();
		AssetThumbnails(AssetThumbnails const&) = delete;
		AssetThumbnails& operator=(AssetThumbnails const&) = delete;

		// Whether the asset type has thumbnail images.
		static bool Supports(AssetType type);

		// The asset's thumbnail, or null while it is not made yet (it is queued: the first one asked for since the last
		// Update is made next) or cannot be (panels show the type's icon then; prefabs that draw nothing, such as a light
		// alone, have none). While a thumbnail is made again, the last one is returned. Textures come back with their aspect ratio, halved
		// until no side is longer than Size (smaller ones keep their size), with the texture's stored values (sRGB colors as they are);
		// rendered thumbnails are Size x Size, and those of fonts are white "Aa" on transparent pixels. Valid until the next Update or
		// Clear.
		nvrhi::ITexture* Get(AssetHandle asset);
		// The waveform of an audio clip, or null while it is computed (queued, the most recently requested first) or when
		// the clip cannot be decoded.
		AudioWaveform const* GetWaveform(AssetHandle clip);
		// Why the asset cannot be read, when its thumbnail or waveform failed because the file did not load (or, for textures
		// and audio clips, decode); null otherwise, also before its preview was tried.
		std::string const* GetLoadError(AssetHandle asset) const;
		// Renders the thumbnail first asked for since the last Update that is missing or out of date, collects or starts a
		// texture thumbnail and a waveform, and forgets the least recently shown previews beyond the limits. Call once per
		// frame before the panels draw: it replaces and releases images, which must not be in the frame's draw data.
		void Update();
		// Forgets every preview and stops those being made; before the panels draw, like Update.
		void Clear();

		// Text (UTF-8, one line) written with a font's glyph atlas, white with anti-aliased coverage in the alpha channel
		// (RGBA, straight alpha), its ink centered and filling about three quarters of a square image of the given size.
		// Fails when the size is 0 or the font has no outlines for the text.
		[[nodiscard]] static Result<Image> RenderFontSample(FontAtlas& atlas, std::string_view text, uint32_t size);
		// The image of a texture's thumbnail: the texture decoded (RGBA), then halved as mip levels are made (colors averaged
		// in linear light) until neither side is longer than maxSize; smaller textures keep their size. Touches no engine
		// state, so it may run on any thread; setting *cancel from another thread stops it. Fails when maxSize is 0, when the
		// texture cannot be decoded (the error says why, in words for users) and when cancelled.
		[[nodiscard]] static Result<Image> ShrinkTexture(TextureAsset const& texture, uint32_t maxSize,
		                                                 std::atomic<bool> const* cancel = nullptr);

	private:
		// What a preview was made from, to tell when it is out of date (IsCurrent).
		struct PreviewSource
		{
			// The loaded asset; empty when its load failed.
			std::weak_ptr<Asset> Object;
			bool Loaded = false;
			// Materials: their version (edits change it).
			uint64_t Version = 0;
			// The asset was loaded for the preview alone and unloaded once it was made; WriteTime is its file's then, still
			// found at the asset manager's version CheckedVersion.
			bool Unloaded = false;
			std::optional<std::filesystem::file_time_type> WriteTime;
			uint64_t CheckedVersion = 0;
		};

		struct Entry
		{
			nvrhi::TextureHandle Texture;
			PreviewSource Source;
			bool Failed = false;
			// Why the asset did not load when that made the thumbnail fail; empty otherwise.
			std::string LoadError;
			uint64_t LastUse = 0;
		};

		struct WaveformEntry
		{
			std::optional<AudioWaveform> Waveform;
			PreviewSource Source;
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
			// The clip was loaded for the waveform alone: unloaded once it is made.
			bool Unload = false;
			std::optional<std::filesystem::file_time_type> WriteTime;
			std::future<Result<AudioWaveform>> Future;
		};

		// A texture's thumbnail image made on a worker thread (ShrinkTexture).
		struct TextureJob
		{
			AssetHandle Texture;
			Ref<TextureAsset> Source;
			// The texture was loaded for the thumbnail alone: unloaded once it is made.
			bool Unload = false;
			std::optional<std::filesystem::file_time_type> WriteTime;
			std::future<Result<Image>> Future;
		};

		// Whether the preview shows the asset as it is now. An asset in use when the preview was made stays loaded until its
		// file changes (or it is reloaded): the preview is current while the same asset (and material version) is loaded.
		// One loaded for the preview alone is current while its file is unchanged, as far as the asset manager's refreshes
		// tell, also when it is loaded again since (unless edited). A preview of an asset that did not load is current
		// while the asset manager remembers the failed load.
		static bool IsCurrent(PreviewSource& source, bool failed, AssetHandle asset);
		// The source of a preview of the loaded asset, which the preview loaded (wasLoaded false) or found in use.
		static PreviewSource MakeSource(Ref<Asset> const& asset, bool wasLoaded, std::optional<std::filesystem::file_time_type> writeTime);
		// Renders the thumbnail of a material, mesh, prefab or environment, then unloads what it loaded for that alone.
		bool Render(AssetHandle asset, Entry& entry);
		// The rendering itself: false when there is nothing to show.
		bool RenderPreview(AssetHandle asset, Entry& entry);
		bool RenderFont(AssetHandle asset, Entry& entry);
		// Copies the image (RGBA) into the entry's texture, which is made again at the image's size when it has another one.
		bool Upload(Image const& image, Entry& entry);
		void UpdateWaveforms();
		void StopWaveformJob();
		// Collects the finished texture thumbnail and starts the one asked for.
		void UpdateTextures();
		void StopTextureJob();
		void Evict();

		size_t m_MaxThumbnails;
		size_t m_MaxWaveforms;
		Scope<SceneRenderer> m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		std::unordered_map<AssetHandle, Entry> m_Entries;
		// The rendered thumbnail and the texture thumbnail made next.
		AssetHandle m_Requested;
		AssetHandle m_RequestedTexture;
		std::optional<TextureJob> m_TextureJob;
		std::atomic<bool> m_CancelTexture = false;
		std::unordered_map<AssetHandle, WaveformEntry> m_Waveforms;
		AssetHandle m_RequestedWaveform;
		std::optional<WaveformJob> m_WaveformJob;
		std::atomic<bool> m_CancelWaveform = false;
		uint64_t m_UseCounter = 0;
	};
}
