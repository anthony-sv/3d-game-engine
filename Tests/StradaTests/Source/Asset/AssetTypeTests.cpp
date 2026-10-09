#include "TestUtilities.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	Buffer ReadFile(std::filesystem::path const& path)
	{
		Result<Buffer> data = FileSystem::ReadBinaryFile(path);
		REQUIRE(data.IsOk());
		return std::move(data.GetValue());
	}

	Buffer MakeBuffer(std::initializer_list<uint8_t> bytes)
	{
		std::vector<uint8_t> const data(bytes);
		return Buffer::Copy(data.data(), data.size());
	}

	Buffer MakeWav()
	{
		// RIFF header of a 1-sample, 8-bit mono PCM file.
		std::vector<uint8_t> data = {'R', 'I', 'F', 'F', 37, 0, 0,   0,   'W', 'A',  'V',  'E', 'f', 'm',  't',
		                             ' ', 16,  0,   0,   0,  1, 0,   1,   0,   0x44, 0xAC, 0,   0,   0x44, 0xAC,
		                             0,   0,   1,   0,   8,  0, 'd', 'a', 't', 'a',  1,    0,   0,   0,    128};
		return Buffer::Copy(data.data(), data.size());
	}
}

TEST_CASE("TextureAsset: encoded images are validated and decoded to RGBA")
{
	Testing::TemporaryDirectory directory;
	Image gray(3, 2, 1);
	gray.GetPixel(2, 1)[0] = 200;
	std::filesystem::path const path = directory.GetPath() / "Gray.png";
	REQUIRE(gray.WritePNG(path).IsOk());

	Result<Ref<TextureAsset>> texture = TextureAsset::CreateFromEncoded(ReadFile(path));
	REQUIRE(texture.IsOk());
	CHECK(texture.GetValue()->GetAssetType() == AssetType::Texture);
	CHECK(texture.GetValue()->IsEncoded());
	CHECK(texture.GetValue()->GetWidth() == 3);
	CHECK(texture.GetValue()->GetHeight() == 2);

	Result<Image> decoded = texture.GetValue()->Decode();
	REQUIRE(decoded.IsOk());
	CHECK(decoded.GetValue().GetChannels() == 4);
	uint8_t const* pixel = decoded.GetValue().GetPixel(2, 1);
	CHECK(pixel[0] == 200);
	CHECK(pixel[1] == 200);
	CHECK(pixel[3] == 255);

	CHECK(TextureAsset::CreateFromEncoded(MakeBuffer({1, 2, 3, 4})).IsError());
	CHECK(TextureAsset::CreateFromEncoded(Buffer()).IsError());
}

TEST_CASE("TextureAsset: decoded images expand to RGBA")
{
	Image rgb(1, 1, 3);
	rgb.GetPixel(0, 0)[0] = 10;
	rgb.GetPixel(0, 0)[1] = 20;
	rgb.GetPixel(0, 0)[2] = 30;
	Result<Ref<TextureAsset>> texture = TextureAsset::CreateFromImage(rgb);
	REQUIRE(texture.IsOk());
	CHECK_FALSE(texture.GetValue()->IsEncoded());
	Result<Image> decoded = texture.GetValue()->Decode();
	REQUIRE(decoded.IsOk());
	CHECK(decoded.GetValue().GetPixel(0, 0)[2] == 30);
	CHECK(decoded.GetValue().GetPixel(0, 0)[3] == 255);

	Image grayAlpha(1, 1, 2);
	grayAlpha.GetPixel(0, 0)[0] = 7;
	grayAlpha.GetPixel(0, 0)[1] = 9;
	Result<Image> expanded = TextureAsset::CreateFromImage(grayAlpha).GetValue()->Decode();
	REQUIRE(expanded.IsOk());
	CHECK(expanded.GetValue().GetPixel(0, 0)[1] == 7);
	CHECK(expanded.GetValue().GetPixel(0, 0)[3] == 9);

	CHECK(TextureAsset::CreateFromImage(Image()).IsError());
}

