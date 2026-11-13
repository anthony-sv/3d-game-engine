#include "TestUtilities.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

using namespace Strada;

namespace
{
	constexpr std::array<EngineContent, 5> AllContent = {EngineContent::Templates, EngineContent::Examples, EngineContent::Docs,
	                                                     EngineContent::AgentKit, EngineContent::MakeGameSkill};

#if defined(ST_PLATFORM_WINDOWS)
	constexpr std::string_view ToolName = "strada.exe";
#else
	constexpr std::string_view ToolName = "strada";
#endif

	void Write(std::filesystem::path const& file, std::string_view text)
	{
		REQUIRE(FileSystem::WriteTextFile(file, text).IsOk());
	}

	void MakeDirectory(std::filesystem::path const& directory)
	{
		REQUIRE(FileSystem::CreateDirectories(directory).IsOk());
	}

	bool IsSamePath(std::filesystem::path const& path, std::filesystem::path const& expected)
	{
		return path.lexically_normal() == expected.lexically_normal();
	}

	Json MakeManifest(std::string const& content)
	{
		Json manifest = Json::object();
		manifest["Strada"] = MakeFileHeader(EngineLayout::ManifestType, EngineLayout::ManifestVersion);
		manifest["Content"] = content;
		return manifest;
	}

	EngineLayout Resolve(std::filesystem::path const& executable)
	{
		Result<EngineLayout> layout = EngineLayout::Resolve(executable);
		REQUIRE_MESSAGE(layout.IsOk(), (layout ? std::string() : layout.GetError()));
		return layout.TakeValue();
	}

	// The right-hand column: the content in the resources directory.
	void CheckInstalledContent(EngineLayout const& layout, std::filesystem::path const& resources)
	{
		CHECK_FALSE(layout.IsSourceTree());
		CHECK(layout.GetSourceDirectory().empty());
		CHECK(IsSamePath(layout.GetResourcesDirectory(), resources));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::Templates), resources / "Templates"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::Examples), resources / "Examples"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::Docs), resources / "Docs"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::AgentKit), resources / "Agents"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::MakeGameSkill), resources / "Agents" / "skills" / "strada-make-game"));
		CHECK(IsSamePath(layout.GetExampleCatalogPath(), resources / "Examples" / "catalog.json"));
	}

	// The left-hand column: the content in the checkout engine.json names.
	void CheckSourceContent(EngineLayout const& layout, std::filesystem::path const& checkout)
	{
		CHECK(layout.IsSourceTree());
		CHECK(IsSamePath(layout.GetSourceDirectory(), checkout));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::Templates), checkout / "Templates"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::Examples), checkout / "Projects"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::Docs), checkout / "Docs"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::AgentKit), checkout / "Agents"));
		CHECK(IsSamePath(layout.GetContentPath(EngineContent::MakeGameSkill), checkout / ".claude" / "skills" / "strada-make-game"));
		CHECK(IsSamePath(layout.GetExampleCatalogPath(), checkout / "Projects" / "catalog.json"));
	}
}

TEST_CASE("EngineLayout: a build tree's marker names the checkout the content is read from")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const checkout = temporary.GetPath() / "Checkout";
	std::filesystem::path const bin = temporary.GetPath() / "Build" / "bin";
	MakeDirectory(checkout);
	MakeDirectory(bin);
	Write(bin / EngineLayout::ManifestFileName, DumpJson(MakeManifest(FileSystem::PathToUtf8(checkout))));

	EngineLayout const layout = Resolve(bin / "StradaEditor.exe");
	CHECK(layout.GetWarnings().empty());
	CHECK(IsSamePath(layout.GetExecutableDirectory(), bin));
	CHECK(IsSamePath(layout.GetResourcesDirectory(), bin));
	CheckSourceContent(layout, checkout);
	CHECK(IsSamePath(layout.GetToolPath(), bin / FileSystem::PathFromUtf8(ToolName)));

	// A trailing separator names the same checkout, and members this version does not know are ignored.
	Json manifest = MakeManifest(FileSystem::PathToUtf8(checkout) + "/");
	manifest["Generator"] = Json::object({{"Name", "CMake"}});
	Write(bin / EngineLayout::ManifestFileName, DumpJson(manifest));
	EngineLayout const trailing = Resolve(bin / "StradaEditor.exe");
	CHECK(trailing.GetWarnings().empty());
	CheckSourceContent(trailing, checkout);

	// The checkout under another spelling the file system takes is the checkout, spelled as the marker spells it.
	std::string spelled = FileSystem::PathToUtf8(checkout);
	spelled.back() = spelled.back() == 't' ? 'T' : 't';
	Write(bin / EngineLayout::ManifestFileName, DumpJson(MakeManifest(spelled)));
	EngineLayout const respelled = Resolve(bin / "StradaEditor.exe");
	if (FileSystem::IsDirectory(FileSystem::PathFromUtf8(spelled)))
	{
		CHECK(respelled.IsSourceTree());
		CHECK(FileSystem::GetStoredSpelling(respelled.GetSourceDirectory()) == FileSystem::GetStoredSpelling(checkout));
	}
	else
	{
		CheckInstalledContent(respelled, bin);
		CHECK(respelled.GetWarnings().size() == 1);
	}
}

