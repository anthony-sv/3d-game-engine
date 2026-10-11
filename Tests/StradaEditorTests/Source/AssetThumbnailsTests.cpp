#include "Audio/AudioTestUtilities.h"
#include "Editor/AssetThumbnails.h"
#include "GpuTestUtilities.h"
#include "Renderer/FontTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Log.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/Renderer.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace Strada;

namespace
{
	// A material file of the given base color.
	std::string MakeMaterialFile(glm::vec3 const& color)
	{
		return fmt::format(
			R"({{"Strada": {{"Version": 1, "Type": "Material"}}, "Material": {{"BaseColor": [{}, {}, {}, 1.0], "Roughness": 0.6}}}})",
			color.r, color.g, color.b);
	}

	// Updates the thumbnails until the clip's waveform is ready or a generous time has passed (it is decoded on a worker).
	AudioWaveform const* WaitForWaveform(AssetThumbnails& thumbnails, AssetHandle clip)
	{
		for (int attempt = 0; attempt < 1000; attempt++)
		{
			thumbnails.Update();
			if (AudioWaveform const* waveform = thumbnails.GetWaveform(clip))
			{
				return waveform;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return nullptr;
	}

	// The average color of the image's central quarter.
	glm::vec3 CenterColor(Image const& image)
	{
		glm::vec3 sum(0.0f);
		uint32_t count = 0;
		for (uint32_t y = image.GetHeight() * 3 / 8; y < image.GetHeight() * 5 / 8; y++)
		{
			for (uint32_t x = image.GetWidth() * 3 / 8; x < image.GetWidth() * 5 / 8; x++)
			{
				uint8_t const* pixel = image.GetPixel(x, y);
				sum += glm::vec3(pixel[0], pixel[1], pixel[2]);
				count++;
			}
		}
		return sum / static_cast<float>(count);
	}
}

TEST_CASE("AssetThumbnails: framing puts the bounds' center in view with the bounding sphere inside the field of view")
{
	AABB bounds;
	bounds.Expand(glm::vec3(1.0f, 0.0f, -1.0f));
	bounds.Expand(glm::vec3(3.0f, 2.0f, 1.0f));
	float const fov = glm::radians(30.0f);
	ThumbnailCamera const camera = FrameThumbnailBounds(bounds, fov);
	CHECK(camera.Target.x == doctest::Approx(2.0f));
	CHECK(camera.Target.y == doctest::Approx(1.0f));
	CHECK(camera.Target.z == doctest::Approx(0.0f));
	float const radius = std::sqrt(3.0f);
	float const distance = glm::length(camera.Position - camera.Target);
	// The sphere's silhouette fits: sin(half angle it covers) = radius / distance <= sin(fov / 2).
	CHECK(radius / distance <= std::sin(fov * 0.5f));
	CHECK(distance < radius / std::sin(fov * 0.5f) * 1.2f);
	// Above and in front (+Z) of the bounds, looking down at them.
	CHECK(camera.Position.y > camera.Target.y);
	CHECK(camera.Position.z > camera.Target.z);
	CHECK(camera.NearPlane > 0.0f);
	CHECK(camera.NearPlane < distance - radius);

	// Invalid bounds (nothing to show) frame the origin.
	ThumbnailCamera const empty = FrameThumbnailBounds(AABB(), fov);
	CHECK(empty.Target == glm::vec3(0.0f));
	CHECK(std::isfinite(empty.Position.x));
}

TEST_CASE("AssetThumbnails: which assets have thumbnails, and none without a renderer")
{
	CHECK(AssetThumbnails::Supports(AssetType::Texture));
	CHECK(AssetThumbnails::Supports(AssetType::Material));
	CHECK(AssetThumbnails::Supports(AssetType::Mesh));
	CHECK(AssetThumbnails::Supports(AssetType::Prefab));
	CHECK(AssetThumbnails::Supports(AssetType::Environment));
	CHECK(AssetThumbnails::Supports(AssetType::Font));
	CHECK_FALSE(AssetThumbnails::Supports(AssetType::Scene));
	CHECK_FALSE(AssetThumbnails::Supports(AssetType::AudioClip));

	AssetManager::Init();
	AssetThumbnails thumbnails;
	CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::DefaultMaterial)) == nullptr);
	thumbnails.Update();
	CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::DefaultMaterial)) == nullptr);
	AssetManager::Shutdown();
}