TEST_CASE("EnvironmentAsset: only HDR data is accepted")
{
	Testing::TemporaryDirectory directory;
	HdrImage image(2, 1, 3);
	image.GetPixel(1, 0)[0] = 4.0f;
	std::filesystem::path const hdrPath = directory.GetPath() / "Sky.hdr";
	REQUIRE(image.WriteHDR(hdrPath).IsOk());

	Result<Ref<EnvironmentAsset>> environment = EnvironmentAsset::CreateFromEncoded(ReadFile(hdrPath));
	REQUIRE(environment.IsOk());
	CHECK(environment.GetValue()->GetAssetType() == AssetType::Environment);
	CHECK(environment.GetValue()->GetWidth() == 2);
	Result<HdrImage> decoded = environment.GetValue()->Decode();
	REQUIRE(decoded.IsOk());
	CHECK(decoded.GetValue().GetChannels() == 4);
	CHECK(decoded.GetValue().GetPixel(1, 0)[0] == doctest::Approx(4.0f).epsilon(0.01));

	Result<Ref<EnvironmentAsset>> fromImage = EnvironmentAsset::CreateFromImage(image);
	REQUIRE(fromImage.IsOk());
	CHECK(fromImage.GetValue()->Decode().GetValue().GetPixel(1, 0)[3] == 1.0f);

	std::filesystem::path const pngPath = directory.GetPath() / "Ldr.png";
	REQUIRE(Image(2, 2, 4).WritePNG(pngPath).IsOk());
	CHECK(EnvironmentAsset::CreateFromEncoded(ReadFile(pngPath)).GetError() == "environments must be Radiance HDR images");
	CHECK(EnvironmentAsset::CreateFromImage(HdrImage(2, 2, 1)).IsError());
}

TEST_CASE("FontAsset: TrueType fonts are accepted and other data rejected")
{
	Result<Ref<FontAsset>> font = FontAsset::Create(ReadFile(STRADA_TEST_FONT_PATH));
	REQUIRE(font.IsOk());
	CHECK(font.GetValue()->GetAssetType() == AssetType::Font);
	CHECK(font.GetValue()->GetData().size() > 1000);

	CHECK(FontAsset::Create(Buffer()).IsError());
	std::vector<uint8_t> garbage(256, 0x5A);
	CHECK(FontAsset::Create(Buffer::Copy(garbage.data(), garbage.size())).IsError());
}

TEST_CASE("AudioClipAsset: formats are detected from their signatures")
{
	CHECK(DetectAudioFormat(MakeWav().GetSpan()) == AudioFormat::Wav);
	CHECK(DetectAudioFormat(MakeBuffer({'f', 'L', 'a', 'C', 0}).GetSpan()) == AudioFormat::Flac);
	CHECK(DetectAudioFormat(MakeBuffer({'O', 'g', 'g', 'S', 0}).GetSpan()) == AudioFormat::Ogg);
	CHECK(DetectAudioFormat(MakeBuffer({'I', 'D', '3', 4, 0}).GetSpan()) == AudioFormat::Mp3);
	CHECK(DetectAudioFormat(MakeBuffer({0xFF, 0xFB, 0x90, 0x00}).GetSpan()) == AudioFormat::Mp3);
	CHECK(DetectAudioFormat(MakeBuffer({0xFF, 0xF9, 0x90, 0x00}).GetSpan()) == AudioFormat::Unknown);
	CHECK(DetectAudioFormat(MakeBuffer({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' '}).GetSpan()) == AudioFormat::Unknown);
	CHECK(DetectAudioFormat({}) == AudioFormat::Unknown);
	CHECK(std::string(AudioFormatToString(AudioFormat::Ogg)) == "Ogg Vorbis");

	Result<Ref<AudioClipAsset>> clip = AudioClipAsset::Create(MakeWav());
	REQUIRE(clip.IsOk());
	CHECK(clip.GetValue()->GetFormat() == AudioFormat::Wav);
	CHECK(clip.GetValue()->GetAssetType() == AssetType::AudioClip);
	CHECK(AudioClipAsset::Create(MakeBuffer({1, 2, 3})).IsError());
}

TEST_CASE("PrefabAsset: documents are validated")
{
	Json document = Json::object();
	document["Strada"] = MakeFileHeader("Prefab", PrefabAsset::FormatVersion);
	document["Entities"] = Json::array({Json::object({{"ID", "1"}, {"Components", Json::object()}})});
	Result<Ref<PrefabAsset>> prefab = PrefabAsset::Create(document);
	REQUIRE(prefab.IsOk());
	CHECK(prefab.GetValue()->GetDocument() == document);

	document["Entities"] = Json::array();
	CHECK(PrefabAsset::Create(document).GetError() == "prefab has no entities");
	document["Strada"] = MakeFileHeader("Scene", 1);
	CHECK(PrefabAsset::Create(document).IsError());
}

