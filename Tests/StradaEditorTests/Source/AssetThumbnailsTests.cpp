#include "Audio/AudioTestUtilities.h"
#include "Editor/AssetThumbnails.h"
#include "GpuTestUtilities.h"
#include "Renderer/FontTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/Core/EmbeddedResources.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Log.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/Renderer.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

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

	// Updates the thumbnails until the asset has a thumbnail or a generous time has passed (textures are shrunk on a worker
	// thread).
	nvrhi::ITexture* WaitForThumbnail(AssetThumbnails& thumbnails, AssetHandle asset)
	{
		for (int attempt = 0; attempt < 1000; attempt++)
		{
			if (nvrhi::ITexture* const thumbnail = thumbnails.Get(asset))
			{
				return thumbnail;
			}
			thumbnails.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return nullptr;
	}

	// An image whose left half is one color and right half another (opaque).
	Image MakeHalvesImage(uint32_t width, uint32_t height, glm::u8vec3 left, glm::u8vec3 right)
	{
		Image image(width, height, 4);
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				glm::u8vec3 const color = x < width / 2 ? left : right;
				uint8_t* pixel = image.GetPixel(x, y);
				pixel[0] = color.r;
				pixel[1] = color.g;
				pixel[2] = color.b;
				pixel[3] = 255;
			}
		}
		return image;
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
	CHECK_FALSE(AssetThumbnails::Supports(AssetType::VisualScript));

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

		AssetManager::RemoveMemoryAsset(handle);
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
	AssetManager::Shutdown();
}

TEST_CASE("AssetThumbnails: texture thumbnails are halved until they fit, keep their aspect ratio and average in linear light")
{
	glm::u8vec3 const red(220, 30, 30);
	glm::u8vec3 const blue(30, 30, 220);
	Result<Ref<TextureAsset>> const wide = TextureAsset::CreateFromImage(MakeHalvesImage(1024, 512, red, blue));
	REQUIRE(wide.IsOk());
	Result<Image> const shrunk = AssetThumbnails::ShrinkTexture(*wide.GetValue(), AssetThumbnails::Size);
	REQUIRE(shrunk.IsOk());
	Image const& image = shrunk.GetValue();
	CHECK(image.GetWidth() == AssetThumbnails::Size);
	CHECK(image.GetHeight() == AssetThumbnails::Size / 2);
	CHECK(image.GetChannels() == 4);
	CHECK(glm::u8vec3(image.GetPixel(10, 64)[0], image.GetPixel(10, 64)[1], image.GetPixel(10, 64)[2]) == red);
	CHECK(glm::u8vec3(image.GetPixel(245, 64)[0], image.GetPixel(245, 64)[1], image.GetPixel(245, 64)[2]) == blue);

	// Fine black and white checks become linear mid-gray: 188 in sRGB, where averaging the stored values would give 128.
	Image checks(512, 512, 4);
	for (uint32_t y = 0; y < 512; y++)
	{
		for (uint32_t x = 0; x < 512; x++)
		{
			uint8_t* pixel = checks.GetPixel(x, y);
			pixel[0] = pixel[1] = pixel[2] = (x + y) % 2 == 0 ? 255 : 0;
			pixel[3] = 255;
		}
	}
	Result<Ref<TextureAsset>> const checked = TextureAsset::CreateFromImage(checks);
	REQUIRE(checked.IsOk());
	Result<Image> const gray = AssetThumbnails::ShrinkTexture(*checked.GetValue(), AssetThumbnails::Size);
	REQUIRE(gray.IsOk());
	CHECK(gray.GetValue().GetWidth() == AssetThumbnails::Size);
	CHECK(gray.GetValue().GetPixel(100, 100)[0] == 188);

	// Encoded textures are decoded first; small ones keep their size and pixels, and odd sizes round down when halved.
	Image const small = MakeHalvesImage(40, 30, red, blue);
	Result<std::vector<uint8_t>> const encoded = small.EncodePNG();
	REQUIRE(encoded.IsOk());
	Result<Ref<TextureAsset>> const file =
		TextureAsset::CreateFromEncoded(Buffer::Copy(encoded.GetValue().data(), encoded.GetValue().size()));
	REQUIRE(file.IsOk());
	Result<Image> const kept = AssetThumbnails::ShrinkTexture(*file.GetValue(), AssetThumbnails::Size);
	REQUIRE(kept.IsOk());
	CHECK(kept.GetValue().GetWidth() == 40);
	CHECK(kept.GetValue().GetHeight() == 30);
	CHECK(kept.GetValue().GetPixels() == small.GetPixels());
	Result<Ref<TextureAsset>> const odd = TextureAsset::CreateFromImage(Image(301, 77, 4));
	REQUIRE(odd.IsOk());
	Result<Image> const oddShrunk = AssetThumbnails::ShrinkTexture(*odd.GetValue(), AssetThumbnails::Size);
	REQUIRE(oddShrunk.IsOk());
	CHECK(oddShrunk.GetValue().GetWidth() == 150);
	CHECK(oddShrunk.GetValue().GetHeight() == 38);

	// A file whose header reads but whose pixels do not decode, no room for pixels, and cancelled work fail.
	std::vector<uint8_t> truncated = encoded.GetValue();
	truncated.resize(60);
	Result<Ref<TextureAsset>> const damaged = TextureAsset::CreateFromEncoded(Buffer::Copy(truncated.data(), truncated.size()));
	REQUIRE(damaged.IsOk());
	Result<Image> const undecoded = AssetThumbnails::ShrinkTexture(*damaged.GetValue(), AssetThumbnails::Size);
	REQUIRE(undecoded.IsError());
	CHECK(undecoded.GetError().find("cannot be decoded") != std::string::npos);
	CHECK(AssetThumbnails::ShrinkTexture(*wide.GetValue(), 0).IsError());
	std::atomic<bool> const cancel = true;
	CHECK(AssetThumbnails::ShrinkTexture(*wide.GetValue(), AssetThumbnails::Size, &cancel).IsError());
}

