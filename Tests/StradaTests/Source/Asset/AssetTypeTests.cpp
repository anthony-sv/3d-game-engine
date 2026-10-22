#include "TestUtilities.h"

#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
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

TEST_CASE("MaterialAsset: imported parameters fit the material ranges and keep their emitted light")
{
	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();

	// Emission above 1 moves into the intensity: color times intensity stays the same, and nothing is reported.
	MaterialData glowing;
	glowing.EmissiveColor = {4.0f, 3.0f, 2.0f};
	glowing.EmissiveIntensity = 2.0f;
	CHECK(FitMaterialToFieldRanges(glowing).empty());
	CHECK(glowing.EmissiveColor == glm::vec3(1.0f, 0.75f, 0.5f));
	CHECK(glowing.EmissiveIntensity == 8.0f);

	// Parameters within their ranges are kept as they are.
	MaterialData plain;
	plain.EmissiveColor = {1.0f, 0.5f, 0.0f};
	plain.EmissiveIntensity = 3.0f;
	MaterialData const original = plain;
	CHECK(FitMaterialToFieldRanges(plain).empty());
	CHECK(plain == original);

	// The rest is clamped or replaced by the defaults and reported; a NaN channel does not scale the others.
	MaterialData broken;
	broken.BaseColor = {2.0f, -1.0f, 0.5f, 1.5f};
	broken.Metallic = NaN;
	broken.Roughness = 1.25f;
	broken.EmissiveColor = {NaN, 2.0f, 1.0f};
	broken.NormalStrength = -1.0f;
	broken.AlphaCutoff = 3.0f;
	broken.UVTiling = {NaN, 2.0f};
	CHECK(FitMaterialToFieldRanges(broken) == std::vector<std::string_view>{"BaseColor", "Metallic", "Roughness", "NormalStrength",
	                                                                        "EmissiveColor", "AlphaCutoff", "UVTiling"});
	CHECK(broken.BaseColor == glm::vec4(1.0f, 0.0f, 0.5f, 1.0f));
	CHECK(broken.Metallic == MaterialData().Metallic);
	CHECK(broken.Roughness == 1.0f);
	CHECK(broken.EmissiveColor == glm::vec3(0.0f, 1.0f, 0.5f));
	CHECK(broken.EmissiveIntensity == 2.0f);
	CHECK(broken.NormalStrength == 0.0f);
	CHECK(broken.AlphaCutoff == 1.0f);
	CHECK(broken.UVTiling == glm::vec2(1.0f, 2.0f));
	MaterialData readBack;
	REQUIRE(DeserializeFields<StructTraits<MaterialData>>(SerializeFields<StructTraits<MaterialData>>(broken), readBack,
	                                                      DeserializationContext{}, "material")
	            .IsOk());
	CHECK(readBack == broken);
}