TEST_CASE("AssetThumbnails: material thumbnails show the material and follow its edits")
{
	ST_REQUIRE_GPU();
	AssetManager::Init();
	REQUIRE(Renderer::Init().IsOk());
	{
		MaterialData red;
		red.BaseColor = glm::vec4(0.9f, 0.05f, 0.05f, 1.0f);
		red.Roughness = 0.6f;
		Ref<MaterialAsset> const material = CreateRef<MaterialAsset>(red);
		AssetHandle const handle = AssetManager::AddMemoryAsset(material, "Red");

		AssetThumbnails thumbnails;
		// Rendered on the next update, one per update.
		CHECK(thumbnails.Get(handle) == nullptr);
		CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::SphereMesh)) == nullptr);
		thumbnails.Update();
		nvrhi::ITexture* const texture = thumbnails.Get(handle);
		REQUIRE(texture != nullptr);
		CHECK(texture->getDesc().width == AssetThumbnails::Size);
		CHECK(texture->getDesc().height == AssetThumbnails::Size);
		CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::SphereMesh)) == nullptr);
		thumbnails.Update();
		CHECK(thumbnails.Get(GetBuiltInHandle(BuiltInAsset::SphereMesh)) != nullptr);

		Result<Image> const before = ReadbackTexture(texture);
		REQUIRE(before);
		glm::vec3 const redCenter = CenterColor(before.GetValue());
		CHECK(redCenter.r > redCenter.g * 2.0f);
		CHECK(redCenter.r > redCenter.b * 2.0f);

		// Editing the material renders it again.
		MaterialData blue = red;
		blue.BaseColor = glm::vec4(0.05f, 0.05f, 0.9f, 1.0f);
		material->SetData(blue);
		thumbnails.Get(handle);
		thumbnails.Update();
		nvrhi::ITexture* const updated = thumbnails.Get(handle);
		REQUIRE(updated != nullptr);
		Result<Image> const after = ReadbackTexture(updated);
		REQUIRE(after);
		glm::vec3 const blueCenter = CenterColor(after.GetValue());
		CHECK(blueCenter.b > blueCenter.r * 2.0f);

		// Textures are their own thumbnails.
		nvrhi::ITexture* const white = thumbnails.Get(GetBuiltInHandle(BuiltInAsset::WhiteTexture));
		REQUIRE(white != nullptr);
		CHECK(white != updated);

		AssetManager::RemoveMemoryAsset(handle);
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
	AssetManager::Shutdown();
}

TEST_CASE("AssetThumbnails: font samples write text with the font's own glyphs")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	Result<Image> const sample = AssetThumbnails::RenderFontSample(*atlas, "Aa", 64);
	REQUIRE(sample.IsOk());
	Image const& image = sample.GetValue();
	REQUIRE(image.GetWidth() == 64);
	REQUIRE(image.GetHeight() == 64);
	REQUIRE(image.GetChannels() == 4);

	// White everywhere; the glyphs are in the alpha channel, anti-aliased, centered and filling about three quarters.
	bool white = true;
	bool partial = false;
	uint32_t left = 64;
	uint32_t right = 0;
	uint32_t top = 64;
	uint32_t bottom = 0;
	for (uint32_t y = 0; y < 64; y++)
	{
		for (uint32_t x = 0; x < 64; x++)
		{
			uint8_t const* pixel = image.GetPixel(x, y);
			white = white && pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255;
			partial = partial || (pixel[3] > 0 && pixel[3] < 255);
			if (pixel[3] >= 128)
			{
				left = std::min(left, x);
				right = std::max(right, x);
				top = std::min(top, y);
				bottom = std::max(bottom, y);
			}
		}
	}
	CHECK(white);
	CHECK(partial);
	REQUIRE(right > left);
	REQUIRE(bottom > top);
	uint32_t const extent = std::max(right - left, bottom - top) + 1;
	CHECK(extent >= 40);
	CHECK(extent <= 52);
	CHECK(std::abs(static_cast<int>(left + right) - 63) <= 4);
	CHECK(std::abs(static_cast<int>(top + bottom) - 63) <= 4);
	CHECK(image.GetPixel(0, 0)[3] == 0);
	CHECK(image.GetPixel(63, 63)[3] == 0);

	// Other text, other pixels.
	Result<Image> const other = AssetThumbnails::RenderFontSample(*atlas, "Bb", 64);
	REQUIRE(other.IsOk());
	CHECK(other.GetValue().GetPixels() != image.GetPixels());

	// No pixels, or no outlines to write: no sample.
	CHECK(AssetThumbnails::RenderFontSample(*atlas, "Aa", 0).IsError());
	CHECK(AssetThumbnails::RenderFontSample(*atlas, "   ", 64).IsError());
	CHECK(AssetThumbnails::RenderFontSample(*atlas, "", 64).IsError());
}

