#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Base.h"
#include "Strada/Math/AABB.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace Strada
{
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
	// environments (their sky) are rendered into small images, at most one per Update so browsing stays smooth. A thumbnail
	// is rendered again when its asset is reloaded or (materials) edited. Main thread only; needs the Renderer.
	class AssetThumbnails
	{
	public:
		// Pixels of rendered thumbnails (square).
		static constexpr uint32_t Size = 256;
		// Rendered thumbnails kept; the least recently shown go first.
		static constexpr size_t MaxThumbnails = 512;

		AssetThumbnails();
		~AssetThumbnails();
		AssetThumbnails(AssetThumbnails const&) = delete;
		AssetThumbnails& operator=(AssetThumbnails const&) = delete;

		// Whether the asset type has thumbnails.
		static bool Supports(AssetType type);

		// The asset's thumbnail, or null while it is not rendered yet (it is queued) or cannot be (the content browser shows
		// the type's badge then). Textures come back with their own size; rendered thumbnails are Size x Size.
		nvrhi::ITexture* Get(AssetHandle asset);
		// Renders the most recently requested thumbnail that is missing or out of date. Call once per frame.
		void Update();
		void Clear();

	private:
		struct Entry
		{
			nvrhi::TextureHandle Texture;
			// What the thumbnail shows: re-rendered when the loaded asset or its version changes.
			std::weak_ptr<Asset> Source;
			uint64_t SourceVersion = 0;
			bool Failed = false;
			uint64_t LastUse = 0;
		};

		// Whether the entry shows the asset as it is now loaded (an unloaded asset is not loaded for this).
		static bool IsCurrent(Entry const& entry, AssetHandle asset);
		bool Render(AssetHandle asset, Entry& entry);
		void Evict();

		Scope<SceneRenderer> m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		std::unordered_map<AssetHandle, Entry> m_Entries;
		AssetHandle m_Requested;
		uint64_t m_UseCounter = 0;
	};
}