TEST_CASE("MaterialAsset: files round trip and accept asset references")
{
	Testing::TemporaryDirectory directory;
	MaterialData data;
	data.BaseColor = {0.8f, 0.1f, 0.1f, 1.0f};
	data.Metallic = 1.0f;
	data.Roughness = 0.25f;
	data.BaseColorTexture = AssetHandle(UUID(70001));
	data.AlphaMode = MaterialAlphaMode::Mask;
	data.UVTiling = {2.0f, 3.0f};

	std::filesystem::path const path = directory.GetPath() / "Red.smat";
	REQUIRE(MaterialSerializer::SaveToFile(data, path).IsOk());
	Result<MaterialData> loaded = MaterialSerializer::LoadFromFile(path, DeserializationContext{});
	REQUIRE(loaded.IsOk());
	CHECK(loaded.GetValue() == data);

	Json document = MaterialSerializer::Serialize(data);
	CHECK(document["Strada"]["Type"] == "Material");
	CHECK(document["Material"]["AlphaMode"] == "Mask");
	CHECK(document["Material"]["BaseColorTexture"] == "70001");

	// Missing fields keep defaults; references resolve through the context.
	Json partial = Json::object();
	partial["Strada"] = MakeFileHeader("Material", MaterialSerializer::FormatVersion);
	partial["Material"] = Json::object({{"NormalTexture", "asset://Textures/Bricks_Normal.png"}});
	DeserializationContext context;
	context.ResolveAssetReference = [](std::string_view reference) -> Result<AssetHandle>
	{
		if (reference == "asset://Textures/Bricks_Normal.png")
		{
			return AssetHandle(UUID(70002));
		}
		return Error{"unknown"};
	};
	Result<MaterialData> resolved = MaterialSerializer::Deserialize(partial, context);
	REQUIRE(resolved.IsOk());
	CHECK(resolved.GetValue().NormalTexture == AssetHandle(UUID(70002)));
	CHECK(resolved.GetValue().Roughness == doctest::Approx(0.5f));

	partial["Material"] = Json::object({{"Roughness", "smooth"}});
	CHECK(MaterialSerializer::Deserialize(partial, context).GetError() == "Material.Roughness: expected a number");
	partial["Material"] = Json::object({{"Shininess", 3}});
	CHECK(MaterialSerializer::Deserialize(partial, context).IsError());
	CHECK(MaterialSerializer::Deserialize(Json::object(), context).IsError());

	MaterialAsset asset(data);
	CHECK(asset.GetVersion() == 0);
	MaterialData changed = asset.GetData();
	changed.Roughness = 0.9f;
	asset.SetData(changed);
	CHECK(asset.GetVersion() == 1);
	CHECK(asset.GetData().Roughness == doctest::Approx(0.9f));
}

TEST_CASE("Asset: every supported extension maps to an asset type, ignoring case")
{
	std::vector<std::string_view> const extensions = GetSupportedAssetExtensions();
	REQUIRE_FALSE(extensions.empty());
	for (size_t i = 0; i < extensions.size(); i++)
	{
		std::string_view const extension = extensions[i];
		CAPTURE(extension);
		CHECK(extension.starts_with("."));
		CHECK(GetAssetTypeForExtension(extension) != AssetType::None);
		std::string upper(extension);
		for (char& character : upper)
		{
			character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
		}
		CHECK(upper != extension);
		CHECK(GetAssetTypeForExtension(upper) == GetAssetTypeForExtension(extension));
		CHECK(std::find(extensions.begin() + static_cast<std::ptrdiff_t>(i) + 1, extensions.end(), extension) == extensions.end());
	}
	CHECK(GetAssetTypeForExtension(".sscene") == AssetType::Scene);
	CHECK(GetAssetTypeForExtension(".glb") == AssetType::Mesh);
	CHECK(GetAssetTypeForExtension(".ttf") == AssetType::Font);
	CHECK(GetAssetTypeForExtension(".txt") == AssetType::None);
	CHECK(GetAssetTypeForExtension("") == AssetType::None);
}