TEST_CASE("AssetThumbnails: textures get thumbnails of their own, made on a worker thread and uploaded by an update")
{
	ST_REQUIRE_GPU();
	AssetManager::Init();
	REQUIRE(Renderer::Init().IsOk());
	{
		glm::u8vec3 const red(220, 30, 30);
		glm::u8vec3 const blue(30, 30, 220);
		Result<Ref<TextureAsset>> wide = TextureAsset::CreateFromImage(MakeHalvesImage(1024, 512, red, blue));
		REQUIRE(wide.IsOk());
		AssetHandle const handle = AssetManager::AddMemoryAsset(wide.TakeValue(), "Wide");

		AssetThumbnails thumbnails;
		// Asked for by the first look, made on a worker, uploaded by an update after it finished.
		CHECK(thumbnails.Get(handle) == nullptr);
		nvrhi::ITexture* const texture = WaitForThumbnail(thumbnails, handle);
		REQUIRE(texture != nullptr);
		// Small, without mip levels, with the texture's stored values (the swapchain shows them unchanged).
		nvrhi::TextureDesc const& desc = texture->getDesc();
		CHECK(desc.width == AssetThumbnails::Size);
		CHECK(desc.height == AssetThumbnails::Size / 2);
		CHECK(desc.mipLevels == 1);
		CHECK(desc.format == nvrhi::Format::RGBA8_UNORM);
		Result<Image> const pixels = ReadbackTexture(texture);
		REQUIRE(pixels);
		uint8_t const* left = pixels.GetValue().GetPixel(10, 64);
		uint8_t const* right = pixels.GetValue().GetPixel(245, 64);
		CHECK(glm::u8vec3(left[0], left[1], left[2]) == red);
		CHECK(glm::u8vec3(right[0], right[1], right[2]) == blue);
		// Kept: asking again does not make it again.
		thumbnails.Update();
		CHECK(thumbnails.Get(handle) == texture);
		CHECK(thumbnails.GetLoadError(handle) == nullptr);

		// Built-in textures too, at their own small size.
		nvrhi::ITexture* const white = WaitForThumbnail(thumbnails, GetBuiltInHandle(BuiltInAsset::WhiteTexture));
		REQUIRE(white != nullptr);
		CHECK(white->getDesc().width <= AssetThumbnails::Size);

		// Clearing forgets the thumbnails and stops the one being made; asking again makes it again.
		thumbnails.Clear();
		CHECK(thumbnails.Get(handle) == nullptr);
		thumbnails.Update();
		thumbnails.Clear();
		CHECK(WaitForThumbnail(thumbnails, handle) != nullptr);

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
	CHECK(error->find("it is damaged") != std::string::npos);
	CHECK(thumbnails.GetLoadError(clip) == nullptr);
	CHECK(thumbnails.GetLoadError(AssetHandle()) == nullptr);

	// Clearing forgets them.
	thumbnails.Clear();
	CHECK(thumbnails.GetWaveform(clip) == nullptr);
	CHECK(WaitForWaveform(thumbnails, clip) != nullptr);
}

TEST_CASE("AssetThumbnails: the least recently shown previews beyond the limits are forgotten every update")
{
	Testing::AssetManagerScope assets;
	std::array<AssetHandle, 3> const clips = {AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 0.1f}}), "A"),
	                                          AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.25f, 0.1f}}), "B"),
	                                          AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.75f, 0.1f}}), "C")};
	// Two waveforms are kept (and no thumbnails: none are rendered here, which once kept eviction from running).
	AssetThumbnails thumbnails(0, 2);
	REQUIRE(WaitForWaveform(thumbnails, clips[0]) != nullptr);
	REQUIRE(WaitForWaveform(thumbnails, clips[1]) != nullptr);
	// A third clip shown: the one shown longest ago goes.
	CHECK(thumbnails.GetWaveform(clips[2]) == nullptr);
	thumbnails.Update();
	CHECK(thumbnails.GetWaveform(clips[1]) != nullptr);
	CHECK(thumbnails.GetWaveform(clips[0]) == nullptr);
	CHECK(WaitForWaveform(thumbnails, clips[0]) != nullptr);
}