TEST_CASE("VisualScriptAsset: documents give the same program unless something the scripting runtime reads differs")
{
	auto const makeNode = [](uint32_t id, std::string type)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id) * 20.0f, 40.0f);
		return node;
	};
	auto const makeLink = [](uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return VisualScriptLink{{from, std::move(fromPin)}, {to, std::move(toPin)}};
	};

	// Every member set, in the event graph and in a function's graph.
	VisualScriptDocument document;
	document.Description = "Casts a ray down and moves the entity onto what it hits";
	VisualScriptVariable speed;
	speed.ID = 1;
	speed.Name = "Speed";
	speed.Type = "float";
	speed.Value = 2.5;
	speed.Public = true;
	speed.Tooltip = "How fast it moves";
	speed.Range = glm::vec2(0.0f, 10.0f);
	speed.FormerNames = {"Pace"};
	document.Variables.push_back(speed);
	VisualScriptVariable ground;
	ground.ID = 2;
	ground.Name = "Ground";
	ground.Type = "float";
	ground.Value = 0.0;
	document.Variables.push_back(ground);

	VisualScriptFunction scale;
	scale.ID = 3;
	scale.Name = "Scale";
	scale.Pure = true;
	scale.Inputs = {{"Factor", "float"}};
	scale.Outputs = {{"Result", "float"}};
	scale.Locals = {{4, "Twice", "float", 2.0}};
	scale.Graph.Nodes = {makeNode(5, "Function.Entry"), makeNode(6, "Function.Return")};
	scale.Graph.Nodes[1].Comment = "Gives the factor back";
	scale.Graph.Links = {makeLink(5, "Factor", 6, "Result")};
	scale.Graph.Comments = {{7, "Scaling", {0.0f, 0.0f}, {300.0f, 200.0f}, {0.2f, 0.4f, 0.8f, 0.25f}}};
	document.Functions.push_back(scale);

	document.Graph.Nodes = {makeNode(10, "Event.Update"), makeNode(11, "Call:Strada.Physics.Raycast"), makeNode(12, "Variable.Set")};
	VisualScriptNode& raycast = document.Graph.Nodes[1];
	raycast.Settings = Json::object({{"Layers", Json::array({"Default"})}});
	raycast.Inputs = Json::object({{"Origin.Y", 5.0},
	                               {"Direction", Json::array({0, -1, 0})},
	                               {"MaxDistance", 100.0},
	                               {"LayerMask", 1},
	                               {"Seed", std::numeric_limits<uint64_t>::max()}});
	raycast.Split.Inputs = {"Origin"};
	raycast.Split.Outputs = {"Hit", "Hit.Point"};
	raycast.Comment = "Looks down";
	raycast.Collapsed = true;
	raycast.Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	document.Graph.Nodes[2].Settings = Json::object({{"Variable", 2}});
	document.Graph.Links = {makeLink(10, "Then", 11, "Execute"), makeLink(11, "Then", 12, "Execute"),
	                        makeLink(11, "Hit.Point.Y", 12, "Value")};
	document.Graph.Comments = {{13, "Ground probe", {-20.0f, -20.0f}, {600.0f, 240.0f}, {0.5f, 0.5f, 0.5f, 0.25f}}};

	CHECK(HaveSameVisualScriptProgram(document, document));
	CHECK(HaveSameVisualScriptProgram(VisualScriptDocument{}, VisualScriptDocument{}));

	// Copies of the document, each changed in one way.
	struct Change
	{
		std::string_view What;
		VisualScriptDocument Document;
	};
	struct Changes
	{
		VisualScriptDocument const& Original;
		std::vector<Change> Documents;

		VisualScriptDocument& Add(std::string_view what) { return Documents.emplace_back(Change{what, Original}).Document; }
	};

	// What the scripting runtime never reads.
	Changes ignored{document, {}};
	ignored.Add("the description").Description.clear();
	ignored.Add("a variable's tooltip").Variables[0].Tooltip = "Metres per second";
	ignored.Add("a variable's range").Variables[0].Range = glm::vec2(1.0f, 20.0f);
	ignored.Add("a variable's range removed").Variables[0].Range.reset();
	ignored.Add("a node moved").Graph.Nodes[1].Position = glm::vec2(500.0f, -60.0f);
	ignored.Add("a node unplaced").Graph.Nodes[1].Position.reset();
	ignored.Add("a node's comment").Graph.Nodes[1].Comment = "Looks around";
	ignored.Add("a node expanded").Graph.Nodes[1].Collapsed = false;
	ignored.Add("a node collapsed").Graph.Nodes[0].Collapsed = true;
	ignored.Add("a function's node moved").Functions[0].Graph.Nodes[0].Position = glm::vec2(9.0f);
	ignored.Add("a function's node comment removed").Functions[0].Graph.Nodes[1].Comment.clear();
	ignored.Add("a comment box moved").Graph.Comments[0].Position = glm::vec2(80.0f, 0.0f);
	ignored.Add("a comment box resized").Graph.Comments[0].Size = glm::vec2(100.0f, 50.0f);
	ignored.Add("a comment box's text").Graph.Comments[0].Text = "Probe";
	ignored.Add("a comment box's color").Graph.Comments[0].Color = glm::vec4(1.0f);
	ignored.Add("a comment box added").Graph.Comments.push_back({14, "More", {0.0f, 400.0f}, {200.0f, 100.0f}, glm::vec4(0.5f)});
	ignored.Add("the comment boxes removed").Graph.Comments.clear();
	ignored.Add("a function's comment boxes removed").Functions[0].Graph.Comments.clear();

	// What it reads, order and counts included.
	Changes compared{document, {}};
	compared.Add("a variable's ID").Variables[1].ID = 20;
	compared.Add("a variable's name").Variables[0].Name = "Velocity";
	compared.Add("a variable's type").Variables[1].Type = "double";
	compared.Add("a variable's value").Variables[0].Value = 3.0;
	compared.Add("a variable's value removed").Variables[0].Value = Json();
	compared.Add("a variable made private").Variables[0].Public = false;
	compared.Add("a variable's former name added").Variables[0].FormerNames.push_back("Rate");
	compared.Add("a variable's former names removed").Variables[0].FormerNames.clear();
	compared.Add("a variable added").Variables.push_back(ground);
	compared.Add("a variable removed").Variables.pop_back();
	std::ranges::reverse(compared.Add("the variables reordered").Variables);
	compared.Add("a function's ID").Functions[0].ID = 30;
	compared.Add("a function's name").Functions[0].Name = "Grow";
	compared.Add("a function made impure").Functions[0].Pure = false;
	compared.Add("a function input's name").Functions[0].Inputs[0].Name = "By";
	compared.Add("a function input's type").Functions[0].Inputs[0].Type = "int";
	compared.Add("a function output added").Functions[0].Outputs.push_back({"Twice", "float"});
	compared.Add("a local's value").Functions[0].Locals[0].Value = 3.0;
	compared.Add("a local's name").Functions[0].Locals[0].Name = "Double";
	compared.Add("a local removed").Functions[0].Locals.clear();
	compared.Add("a function removed").Functions.clear();
	compared.Add("a function's link removed").Functions[0].Graph.Links.clear();
	compared.Add("a function's node disabled").Functions[0].Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Disabled;
	compared.Add("a node's ID").Graph.Nodes[2].ID = 15;
	compared.Add("a node's type").Graph.Nodes[1].Type = "Call:Strada.Physics.SphereCast";
	compared.Add("a node's setting").Graph.Nodes[2].Settings["Variable"] = 1;
	compared.Add("a node's literal").Graph.Nodes[1].Inputs["MaxDistance"] = 50.0;
	compared.Add("a node's literal added").Graph.Nodes[1].Inputs["Origin.X"] = 1.0;
	compared.Add("a node's split input joined").Graph.Nodes[1].Split.Inputs.clear();
	compared.Add("a node's split output added").Graph.Nodes[1].Split.Outputs.push_back("Hit.Normal");
	compared.Add("a node enabled").Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Enabled;
	compared.Add("a node disabled").Graph.Nodes[0].Enabled = VisualScriptNodeEnabled::Disabled;
	compared.Add("a node added").Graph.Nodes.push_back(makeNode(14, "Event.Create"));
	compared.Add("a node removed").Graph.Nodes.pop_back();
	std::ranges::reverse(compared.Add("the nodes reordered").Graph.Nodes);
	compared.Add("a link's end").Graph.Links[2].From.Pin = "Hit.Point.X";
	compared.Add("a link added").Graph.Links.push_back(makeLink(12, "Then", 10, "Execute"));
	compared.Add("a link removed").Graph.Links.pop_back();

	for (Change const& change : ignored.Documents)
	{
		CAPTURE(change.What);
		REQUIRE(change.Document != document);
		CHECK(HaveSameVisualScriptProgram(document, change.Document));
		CHECK(HaveSameVisualScriptProgram(change.Document, document));
	}
	for (Change const& change : compared.Documents)
	{
		CAPTURE(change.What);
		REQUIRE(change.Document != document);
		CHECK_FALSE(HaveSameVisualScriptProgram(document, change.Document));
		CHECK_FALSE(HaveSameVisualScriptProgram(change.Document, document));
	}

	// Values the runtime reads differently, although operator== takes them as equal: the runtime reads the document's
	// text, in which they differ (an int literal or an ID written as 1.0 is no integer to it).
	Changes rewritten{document, {}};
	rewritten.Add("an ID setting written as a float").Graph.Nodes[2].Settings["Variable"] = 2.0;
	rewritten.Add("an integer literal written as a float").Graph.Nodes[1].Inputs["LayerMask"] = 1.0;
	rewritten.Add("a vector literal's component written as a float").Graph.Nodes[1].Inputs["Direction"][1] = -1.0;
	rewritten.Add("an unsigned literal written as the signed number of its bits").Graph.Nodes[1].Inputs["Seed"] = int64_t{-1};
	rewritten.Add("a variable's float value written as an integer").Variables[1].Value = 0;
	rewritten.Add("a variable's zero written as negative zero").Variables[1].Value = -0.0;
	rewritten.Add("a local's float value written as an integer").Functions[0].Locals[0].Value = 2;
	std::string const text = DumpJson(VisualScriptSerializer::SerializeDocument(document));
	for (Change const& change : rewritten.Documents)
	{
		CAPTURE(change.What);
		REQUIRE(change.Document == document);
		CHECK(DumpJson(VisualScriptSerializer::SerializeDocument(change.Document)) != text);
		CHECK_FALSE(HaveSameVisualScriptProgram(document, change.Document));
		CHECK_FALSE(HaveSameVisualScriptProgram(change.Document, document));
	}

	// Integers are alike whether they are stored signed or unsigned, as the loader stores those it reads: both are written
	// as the same digits.
	VisualScriptDocument unsignedId = document;
	unsignedId.Graph.Nodes[2].Settings["Variable"] = 2u;
	REQUIRE(unsignedId.Graph.Nodes[2].Settings.at("Variable").is_number_unsigned());
	REQUIRE_FALSE(document.Graph.Nodes[2].Settings.at("Variable").is_number_unsigned());
	CHECK(HaveSameVisualScriptProgram(document, unsignedId));
	CHECK(HaveSameVisualScriptProgram(unsignedId, document));
	Result<Json> const parsed = ParseJson(text);
	REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
	DeserializationContext const strict;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> const reloaded = VisualScriptSerializer::DeserializeDocument(parsed.GetValue(), strict, &issues);
	REQUIRE_MESSAGE(reloaded.IsOk(), reloaded.GetError());
	CHECK(issues.empty());
	CHECK(reloaded.GetValue().Graph.Nodes.at(1).Inputs.at("LayerMask").is_number_unsigned());
	CHECK(HaveSameVisualScriptProgram(document, reloaded.GetValue()));
	CHECK(HaveSameVisualScriptProgram(reloaded.GetValue(), document));

	// Everything the runtime never reads changed at once: still the same program.
	VisualScriptDocument relaidOut = document;
	relaidOut.Description.clear();
	relaidOut.Variables[0].Tooltip.clear();
	relaidOut.Variables[0].Range.reset();
	for (VisualScriptGraph* const graph : {&relaidOut.Graph, &relaidOut.Functions[0].Graph})
	{
		for (VisualScriptNode& node : graph->Nodes)
		{
			node.Position.reset();
			node.Comment = "Moved";
			node.Collapsed = !node.Collapsed;
		}
		graph->Comments.clear();
	}
	CHECK(HaveSameVisualScriptProgram(document, relaidOut));
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
	CHECK(GetAssetTypeForExtension(".sgraph") == AssetType::VisualScript);
	CHECK(GetAssetTypeForExtension(".txt") == AssetType::None);
	CHECK(GetAssetTypeForExtension("") == AssetType::None);
}