TEST_CASE("EngineLayout: without a marker the content lies beside the executable")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const engine = temporary.GetPath() / "Strada";
	EngineLayout const layout = Resolve(engine / "StradaEditor");
	CHECK(layout.GetWarnings().empty());
	CHECK(IsSamePath(layout.GetExecutableDirectory(), engine));
	CheckInstalledContent(layout, engine);
	CHECK(IsSamePath(layout.GetToolPath(), engine / FileSystem::PathFromUtf8(ToolName)));

	// A marker without Content names no checkout, and is not damaged.
	MakeDirectory(engine);
	Json manifest = Json::object();
	manifest["Strada"] = MakeFileHeader(EngineLayout::ManifestType, EngineLayout::ManifestVersion);
	Write(engine / EngineLayout::ManifestFileName, DumpJson(manifest));
	EngineLayout const unnamed = Resolve(engine / "StradaEditor");
	CHECK(unnamed.GetWarnings().empty());
	CheckInstalledContent(unnamed, engine);
}

TEST_CASE("EngineLayout: in a macOS app bundle the content lies in Contents/Resources")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const contents = temporary.GetPath() / "Strada.app" / "Contents";
	std::filesystem::path const macOS = contents / "MacOS";
	MakeDirectory(macOS);

	// Without Info.plist the folder is no bundle: the content lies beside the executable.
	EngineLayout const plain = Resolve(macOS / "StradaEditor");
	CheckInstalledContent(plain, macOS);

	Write(contents / "Info.plist", "<plist/>");
	EngineLayout const bundle = Resolve(macOS / "StradaEditor");
	CHECK(bundle.GetWarnings().empty());
	CHECK(IsSamePath(bundle.GetExecutableDirectory(), macOS));
	CheckInstalledContent(bundle, contents / "Resources");
	CHECK(IsSamePath(bundle.GetToolPath(), macOS / FileSystem::PathFromUtf8(ToolName)));

	// The marker is read from the resources directory, not beside the executable.
	std::filesystem::path const checkout = temporary.GetPath() / "Checkout";
	MakeDirectory(checkout);
	Write(macOS / EngineLayout::ManifestFileName, DumpJson(MakeManifest(FileSystem::PathToUtf8(checkout))));
	CheckInstalledContent(Resolve(macOS / "StradaEditor"), contents / "Resources");
	Write(contents / "Resources" / EngineLayout::ManifestFileName, DumpJson(MakeManifest(FileSystem::PathToUtf8(checkout))));
	CheckSourceContent(Resolve(macOS / "StradaEditor"), checkout);
}