TEST_CASE("AssetThumbnails: a clip that did not decode gets its waveform as soon as a refresh finds its file repaired")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Hit.wav";
	std::vector<uint8_t> const broken = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E', 1, 2, 3, 4};
	REQUIRE(FileSystem::WriteBinaryFile(file, broken).IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const clip = AssetManager::FindByPath("Hit.wav");
	REQUIRE(clip.IsValid());

	AssetThumbnails thumbnails;
	for (int attempt = 0; attempt < 1000 && thumbnails.GetLoadError(clip) == nullptr; attempt++)
	{
		CHECK(thumbnails.GetWaveform(clip) == nullptr);
		thumbnails.Update();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(thumbnails.GetLoadError(clip) != nullptr);
	// It stays failed without asking again while nothing changed.
	for (int frame = 0; frame < 10; frame++)
	{
		CHECK(thumbnails.GetWaveform(clip) == nullptr);
		thumbnails.Update();
	}
	CHECK(thumbnails.GetLoadError(clip) != nullptr);

	// Another application repairs the file; the refresh that notices unloads the clip and its waveform is made right away.
	std::filesystem::file_time_type const written = FileSystem::GetLastWriteTime(file).value();
	std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.2f}});
	REQUIRE(FileSystem::WriteBinaryFile(file, Testing::MakeWav(samples).GetSpan()).IsOk());
	std::error_code timeError;
	std::filesystem::last_write_time(file, written + std::chrono::seconds(2), timeError);
	REQUIRE_FALSE(timeError);
	REQUIRE(AssetManager::Refresh().IsOk());
	std::chrono::steady_clock::time_point const refreshed = std::chrono::steady_clock::now();
	AudioWaveform const* const waveform = WaitForWaveform(thumbnails, clip);
	REQUIRE(waveform != nullptr);
	// Well before the five seconds failed previews once waited.
	CHECK(std::chrono::steady_clock::now() - refreshed < std::chrono::seconds(4));
	CHECK(waveform->Peaks.front() == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(thumbnails.GetLoadError(clip) == nullptr);
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

		// Loaded for its thumbnail alone, the material was let go.
		CHECK_FALSE(AssetManager::IsLoaded(paint));

		// Another application makes it blue: once a refresh noticed, its thumbnail is rendered again.
		std::filesystem::file_time_type const written = FileSystem::GetLastWriteTime(file).value();
		REQUIRE(FileSystem::WriteTextFile(file, MakeMaterialFile(glm::vec3(0.05f, 0.05f, 0.9f))).IsOk());
		std::error_code timeError;
		std::filesystem::last_write_time(file, written + std::chrono::seconds(2), timeError);
		REQUIRE_FALSE(timeError);
		REQUIRE(AssetManager::Refresh().IsOk());
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

		// Files that do not load or decode have no thumbnail and say why, once their thumbnail was tried.
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Broken.smat", "not a material").IsOk());
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Broken.png", "not an image").IsOk());
		Result<std::vector<uint8_t>> damaged = Image(64, 64, 4).EncodePNG();
		REQUIRE(damaged.IsOk());
		damaged.GetValue().resize(60);
		REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "Damaged.png", damaged.GetValue()).IsOk());
		REQUIRE(Image(4, 4, 4).WritePNG(directory.GetPath() / "Fine.png").IsOk());
		REQUIRE(AssetManager::Refresh().IsOk());
		AssetHandle const broken = AssetManager::FindByPath("Broken.smat");
		AssetHandle const brokenTexture = AssetManager::FindByPath("Broken.png");
		AssetHandle const damagedTexture = AssetManager::FindByPath("Damaged.png");
		AssetHandle const fineTexture = AssetManager::FindByPath("Fine.png");
		REQUIRE(broken.IsValid());
		REQUIRE(brokenTexture.IsValid());
		REQUIRE(damagedTexture.IsValid());
		REQUIRE(fineTexture.IsValid());
		CHECK(thumbnails.GetLoadError(broken) == nullptr);
		CHECK(thumbnails.Get(broken) == nullptr);
		thumbnails.Update();
		CHECK(thumbnails.Get(broken) == nullptr);
		REQUIRE(thumbnails.GetLoadError(broken) != nullptr);
		CHECK_FALSE(thumbnails.GetLoadError(broken)->empty());
		CHECK(thumbnails.GetLoadError(paint) == nullptr);
		CHECK(thumbnails.GetLoadError(brokenTexture) == nullptr);
		CHECK(thumbnails.Get(brokenTexture) == nullptr);
		thumbnails.Update();
		CHECK(thumbnails.Get(brokenTexture) == nullptr);
		REQUIRE(thumbnails.GetLoadError(brokenTexture) != nullptr);
		for (int attempt = 0; attempt < 1000 && thumbnails.GetLoadError(damagedTexture) == nullptr; attempt++)
		{
			CHECK(thumbnails.Get(damagedTexture) == nullptr);
			thumbnails.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		REQUIRE(thumbnails.GetLoadError(damagedTexture) != nullptr);
		CHECK(thumbnails.GetLoadError(damagedTexture)->find("cannot be decoded") != std::string::npos);
		nvrhi::ITexture* const fine = WaitForThumbnail(thumbnails, fineTexture);
		REQUIRE(fine != nullptr);
		CHECK(fine->getDesc().width == 4);
		CHECK(thumbnails.GetLoadError(fineTexture) == nullptr);

		// A texture changed by another application gets a thumbnail of its new size once a refresh notices.
		std::filesystem::path const fineFile = directory.GetPath() / "Fine.png";
		std::filesystem::file_time_type const fineWritten = FileSystem::GetLastWriteTime(fineFile).value();
		REQUIRE(Image(600, 300, 4).WritePNG(fineFile).IsOk());
		std::filesystem::last_write_time(fineFile, fineWritten + std::chrono::seconds(2), timeError);
		REQUIRE_FALSE(timeError);
		REQUIRE(AssetManager::Refresh().IsOk());
		nvrhi::ITexture* larger = nullptr;
		for (int attempt = 0; attempt < 1000 && larger == nullptr; attempt++)
		{
			thumbnails.Update();
			nvrhi::ITexture* const current = thumbnails.Get(fineTexture);
			larger = current != nullptr && current->getDesc().width != 4 ? current : nullptr;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		REQUIRE(larger != nullptr);
		CHECK(larger->getDesc().width == 150);
		CHECK(larger->getDesc().height == 75);
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
	AssetManager::Shutdown();
}

TEST_CASE("AssetThumbnails: what a preview loads for itself alone is let go once it is made")
{
	ST_REQUIRE_GPU();
	Testing::TemporaryDirectory directory;
	AssetManager::Init();
	REQUIRE(Renderer::Init().IsOk());
	{
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Paint.smat", MakeMaterialFile(glm::vec3(0.9f, 0.05f, 0.05f))).IsOk());
		REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Used.smat", MakeMaterialFile(glm::vec3(0.05f, 0.9f, 0.05f))).IsOk());
		REQUIRE(Image(64, 32, 4).WritePNG(directory.GetPath() / "Wood.png").IsOk());
		std::vector<float> const samples = Testing::MakeSamples({{0.5f, 0.2f}});
		REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "Hit.wav", Testing::MakeWav(samples).GetSpan()).IsOk());
		REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
		AssetHandle const paint = AssetManager::FindByPath("Paint.smat");
		AssetHandle const used = AssetManager::FindByPath("Used.smat");
		AssetHandle const wood = AssetManager::FindByPath("Wood.png");
		AssetHandle const hit = AssetManager::FindByPath("Hit.wav");
		REQUIRE(AssetManager::GetLoadedFileAssets().empty());

		// A rendered preview, a texture's and a waveform: each asset is loaded for the preview and let go after.
		AssetThumbnails thumbnails;
		thumbnails.Get(paint);
		thumbnails.Update();
		nvrhi::ITexture* const painted = thumbnails.Get(paint);
		REQUIRE(painted != nullptr);
		REQUIRE(WaitForThumbnail(thumbnails, wood) != nullptr);
		REQUIRE(WaitForWaveform(thumbnails, hit) != nullptr);
		CHECK(AssetManager::GetLoadedFileAssets().empty());
		// They stay current, without loading their assets again, while their files are unchanged.
		for (int frame = 0; frame < 3; frame++)
		{
			CHECK(thumbnails.Get(paint) == painted);
			CHECK(thumbnails.Get(wood) != nullptr);
			CHECK(thumbnails.GetWaveform(hit) != nullptr);
			thumbnails.Update();
		}
		CHECK(AssetManager::GetLoadedFileAssets().empty());
		// Loaded again by something else, an unchanged asset keeps its preview.
		REQUIRE(AssetManager::LoadAsset(paint).IsOk());
		thumbnails.Update();
		CHECK(thumbnails.Get(paint) == painted);
		AssetManager::UnloadAsset(paint);

		// An asset in use stays loaded, and its preview follows its edits.
		Result<Ref<MaterialAsset>> const inUse = AssetManager::TryGetAsset<MaterialAsset>(used);
		REQUIRE(inUse.IsOk());
		thumbnails.Get(used);
		thumbnails.Update();
		REQUIRE(thumbnails.Get(used) != nullptr);
		CHECK(AssetManager::IsLoaded(used));
		MaterialData blue = inUse.GetValue()->GetData();
		blue.BaseColor = glm::vec4(0.05f, 0.05f, 0.9f, 1.0f);
		inUse.GetValue()->SetData(blue);
		thumbnails.Get(used);
		thumbnails.Update();
		nvrhi::ITexture* const edited = thumbnails.Get(used);
		REQUIRE(edited != nullptr);
		Result<Image> const image = ReadbackTexture(edited);
		REQUIRE(image);
		glm::vec3 const center = CenterColor(image.GetValue());
		CHECK(center.b > center.g * 2.0f);
		CHECK(AssetManager::GetLoadedFileAssets() == std::vector<AssetHandle>{used});
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
	AssetManager::Shutdown();
}