TEST_CASE("AssetThumbnails: audio clips get waveforms decoded on a worker thread")
{
	Testing::AssetManagerScope assets;
	AssetHandle const clip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 0.5f}, {0.25f, 0.5f}}), "Tone");
	AssetThumbnails thumbnails;
	// Requested by the first look, decoded after.
	CHECK(thumbnails.GetWaveform(clip) == nullptr);
	AudioWaveform const* const waveform = WaitForWaveform(thumbnails, clip);
	REQUIRE(waveform != nullptr);
	CHECK(waveform->Peaks.size() == AssetThumbnails::WaveformParts);
	CHECK(waveform->Peaks.front() == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(waveform->Peaks.back() == doctest::Approx(0.25f).epsilon(0.01));
	CHECK(waveform->Duration == doctest::Approx(1.0).epsilon(0.001));
	// Kept: asking again does not decode again.
	CHECK(thumbnails.GetWaveform(clip) == waveform);

	// Other assets have none; clips that cannot be decoded neither (said once in the log).
	CHECK(thumbnails.GetWaveform(GetBuiltInHandle(BuiltInAsset::DefaultMaterial)) == nullptr);
	std::vector<uint8_t> const broken = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E', 1, 2, 3, 4};
	Result<Ref<AudioClipAsset>> brokenClip = AudioClipAsset::Create(Buffer::Copy(broken.data(), broken.size()));
	REQUIRE(brokenClip.IsOk());
	AssetHandle const noise = AssetManager::AddMemoryAsset(brokenClip.TakeValue(), "Noise");
	uint64_t const firstLog = Log::GetNextEntryIndex();
	for (int attempt = 0; attempt < 1000 && !Testing::WasLogged(firstLog, "cannot be shown"); attempt++)
	{
		CHECK(thumbnails.GetWaveform(noise) == nullptr);
		thumbnails.Update();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(Testing::WasLogged(firstLog, "cannot be shown"));
	CHECK(thumbnails.GetWaveform(noise) == nullptr);
	// The content browser marks such clips: they cannot be read.
	std::string const* const error = thumbnails.GetLoadError(noise);
	REQUIRE(error != nullptr);
	CHECK(error->find("cannot be decoded") != std::string::npos);
	CHECK(thumbnails.GetLoadError(clip) == nullptr);
	CHECK(thumbnails.GetLoadError(AssetHandle()) == nullptr);

	// Clearing forgets them.
	thumbnails.Clear();
	CHECK(thumbnails.GetWaveform(clip) == nullptr);
	CHECK(WaitForWaveform(thumbnails, clip) != nullptr);
}

TEST_CASE("AssetThumbnails: files changed outside the editor get new thumbnails, and fonts write Aa in their glyphs")
{
	ST_REQUIRE_GPU();
	Testing::TemporaryDirectory directory;
	AssetManager::Init();
	REQUIRE(Renderer::Init().IsOk());
	{
		std::filesystem::path const file = directory.GetPath() / "Paint.smat";
		REQUIRE(FileSystem::WriteTextFile(file, MakeMaterialFile(glm::vec3(0.9f, 0.05f, 0.05f))).IsOk());
		std::span<uint8_t const> const font = EmbeddedResources::Get(EmbeddedResources::DefaultFont);
		REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "Title.ttf", font).IsOk());
		REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
		AssetHandle const paint = AssetManager::FindByPath("Paint.smat");
		AssetHandle const title = AssetManager::FindByPath("Title.ttf");
		REQUIRE(paint.IsValid());
		REQUIRE(title.IsValid());

		AssetThumbnails thumbnails;
		CHECK(thumbnails.Get(paint) == nullptr);
		thumbnails.Update();
		nvrhi::ITexture* const red = thumbnails.Get(paint);
		REQUIRE(red != nullptr);
		Result<Image> const before = ReadbackTexture(red);
		REQUIRE(before);
		glm::vec3 const redCenter = CenterColor(before.GetValue());
		CHECK(redCenter.r > redCenter.b * 2.0f);

		// Another application makes it blue: the refresh that notices unloads it, and its thumbnail is rendered again.
		std::filesystem::file_time_type const written = FileSystem::GetLastWriteTime(file).value();
		REQUIRE(FileSystem::WriteTextFile(file, MakeMaterialFile(glm::vec3(0.05f, 0.05f, 0.9f))).IsOk());
		std::error_code timeError;
		std::filesystem::last_write_time(file, written + std::chrono::seconds(2), timeError);
		REQUIRE_FALSE(timeError);
		Result<AssetRefreshResult> const refreshed = AssetManager::Refresh();
		REQUIRE(refreshed.IsOk());
		CHECK(std::find(refreshed.GetValue().Modified.begin(), refreshed.GetValue().Modified.end(), paint) !=
		      refreshed.GetValue().Modified.end());
		CHECK_FALSE(AssetManager::IsLoaded(paint));
		thumbnails.Get(paint);
		thumbnails.Update();
		nvrhi::ITexture* const blue = thumbnails.Get(paint);
		REQUIRE(blue != nullptr);
		Result<Image> const after = ReadbackTexture(blue);
		REQUIRE(after);
		glm::vec3 const blueCenter = CenterColor(after.GetValue());
		CHECK(blueCenter.b > blueCenter.r * 2.0f);

		// Fonts: white "Aa" over transparent pixels.
		CHECK(thumbnails.Get(title) == nullptr);
		thumbnails.Update();
		nvrhi::ITexture* const sample = thumbnails.Get(title);
		REQUIRE(sample != nullptr);
		CHECK(sample->getDesc().width == AssetThumbnails::Size);
		Result<Image> const glyphs = ReadbackTexture(sample);
		REQUIRE(glyphs);
		uint32_t covered = 0;
		for (uint32_t y = 0; y < glyphs.GetValue().GetHeight(); y++)
		{
			for (uint32_t x = 0; x < glyphs.GetValue().GetWidth(); x++)
			{
				covered += glyphs.GetValue().GetPixel(x, y)[3] >= 128 ? 1u : 0u;
			}
		}
		CHECK(covered > AssetThumbnails::Size * AssetThumbnails::Size / 20);
		CHECK(glyphs.GetValue().GetPixel(0, 0)[3] == 0);
		CHECK(thumbnails.GetLoadError(title) == nullptr);

		// Files that do not load have no thumbnail and say why, once their thumbnail was tried; textures at once.
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Broken.smat", "not a material").IsOk());
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Broken.png", "not an image").IsOk());
		REQUIRE(Image(4, 4, 4).WritePNG(directory.GetPath() / "Fine.png").IsOk());
		REQUIRE(AssetManager::Refresh().IsOk());
		AssetHandle const broken = AssetManager::FindByPath("Broken.smat");
		AssetHandle const brokenTexture = AssetManager::FindByPath("Broken.png");
		AssetHandle const fineTexture = AssetManager::FindByPath("Fine.png");
		REQUIRE(broken.IsValid());
		REQUIRE(brokenTexture.IsValid());
		REQUIRE(fineTexture.IsValid());
		CHECK(thumbnails.GetLoadError(broken) == nullptr);
		CHECK(thumbnails.Get(broken) == nullptr);
		thumbnails.Update();
		CHECK(thumbnails.Get(broken) == nullptr);
		REQUIRE(thumbnails.GetLoadError(broken) != nullptr);
		CHECK_FALSE(thumbnails.GetLoadError(broken)->empty());
		CHECK(thumbnails.GetLoadError(paint) == nullptr);
		CHECK(thumbnails.Get(brokenTexture) == nullptr);
		REQUIRE(thumbnails.GetLoadError(brokenTexture) != nullptr);
		CHECK(thumbnails.Get(fineTexture) != nullptr);
		CHECK(thumbnails.GetLoadError(fineTexture) == nullptr);
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
	AssetManager::Shutdown();
}