TEST_CASE("EngineLayout: a damaged, foreign or newer marker, or one naming no existing checkout, is ignored with one warning")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const bin = temporary.GetPath() / "bin";
	std::filesystem::path const checkout = temporary.GetPath() / "Checkout";
	MakeDirectory(bin);
	MakeDirectory(checkout);
	Write(temporary.GetPath() / "File.txt", "not a folder");
	std::string const checkoutText = FileSystem::PathToUtf8(checkout);

	Json foreign = MakeManifest(checkoutText);
	foreign["Strada"]["Type"] = "Project";
	Json newer = MakeManifest(checkoutText);
	newer["Strada"]["Version"] = EngineLayout::ManifestVersion + 1;
	Json headerless = Json::object();
	headerless["Content"] = checkoutText;
	Json number = MakeManifest(checkoutText);
	number["Content"] = 42;
	Json oversized = MakeManifest(checkoutText);
	oversized["Padding"] = std::string(static_cast<size_t>(EngineLayout::MaxManifestBytes), 'x');

	struct Case
	{
		char const* Name;
		std::string Text;
	};
	std::array<Case, 12> const cases = {{
		{"not JSON", "{ \"Strada\": "},
		{"another type", DumpJson(foreign)},
		{"a newer version", DumpJson(newer)},
		{"no header", DumpJson(headerless)},
		{"an array", "[1, 2, 3]"},
		{"Content of another kind", DumpJson(number)},
		{"an empty Content", DumpJson(MakeManifest(""))},
		{"a relative Content", DumpJson(MakeManifest("Checkout"))},
		{"a Content that does not exist", DumpJson(MakeManifest(FileSystem::PathToUtf8(temporary.GetPath() / "Missing")))},
		{"a Content that is a file", DumpJson(MakeManifest(FileSystem::PathToUtf8(temporary.GetPath() / "File.txt")))},
		{"a Content with a control character", DumpJson(MakeManifest(checkoutText + std::string(1, '\0') + "x"))},
		{"a marker larger than its limit", DumpJson(oversized)},
	}};
	for (Case const& damaged : cases)
	{
		CAPTURE(damaged.Name);
		Write(bin / EngineLayout::ManifestFileName, damaged.Text);
		EngineLayout const layout = Resolve(bin / "StradaEditor");
		CheckInstalledContent(layout, bin);
		REQUIRE(layout.GetWarnings().size() == 1);
		CHECK(layout.GetWarnings().front().find("engine.json") != std::string::npos);
	}

	// A folder named engine.json is no marker either.
	REQUIRE(FileSystem::Remove(bin / EngineLayout::ManifestFileName).IsOk());
	MakeDirectory(bin / EngineLayout::ManifestFileName);
	EngineLayout const folder = Resolve(bin / "StradaEditor");
	CheckInstalledContent(folder, bin);
	CHECK(folder.GetWarnings().size() == 1);
}

TEST_CASE("EngineLayout: only an empty path cannot be resolved, and an empty layout names no folder")
{
	CHECK_FALSE(EngineLayout::Resolve({}).IsOk());

	// A bare file name gives directories relative to the working directory, for whoever uses them to resolve.
	EngineLayout const bare = Resolve("StradaEditor");
	CHECK(bare.GetExecutableDirectory() == std::filesystem::path("."));
	CHECK(bare.GetResourcesDirectory() == std::filesystem::path("."));

	EngineLayout const empty;
	CHECK_FALSE(empty.IsSourceTree());
	CHECK(empty.GetExecutableDirectory().empty());
	CHECK(empty.GetResourcesDirectory().empty());
	for (EngineContent const content : AllContent)
	{
		CAPTURE(static_cast<int>(content));
		CHECK(empty.GetContentPath(content).empty());
	}
	CHECK(empty.GetExampleCatalogPath().empty());
	CHECK(empty.GetToolPath().empty());
	CHECK(empty.GetWarnings().empty());
}

TEST_CASE("EngineLayout: the tests run in a build tree whose content is this checkout")
{
	EngineLayout const& layout = EngineLayout::Get();
	CHECK(&layout == &EngineLayout::Get());
	CHECK(layout.GetWarnings().empty());
	REQUIRE(layout.IsSourceTree());
	CHECK(IsSamePath(layout.GetExecutableDirectory(), FileSystem::GetExecutableDirectory()));

	// CMake's marker, in the engine's file form.
	Result<std::string> text = FileSystem::ReadTextFile(layout.GetResourcesDirectory() / EngineLayout::ManifestFileName);
	REQUIRE(text.IsOk());
	Result<Json> manifest = ParseJson(text.GetValue());
	REQUIRE(manifest.IsOk());
	CHECK(ReadFileHeader(manifest.GetValue(), EngineLayout::ManifestType, EngineLayout::ManifestVersion).IsOk());
	CHECK(text.GetValue() == DumpJson(manifest.GetValue()));

	// The data directory's parents, not "Data/../..": lexically_normal() keeps a separator after a trailing "..".
	std::filesystem::path const checkout = FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR).parent_path().parent_path();
	CHECK(FileSystem::GetStoredSpelling(layout.GetSourceDirectory()) == FileSystem::GetStoredSpelling(checkout));
	std::filesystem::path const docs = layout.GetContentPath(EngineContent::Docs);
	CHECK(FileSystem::GetStoredSpelling(docs) == FileSystem::GetStoredSpelling(checkout / "Docs"));
	CHECK(FileSystem::IsRegularFile(docs / "Architecture.md"));
	CHECK(FileSystem::IsDirectory(layout.GetContentPath(EngineContent::Examples)));
	CHECK(FileSystem::IsRegularFile(layout.GetContentPath(EngineContent::MakeGameSkill) / "SKILL.md"));
}