TEST_CASE("AssetThumbnails: prefabs of particles show them a moment after they start, and prefabs that draw nothing have none")
{
	ST_REQUIRE_GPU();
	Testing::TemporaryDirectory directory;
	AssetManager::Init();
	REQUIRE(Renderer::Init().IsOk());
	{
		EditorContext context;
		EditorOperations operations(context);
		REQUIRE(operations.CreateProject(directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
		// A burst of green sparks, as the samples' effects are, and a lamp alone.
		ParticleSystemComponent sparks;
		sparks.Emitting = false;
		sparks.Burst = 80;
		sparks.Shape = ParticleEmitterShape::Sphere;
		sparks.SpeedMin = 1.0f;
		sparks.SpeedMax = 2.0f;
		sparks.StartSize = 0.5f;
		sparks.EndSize = 0.5f;
		sparks.StartColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
		sparks.EndColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
		EntityCreateInfo burst;
		burst.Name = "Burst";
		burst.Components = Json::object({{"ParticleSystem", SerializeComponent(sparks)}});
		Result<UUID> const burstEntity = operations.CreateEntity(burst);
		REQUIRE(burstEntity.IsOk());
		EntityCreateInfo lamp;
		lamp.Name = "Lamp";
		lamp.Components = Json::object({{"PointLight", Json::object()}});
		Result<UUID> const lampEntity = operations.CreateEntity(lamp);
		REQUIRE(lampEntity.IsOk());
		Result<AssetHandle> const burstPrefab = operations.CreatePrefab(burstEntity.GetValue(), "Burst.sprefab");
		Result<AssetHandle> const lampPrefab = operations.CreatePrefab(lampEntity.GetValue(), "Lamp.sprefab");
		REQUIRE(burstPrefab.IsOk());
		REQUIRE(lampPrefab.IsOk());

		AssetThumbnails thumbnails;
		thumbnails.Get(burstPrefab.GetValue());
		thumbnails.Update();
		nvrhi::ITexture* const shown = thumbnails.Get(burstPrefab.GetValue());
		REQUIRE(shown != nullptr);
		Result<Image> const image = ReadbackTexture(shown);
		REQUIRE(image);
		// The sparks fill the middle of the image, framed with their extent, glowing over a dark backdrop (where they add up,
		// the glow turns pale).
		glm::vec3 const center = CenterColor(image.GetValue());
		CHECK(center.g > 150.0f);
		CHECK(center.g > center.r + 25.0f);
		CHECK(center.g > center.b + 25.0f);
		uint8_t const* const corner = image.GetValue().GetPixel(2, 2);
		CHECK(corner[0] < 60);
		CHECK(corner[1] < 60);
		CHECK(corner[2] < 60);

		// A prefab that draws nothing gets no image (panels show the prefab icon), and no load error: it loads.
		thumbnails.Get(lampPrefab.GetValue());
		thumbnails.Update();
		CHECK(thumbnails.Get(lampPrefab.GetValue()) == nullptr);
		CHECK(thumbnails.GetLoadError(lampPrefab.GetValue()) == nullptr);
		// It is not tried again while its file is unchanged.
		thumbnails.Update();
		CHECK(thumbnails.Get(lampPrefab.GetValue()) == nullptr);
		GraphicsDevice::WaitForIdle();
	}
	Renderer::Shutdown();
	AssetManager::Shutdown();
}
