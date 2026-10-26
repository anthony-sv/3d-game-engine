#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Editor/RecentProjects.h"
#include "Editor/VisualScripting/VisualScriptView.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/UUID.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	std::filesystem::path GetViewsFixturePath()
	{
		return FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Editor" / "Views.json";
	}

	Json ReadJson(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		Result<Json> json = ParseJson(text.GetValue());
		REQUIRE_MESSAGE(json.IsOk(), json.GetError());
		return json.TakeValue();
	}

	void WriteJson(std::filesystem::path const& path, Json const& json)
	{
		REQUIRE(FileSystem::WriteTextFile(path, DumpJson(json)).IsOk());
	}

	std::set<std::string> GetMembers(Json const& object)
	{
		std::set<std::string> members;
		for (auto const& member : object.items())
		{
			members.insert(member.key());
		}
		return members;
	}

	// Loads the file into new settings, which must be what the file holds: no warning.
	VisualScriptViews LoadCleanly(std::filesystem::path const& file)
	{
		VisualScriptViewSettings settings(file);
		std::vector<std::string> const warnings = settings.Load();
		CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
		return settings.GetViews();
	}

	bool AnyContains(std::vector<std::string> const& warnings, std::string_view text)
	{
		return std::any_of(warnings.begin(), warnings.end(),
		                   [text](std::string const& warning)
		                   {
							   return warning.find(text) != std::string::npos;
						   });
	}

	VisualScriptViewState MakeView(uint32_t function, float x, float y, float zoom)
	{
		VisualScriptViewState view;
		view.Function = function;
		view.Center = {x, y};
		view.Zoom = zoom;
		return view;
	}

	AssetHandle MakeHandle(uint64_t value)
	{
		return AssetHandle(UUID(value));
	}
}

TEST_CASE("VisualScriptViews: every member round-trips through the file, and a file without Scroll gets the platform's mode")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "Editor" / "VisualScriptViews.json";
	REQUIRE(FileSystem::CreateDirectories(file.parent_path()).IsOk());
	REQUIRE(FileSystem::Copy(GetViewsFixturePath(), file, false).IsOk());

	VisualScriptViewSettings settings(file);
	std::vector<std::string> const warnings = settings.Load();
	CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
	VisualScriptViews const& views = settings.GetViews();
	CHECK(views.Panes == VisualScriptPaneSettings{18.5f, 22.25f, true, false, true});
	CHECK_FALSE(settings.IsContextPalette());
	CHECK(settings.IsReduceMotion());
	CHECK(settings.GetScrollMode() == VisualScriptScrollMode::Pan);
	CHECK_FALSE(settings.IsSnapToGrid());
	CHECK_FALSE(settings.IsMoveCommentContents());
	REQUIRE(settings.GetRecents().size() == 4);
	CHECK(settings.GetRecents()[0] == VisualScriptRecentNode{"Flow.Branch", Json::object()});
	CHECK(settings.GetRecents()[1] == VisualScriptRecentNode{"Literal", Json::object({{"Type", "float"}})});
	CHECK(settings.GetRecents()[3].Settings == Json::object({{"TypeArguments", Json::array({"Strada.RigidBodyComponent"})}}));
	CHECK(std::vector<std::string>(settings.GetRecentTypes().begin(), settings.GetRecentTypes().end()) ==
	      std::vector<std::string>{"float", "Strada.Vector3", "Strada.Entity"});
	CHECK(settings.GetCommentColor() == glm::vec4(0.25f, 0.5f, 0.75f, 0.375f));
	CHECK(settings.GetLastVariableType() == "Strada.Vector3");
	CHECK(settings.IsHintSeen("Canvas"));
	CHECK_FALSE(settings.IsHintSeen("Palette"));

	REQUIRE(views.Projects.size() == 2);
	VisualScriptProjectViews const& blocks = views.Projects[0];
	// Projects are keyed by their normalized path (absolute on every platform).
	CHECK(blocks.Project == RecentProjects::NormalizePath(FileSystem::PathFromUtf8("C:/Games/Blocks/Blocks.sproj")));
	CHECK(blocks.Open == std::vector<AssetHandle>{MakeHandle(8146390251172604461ull), MakeHandle(1202590842880147981ull)});
	REQUIRE(blocks.Graphs.size() == 2);
	VisualScriptGraphViews const& player = blocks.Graphs[0];
	CHECK(player.Graph == MakeHandle(8146390251172604461ull));
	CHECK(player.Shown == 12);
	CHECK(player.ContextEntity == UUID(3823112500970735607ull));
	CHECK(player.Views == std::vector<VisualScriptViewState>{MakeView(0, 120.0f, 40.0f, 0.75f), MakeView(12, -512.5f, 96.0f, 1.25f)});
	REQUIRE(player.FindView(12) != nullptr);
	CHECK(player.FindView(12)->Zoom == 1.25f);
	CHECK(player.FindView(3) == nullptr);
	REQUIRE(player.Bookmarks.size() == 2);
	CHECK(player.Bookmarks[1] == VisualScriptBookmark{9, "Spawn Waves", MakeView(12, -64.0f, 2048.0f, 0.5f)});
	REQUIRE(player.FindBookmark(1) != nullptr);
	CHECK(player.FindBookmark(1)->Name == "Scoring");
	CHECK(player.FindBookmark(2) == nullptr);
	CHECK(views.Projects[1].Project == RecentProjects::NormalizePath(FileSystem::PathFromUtf8("/home/player/Games/Orbit/Orbit.sproj")));
	CHECK(views.Projects[1].FindGraph(MakeHandle(6917529027641081857ull)) != nullptr);
	CHECK(views.Projects[1].FindGraph(MakeHandle(8146390251172604461ull)) == nullptr);

	// What loads saves and loads again, every member written.
	REQUIRE(settings.Save().IsOk());
	CHECK(LoadCleanly(file) == views);
	Json saved = ReadJson(file);
	CHECK(GetMembers(saved["VisualScriptViews"]) == GetMembers(ReadJson(GetViewsFixturePath())["VisualScriptViews"]));
	CHECK(saved["VisualScriptViews"]["Scroll"] == "Pan");

	// Without Scroll the mode is the platform's: Pan on macOS, Zoom elsewhere; saving keeps it unset.
	saved["VisualScriptViews"].erase("Scroll");
	WriteJson(file, saved);
	REQUIRE(settings.Load().empty());
	CHECK_FALSE(settings.GetViews().Scroll.has_value());
	CHECK(settings.GetScrollMode() == GetDefaultVisualScriptScrollMode());
#if defined(ST_PLATFORM_MACOS)
	CHECK(GetDefaultVisualScriptScrollMode() == VisualScriptScrollMode::Pan);
#else
	CHECK(GetDefaultVisualScriptScrollMode() == VisualScriptScrollMode::Zoom);
#endif
	REQUIRE(settings.Save().IsOk());
	CHECK_FALSE(ReadJson(file)["VisualScriptViews"].contains("Scroll"));
	settings.SetScrollMode(VisualScriptScrollMode::Zoom);
	REQUIRE(settings.Save().IsOk());
	CHECK(ReadJson(file)["VisualScriptViews"]["Scroll"] == "Zoom");
	CHECK(LoadCleanly(file) == settings.GetViews());
}

TEST_CASE("VisualScriptViews: without a file the defaults hold, and saving creates the file and its directory")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "UserData" / "Editor" / "VisualScriptViews.json";
	VisualScriptViewSettings settings(file);
	CHECK(settings.GetFile() == file);
	CHECK(settings.Load().empty());
	CHECK(settings.GetViews() == VisualScriptViews());
	CHECK(settings.GetPanes() == VisualScriptPaneSettings{16.0f, 20.0f, true, true, true});
	CHECK(settings.IsContextPalette());
	CHECK_FALSE(settings.IsReduceMotion());
	CHECK(settings.GetScrollMode() == GetDefaultVisualScriptScrollMode());
	CHECK(settings.IsSnapToGrid());
	CHECK(settings.IsMoveCommentContents());
	CHECK(settings.GetRecents().empty());
	CHECK(settings.GetRecentTypes().empty());
	CHECK(settings.GetCommentColor() == VisualScriptComment().Color);
	CHECK(settings.GetLastVariableType() == "float");
	CHECK(settings.GetProject().empty());
	CHECK(settings.GetOpenGraphs().empty());

	REQUIRE(settings.Save().IsOk());
	REQUIRE(FileSystem::Exists(file));
	Json const saved = ReadJson(file);
	Result<int> const version = ReadFileHeader(saved, "VisualScriptViews", VisualScriptViewSettings::FormatVersion);
	REQUIRE_MESSAGE(version.IsOk(), version.GetError());
	// The defaults are written, except the scroll mode and the comment colour, which then stay the platform's and the
	// format's.
	CHECK(GetMembers(saved["VisualScriptViews"]) == std::set<std::string>{"Panes", "ContextPalette", "ReduceMotion", "SnapToGrid",
	                                                                      "MoveCommentContents", "Recents", "RecentTypes",
	                                                                      "LastVariableType", "HintsSeen", "Projects"});
	CHECK(LoadCleanly(file) == VisualScriptViews());
}

TEST_CASE("VisualScriptViews: a file that is not a views file of this version loads as the defaults with one warning")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "VisualScriptViews.json";
	Json const fixture = ReadJson(GetViewsFixturePath());

	auto const checkIgnored = [&file](std::string_view text, std::string_view reason)
	{
		CAPTURE(text);
		REQUIRE(FileSystem::WriteTextFile(file, text).IsOk());
		VisualScriptViewSettings settings(file);
		settings.SetProject(file.parent_path() / "Game.sproj");
		std::vector<std::string> const warnings = settings.Load();
		REQUIRE(warnings.size() == 1);
		CHECK(warnings[0].find(FileSystem::PathToUtf8(file)) != std::string::npos);
		CHECK(warnings[0].find(reason) != std::string::npos);
		CHECK(warnings[0].find("the file is ignored") != std::string::npos);
		// The defaults, and the current project stays current.
		REQUIRE(settings.GetViews().Projects.size() == 1);
		CHECK(settings.GetViews().Projects[0].Project == settings.GetProject());
		VisualScriptViews expected;
		expected.Projects = settings.GetViews().Projects;
		CHECK(settings.GetViews() == expected);
	};
	checkIgnored("{ \"Strada\": ", "not valid JSON");
	checkIgnored("[]", "must be a JSON object");

	Json other = fixture;
	other["Strada"]["Type"] = "RecentProjects";
	checkIgnored(DumpJson(other), "expected a VisualScriptViews file but found 'RecentProjects'");
	Json newer = fixture;
	newer["Strada"]["Version"] = VisualScriptViewSettings::FormatVersion + 1;
	checkIgnored(DumpJson(newer), "newer than the supported version");
	Json headless = fixture;
	headless.erase("Strada");
	checkIgnored(DumpJson(headless), "missing \"Strada\" file header");
	Json empty = fixture;
	empty["VisualScriptViews"] = Json::array();
	checkIgnored(DumpJson(empty), "expected a \"VisualScriptViews\" object");
}

TEST_CASE("VisualScriptViews: unknown members are ignored, and invalid entries are skipped or clamped, each with a warning")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "VisualScriptViews.json";
	Json const fixture = ReadJson(GetViewsFixturePath());
	VisualScriptViews const expected = [&]
	{
		WriteJson(file, fixture);
		return LoadCleanly(file);
	}();

	// Members a later version may add are ignored at every level.
	Json extended = fixture;
	Json& content = extended["VisualScriptViews"];
	content["Layout"] = Json::object({{"Docked", true}});
	content["Panes"]["Drawer"] = 10.0;
	content["Recents"][0]["Pinned"] = true;
	content["Projects"][0]["Theme"] = "Dark";
	content["Projects"][0]["Graphs"][0]["Folded"] = Json::array({1, 2});
	content["Projects"][0]["Graphs"][0]["Views"][0]["Rotation"] = 0.5;
	content["Projects"][0]["Graphs"][0]["Bookmarks"][0]["Color"] = "Red";
	WriteJson(file, extended);
	CHECK(LoadCleanly(file) == expected);

	// Damage of every kind.
	Json damaged = fixture;
	Json& views = damaged["VisualScriptViews"];
	views["Panes"]["Sidebar"] = 3.0;
	views["Panes"]["ShowMinimap"] = "yes";
	views["ReduceMotion"] = 1;
	views["Scroll"] = "Sideways";
	Json const firstRecent = views["Recents"][0];
	views["Recents"].push_back(firstRecent);
	views["Recents"].push_back(Json::object({{"Type", ""}}));
	views["Recents"].push_back(Json::object({{"Type", "Flow.Delay"}, {"Settings", Json::array()}}));
	views["RecentTypes"] = Json::array({"float", "", "float", 7, "Strada.Quaternion"});
	views["CommentColor"] = Json::array({2.0, 0.0, 0.0, 1.0});
	views["LastVariableType"] = "line\nbreak";
	views["HintsSeen"] = "Canvas";
	Json& projects = views["Projects"];
	projects.push_back(Json::object({{"Project", 5}}));
	Json const orbit = projects[1];
	projects.push_back(orbit);
	Json& graphs = projects[0]["Graphs"];
	graphs[0]["Shown"] = -1;
	graphs[0]["ContextEntity"] = "abc";
	graphs[1]["ContextEntity"] = "0";
	Json const firstView = graphs[0]["Views"][0];
	graphs[0]["Views"][0]["Zoom"] = 9.0;
	graphs[0]["Views"][1]["Center"] = Json::array({2.0e7, -3.0e7});
	graphs[0]["Views"].push_back(firstView);
	graphs[0]["Views"].push_back(Json::object({{"Function", 3}, {"Center", Json::array({1.0})}, {"Zoom", 1.0}}));
	graphs[0]["Views"].push_back(Json::object({{"Function", -2}, {"Center", Json::array({0.0, 0.0})}, {"Zoom", 1.0}}));
	graphs[0]["Bookmarks"].push_back(
		Json::object({{"Slot", 12}, {"Name", "Far"}, {"Function", 0}, {"Center", Json::array({0, 0})}, {"Zoom", 1}}));
	graphs[0]["Bookmarks"].push_back(
		Json::object({{"Slot", 9}, {"Name", "Again"}, {"Function", 0}, {"Center", Json::array({0, 0})}, {"Zoom", 1}}));
	graphs[0]["Bookmarks"].push_back(
		Json::object({{"Slot", 5}, {"Name", "Two\nLines"}, {"Function", 0}, {"Center", Json::array({0, 0})}, {"Zoom", 1}}));
	graphs[0]["Bookmarks"].push_back(
		Json::object({{"Slot", 6}, {"Name", ""}, {"Function", 0}, {"Center", Json::array({0, 0})}, {"Zoom", 1}}));
	graphs[0]["Bookmarks"].push_back(Json::object({{"Slot", 7}, {"Function", 0}, {"Center", Json::array({0, 0})}, {"Zoom", 1}}));
	Json const player = graphs[0];
	graphs.push_back(Json::object({{"Shown", 0}}));
	graphs.push_back(player);
	Json const firstOpen = projects[0]["Open"][0];
	projects[0]["Open"].push_back("0");
	projects[0]["Open"].push_back(firstOpen);
	WriteJson(file, damaged);

	VisualScriptViewSettings settings(file);
	std::vector<std::string> const warnings = settings.Load();
	CAPTURE(warnings.size());
	for (std::string_view const expectedWarning :
	     {"Panes.Sidebar: outside 10 to 1000; clamped",
	      "Panes.ShowMinimap: expected true or false; true is used",
	      "ReduceMotion: expected true or false; false is used",
	      "Scroll: expected \"Zoom\" or \"Pan\"; the platform's default is used",
	      "Recents[4]: repeats an earlier entry; skipped",
	      "Recents[5]: expected an object with a node Type; skipped",
	      "Recents[6].Settings: expected an object; the entry is skipped",
	      "RecentTypes[1]: expected text of 1 to 256 bytes",
	      "RecentTypes[2]: repeats an earlier entry; skipped",
	      "RecentTypes[3]: expected text",
	      "CommentColor: expected four numbers from 0 to 1; the default comment colour is used",
	      "LastVariableType: expected a type ID; \"float\" is used",
	      "HintsSeen: expected an array; ignored",
	      "Projects[2]: expected an object with the Project's file path; skipped",
	      "Projects[3]: repeats an earlier project; skipped",
	      "Projects[0].Open[2]: expected a graph's handle as a decimal string; skipped",
	      "Projects[0].Open[3]: repeats an earlier graph; skipped",
	      "Projects[0].Graphs[0].Shown: expected 0 or a function's ID; the event graph is used",
	      "Projects[0].Graphs[0].ContextEntity: expected an entity ID as a decimal string; none is used",
	      "Projects[0].Graphs[0].Views[0].Zoom: outside 0.1 to 2; clamped",
	      "Projects[0].Graphs[0].Views[1].Center: outside -10000000 to 10000000; clamped",
	      "Projects[0].Graphs[0].Views[2]: sub-graph 0 has an earlier view; skipped",
	      "Projects[0].Graphs[0].Views[3].Center: expected two numbers; the view is skipped",
	      "Projects[0].Graphs[0].Views[4].Function: expected 0 or a function's ID; the view is skipped",
	      "Projects[0].Graphs[0].Bookmarks[2].Slot: expected a slot from 1 to 9; skipped",
	      "Projects[0].Graphs[0].Bookmarks[3]: slot 9 has an earlier bookmark; skipped",
	      "Projects[0].Graphs[0].Bookmarks[4].Name: not a name of 1 to 64 bytes on one line; \"Two\" is used",
	      "Projects[0].Graphs[0].Bookmarks[5].Name: not a name of 1 to 64 bytes on one line; \"Bookmark 6\" is used",
	      "Projects[0].Graphs[0].Bookmarks[6].Name: expected a name; skipped",
	      "Projects[0].Graphs[2]: expected an object with the Graph's handle as a decimal string; skipped",
	      "Projects[0].Graphs[3]: repeats an earlier graph; skipped"})
	{
		CAPTURE(expectedWarning);
		CHECK(AnyContains(warnings, expectedWarning));
	}
	CHECK(warnings.size() == 31);
	// "0" is the usual form of no entity.
	CHECK_FALSE(AnyContains(warnings, "Graphs[1].ContextEntity"));

	VisualScriptViews const& loaded = settings.GetViews();
	CHECK(loaded.Panes.Sidebar == VisualScriptViewSettings::MinSidebarWidth);
	CHECK(loaded.Panes.ShowMinimap);
	CHECK_FALSE(loaded.ReduceMotion);
	CHECK_FALSE(loaded.Scroll.has_value());
	CHECK(loaded.Recents == expected.Recents);
	CHECK(loaded.RecentTypes == std::vector<std::string>{"float", "Strada.Quaternion"});
	CHECK_FALSE(loaded.CommentColor.has_value());
	CHECK(loaded.LastVariableType == "float");
	CHECK(loaded.HintsSeen.empty());
	REQUIRE(loaded.Projects.size() == 2);
	CHECK(loaded.Projects[0].Open == expected.Projects[0].Open);
	REQUIRE(loaded.Projects[0].Graphs.size() == 2);
	VisualScriptGraphViews const& repaired = loaded.Projects[0].Graphs[0];
	CHECK(repaired.Shown == 0);
	CHECK_FALSE(repaired.ContextEntity.IsValid());
	CHECK_FALSE(loaded.Projects[0].Graphs[1].ContextEntity.IsValid());
	CHECK(repaired.Views == std::vector<VisualScriptViewState>{MakeView(0, 120.0f, 40.0f, 2.0f), MakeView(12, 1.0e7f, -1.0e7f, 1.25f)});
	REQUIRE(repaired.Bookmarks.size() == 4);
	CHECK(repaired.Bookmarks[0].Slot == 1);
	CHECK(repaired.Bookmarks[1] == VisualScriptBookmark{5, "Two", MakeView(0, 0.0f, 0.0f, 1.0f)});
	CHECK(repaired.Bookmarks[2] == VisualScriptBookmark{6, "Bookmark 6", MakeView(0, 0.0f, 0.0f, 1.0f)});
	CHECK(repaired.Bookmarks[3].Name == "Spawn Waves");

	// What was repaired is written as it loaded: the file then loads without a warning.
	REQUIRE(settings.Save().IsOk());
	CHECK(LoadCleanly(file) == loaded);
}

TEST_CASE("VisualScriptViews: the limits keep the most recently used projects, graphs and views, and the newest recents, types and hints")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "VisualScriptViews.json";
	VisualScriptViewSettings settings(file);
	REQUIRE(settings.Load().empty());
	auto const project = [&temporary](size_t index)
	{
		return temporary.GetPath() / fmt::format("Project{}", index) / "Game.sproj";
	};

	// Projects: the 32 used last, most recent first; using one again moves it to the front.
	for (size_t index = 0; index < 40; index++)
	{
		settings.SetProject(project(index));
		REQUIRE(settings.SetView(MakeHandle(1000 + index), MakeView(0, 1.0f, 2.0f, 1.0f)));
	}
	std::vector<VisualScriptProjectViews> const& projects = settings.GetViews().Projects;
	REQUIRE(projects.size() == VisualScriptViewSettings::MaxProjects);
	CHECK(projects.front().Project == RecentProjects::NormalizePath(project(39)));
	CHECK(projects.back().Project == RecentProjects::NormalizePath(project(8)));
	settings.SetProject(project(20));
	CHECK(projects.front().Project == RecentProjects::NormalizePath(project(20)));
	CHECK(projects[1].Project == RecentProjects::NormalizePath(project(39)));
	CHECK(settings.FindGraph(MakeHandle(1020)) != nullptr);
	// A project dropped earlier starts over.
	settings.SetProject(project(0));
	CHECK(projects.size() == VisualScriptViewSettings::MaxProjects);
	CHECK(settings.FindGraph(MakeHandle(1000)) == nullptr);
	CHECK(projects.back().Project == RecentProjects::NormalizePath(project(9)));

	// Graphs: the 256 used last in the project.
	for (uint64_t graph = 1; graph <= 300; graph++)
	{
		settings.SetShown(MakeHandle(graph), 0);
	}
	std::vector<VisualScriptGraphViews> const& graphs = projects.front().Graphs;
	REQUIRE(graphs.size() == VisualScriptViewSettings::MaxGraphsPerProject);
	CHECK(graphs.front().Graph == MakeHandle(300));
	CHECK(graphs.back().Graph == MakeHandle(45));
	CHECK(settings.FindGraph(MakeHandle(44)) == nullptr);
	REQUIRE(settings.SetView(MakeHandle(45), MakeView(0, 0.0f, 0.0f, 1.0f)));
	CHECK(graphs.front().Graph == MakeHandle(45));

	// Views: the 64 sub-graphs shown last; showing one again moves its view to the front.
	AssetHandle const graph = MakeHandle(7);
	for (uint32_t function = 0; function < 70; function++)
	{
		REQUIRE(settings.SetView(graph, MakeView(function, static_cast<float>(function), 0.0f, 1.0f)));
	}
	VisualScriptGraphViews const* views = settings.FindGraph(graph);
	REQUIRE(views != nullptr);
	REQUIRE(views->Views.size() == VisualScriptViewSettings::MaxViewsPerGraph);
	CHECK(views->Views.front().Function == 69);
	CHECK(views->Views.back().Function == 6);
	CHECK(views->FindView(5) == nullptr);
	REQUIRE(settings.SetView(graph, MakeView(30, -4.0f, 8.0f, 0.5f)));
	CHECK(views->Views.front() == MakeView(30, -4.0f, 8.0f, 0.5f));
	CHECK(views->Views.size() == VisualScriptViewSettings::MaxViewsPerGraph);

	// Bookmarks: slots 1 to 9, in slot order, names on one line of at most 64 bytes.
	CHECK_FALSE(settings.SetBookmark(graph, {0, "Zero", MakeView(0, 0.0f, 0.0f, 1.0f)}));
	CHECK_FALSE(settings.SetBookmark(graph, {10, "Ten", MakeView(0, 0.0f, 0.0f, 1.0f)}));
	std::string longName;
	for (int character = 0; character < 40; character++)
	{
		longName += "\xC3\xA9";
	}
	REQUIRE(settings.SetBookmark(graph, {4, longName, MakeView(2, 10.0f, 20.0f, 1.0f)}));
	REQUIRE(settings.SetBookmark(graph, {2, "Doors\nand windows", MakeView(0, 1.0f, 1.0f, 1.0f)}));
	REQUIRE(settings.SetBookmark(graph, {8, "", MakeView(0, 1.0f, 1.0f, 1.0f)}));
	REQUIRE(views->Bookmarks.size() == 3);
	CHECK(views->Bookmarks[0].Name == "Doors");
	CHECK(views->Bookmarks[1].Slot == 4);
	CHECK(views->Bookmarks[1].Name == longName.substr(0, 64));
	CHECK(views->Bookmarks[2].Name == "Bookmark 8");
	REQUIRE(settings.SetBookmark(graph, {4, "Lift", MakeView(3, 0.0f, 0.0f, 1.5f)}));
	CHECK(views->FindBookmark(4)->Name == "Lift");
	CHECK(views->Bookmarks.size() == 3);
	settings.RemoveBookmark(graph, 2);
	settings.RemoveBookmark(graph, 5);
	CHECK(views->FindBookmark(2) == nullptr);
	CHECK(views->Bookmarks.size() == 2);

	// Recents, recent types and hints: the newest first, an entry again moves to the front.
	for (int index = 0; index < 25; index++)
	{
		REQUIRE(settings.AddRecent({"Literal", Json::object({{"Type", fmt::format("Type{}", index)}})}));
		REQUIRE(settings.AddRecentType(fmt::format("Type{}", index)));
	}
	REQUIRE(settings.GetRecents().size() == VisualScriptViewSettings::MaxRecents);
	CHECK(settings.GetRecents().front().Settings["Type"] == "Type24");
	CHECK(settings.GetRecents().back().Settings["Type"] == "Type5");
	REQUIRE(settings.AddRecent({"Literal", Json::object({{"Type", "Type10"}})}));
	CHECK(settings.GetRecents().front().Settings["Type"] == "Type10");
	CHECK(settings.GetRecents().size() == VisualScriptViewSettings::MaxRecents);
	CHECK_FALSE(settings.AddRecent({"", Json::object()}));
	CHECK_FALSE(settings.AddRecent({"Flow.Branch", Json::array()}));
	REQUIRE(settings.GetRecentTypes().size() == VisualScriptViewSettings::MaxRecentTypes);
	CHECK(settings.GetRecentTypes().front() == "Type24");
	CHECK(settings.GetRecentTypes().back() == "Type15");
	REQUIRE(settings.AddRecentType("Type20"));
	CHECK(settings.GetRecentTypes().front() == "Type20");
	CHECK(settings.GetRecentTypes().size() == VisualScriptViewSettings::MaxRecentTypes);
	CHECK_FALSE(settings.AddRecentType(std::string(300, 'x')));
	for (int index = 0; index < 20; index++)
	{
		REQUIRE(settings.SetHintSeen(fmt::format("Hint{}", index)));
	}
	REQUIRE(settings.SetHintSeen("Hint19"));
	CHECK(settings.GetViews().HintsSeen.size() == VisualScriptViewSettings::MaxHints);
	CHECK(settings.IsHintSeen("Hint19"));
	CHECK(settings.IsHintSeen("Hint4"));
	CHECK_FALSE(settings.IsHintSeen("Hint3"));
	CHECK_FALSE(settings.SetHintSeen(""));
	CHECK_FALSE(settings.SetHintSeen(std::string(65, 'h')));

	// All of it saves and loads again.
	REQUIRE(settings.Save().IsOk());
	CHECK(LoadCleanly(file) == settings.GetViews());

	// A file beyond the limits keeps the most recent entries, saying what it dropped.
	Json crowded = VisualScriptViewSettings::Serialize(VisualScriptViews());
	for (size_t index = 0; index < 40; index++)
	{
		crowded["VisualScriptViews"]["Projects"].push_back(
			Json::object({{"Project", FileSystem::PathToUtf8(RecentProjects::NormalizePath(project(index)))},
		                  {"Open", Json::array()},
		                  {"Graphs", Json::array()}}));
	}
	for (int index = 0; index < 30; index++)
	{
		crowded["VisualScriptViews"]["HintsSeen"].push_back(fmt::format("Hint{}", index));
	}
	WriteJson(file, crowded);
	settings.SetProject(project(5));
	std::vector<std::string> const warnings = settings.Load();
	CHECK(warnings.size() == 2);
	CHECK(AnyContains(warnings, "Projects: more than 32 entries; the rest are dropped"));
	CHECK(AnyContains(warnings, "HintsSeen: more than 16 entries; the rest are dropped"));
	std::vector<VisualScriptProjectViews> const& loaded = settings.GetViews().Projects;
	REQUIRE(loaded.size() == VisualScriptViewSettings::MaxProjects);
	// The project current before the load is current again, moved to the front; the others keep the file's order.
	CHECK(settings.GetProject() == RecentProjects::NormalizePath(project(5)));
	CHECK(loaded[0].Project == RecentProjects::NormalizePath(project(5)));
	CHECK(loaded[1].Project == RecentProjects::NormalizePath(project(0)));
	CHECK(loaded[5].Project == RecentProjects::NormalizePath(project(4)));
	CHECK(loaded[6].Project == RecentProjects::NormalizePath(project(6)));
	CHECK(loaded.back().Project == RecentProjects::NormalizePath(project(31)));
	CHECK(settings.GetViews().HintsSeen.back() == "Hint15");
}

TEST_CASE("VisualScriptViews: graphs are kept per project, keyed by the project file's normalized path")
{
	Testing::TemporaryDirectory temporary;
	VisualScriptViewSettings settings(temporary.GetPath() / "VisualScriptViews.json");
	std::filesystem::path const blocks = temporary.GetPath() / "Blocks" / "Blocks.sproj";
	std::filesystem::path const orbit = temporary.GetPath() / "Orbit" / "Orbit.sproj";
	AssetHandle const player = MakeHandle(8146390251172604461ull);
	AssetHandle const door = MakeHandle(1202590842880147981ull);

	// Paths that name one file normalize to one form.
	CHECK(RecentProjects::NormalizePath(temporary.GetPath() / "Blocks" / "Sub" / ".." / "Blocks.sproj") ==
	      RecentProjects::NormalizePath(blocks));
	CHECK(RecentProjects::NormalizePath(blocks).is_absolute());
	REQUIRE(FileSystem::WriteTextFile(blocks, "{}").IsOk());
	CHECK(RecentProjects::NormalizePath(blocks) == RecentProjects::NormalizePath(temporary.GetPath() / "Blocks" / "." / "Blocks.sproj"));
	// Where file systems ignore letter case, an existing file's names are spelled as stored.
	if (Testing::IgnoresLetterCase(temporary.GetPath()))
	{
		CHECK(RecentProjects::NormalizePath(temporary.GetPath() / "BLOCKS" / "blocks.SPROJ") == RecentProjects::NormalizePath(blocks));
	}

	// Without a project nothing is kept per graph.
	CHECK_FALSE(settings.SetView(player, MakeView(0, 0.0f, 0.0f, 1.0f)));
	settings.SetShown(player, 3);
	settings.SetOpenGraphs(std::vector<AssetHandle>{player});
	CHECK(settings.FindGraph(player) == nullptr);
	CHECK(settings.GetOpenGraphs().empty());
	CHECK(settings.GetViews().Projects.empty());

	settings.SetProject(temporary.GetPath() / "Blocks" / "Sub" / ".." / "Blocks.sproj");
	CHECK(settings.GetProject() == RecentProjects::NormalizePath(blocks));
	REQUIRE(settings.SetView(player, MakeView(0, 100.0f, -50.0f, 0.5f)));
	// Views are clamped into their ranges; one that is not finite is not kept.
	REQUIRE(settings.SetView(player, MakeView(4, 3.0e7f, -3.0e7f, 0.01f)));
	CHECK_FALSE(settings.SetView(player, MakeView(5, std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f)));
	CHECK_FALSE(settings.SetView(player, MakeView(5, 0.0f, 0.0f, std::numeric_limits<float>::infinity())));
	CHECK_FALSE(settings.SetView(AssetHandle(), MakeView(0, 0.0f, 0.0f, 1.0f)));
	CHECK_FALSE(settings.SetBookmark(player, {1, "Start", MakeView(0, std::numeric_limits<float>::infinity(), 0.0f, 1.0f)}));
	REQUIRE(settings.SetBookmark(player, {1, "Start", MakeView(0, 0.0f, 0.0f, 9.0f)}));
	settings.SetShown(player, 4);
	settings.SetContextEntity(player, UUID(77));
	settings.SetContextEntity(door, UUID(78));
	VisualScriptGraphViews const* views = settings.FindGraph(player);
	REQUIRE(views != nullptr);
	CHECK(views->Shown == 4);
	CHECK(views->ContextEntity == UUID(77));
	CHECK(views->Views ==
	      std::vector<VisualScriptViewState>{MakeView(4, 1.0e7f, -1.0e7f, VisualScriptMinZoom), MakeView(0, 100.0f, -50.0f, 0.5f)});
	CHECK(views->FindBookmark(1)->View.Zoom == VisualScriptMaxZoom);
	CHECK(settings.GetViews().Projects.front().Graphs.front().Graph == door);

	// The open windows keep their tab order, without repeats or invalid handles.
	settings.SetOpenGraphs(std::vector<AssetHandle>{door, AssetHandle(), player, door});
	CHECK(std::vector<AssetHandle>(settings.GetOpenGraphs().begin(), settings.GetOpenGraphs().end()) ==
	      std::vector<AssetHandle>{door, player});

	// Another project keeps its own graphs.
	settings.SetProject(orbit);
	CHECK(settings.FindGraph(player) == nullptr);
	CHECK(settings.GetOpenGraphs().empty());
	settings.SetOpenGraphs(std::vector<AssetHandle>{player});
	settings.SetProject(blocks);
	REQUIRE(settings.FindGraph(player) != nullptr);
	CHECK(settings.FindGraph(player)->Shown == 4);
	CHECK(settings.GetOpenGraphs().size() == 2);
	CHECK(settings.GetViews().Projects.size() == 2);

	// A deleted graph is forgotten, its open window too.
	settings.ForgetGraph(player);
	CHECK(settings.FindGraph(player) == nullptr);
	CHECK(std::vector<AssetHandle>(settings.GetOpenGraphs().begin(), settings.GetOpenGraphs().end()) == std::vector<AssetHandle>{door});
	settings.SetProject(orbit);
	CHECK(settings.GetOpenGraphs().size() == 1);

	// Settings shared by every window keep what is valid.
	settings.SetPanes({4.0f, 2000.0f, false, true, false});
	CHECK(settings.GetPanes() ==
	      VisualScriptPaneSettings{VisualScriptViewSettings::MinSidebarWidth, VisualScriptViewSettings::MaxPaneWidth, false, true, false});
	settings.SetPanes({std::numeric_limits<float>::quiet_NaN(), 30.0f, true, true, true});
	CHECK(settings.GetPanes() == VisualScriptPaneSettings{VisualScriptViewSettings::MinSidebarWidth, 30.0f, true, true, true});
	CHECK(settings.SetCommentColor({0.1f, 0.2f, 0.3f, 0.4f}));
	CHECK_FALSE(settings.SetCommentColor({1.5f, 0.2f, 0.3f, 0.4f}));
	CHECK_FALSE(settings.SetCommentColor({std::numeric_limits<float>::quiet_NaN(), 0.2f, 0.3f, 0.4f}));
	CHECK(settings.GetCommentColor() == glm::vec4(0.1f, 0.2f, 0.3f, 0.4f));
	CHECK(settings.SetLastVariableType("Strada.Entity"));
	CHECK_FALSE(settings.SetLastVariableType(""));
	CHECK_FALSE(settings.SetLastVariableType("two\nlines"));
	CHECK(settings.GetLastVariableType() == "Strada.Entity");
	settings.SetContextPalette(false);
	settings.SetReduceMotion(true);
	settings.SetSnapToGrid(false);
	settings.SetMoveCommentContents(false);
	CHECK_FALSE(settings.IsContextPalette());
	CHECK(settings.IsReduceMotion());
	CHECK_FALSE(settings.IsSnapToGrid());
	CHECK_FALSE(settings.IsMoveCommentContents());

	// Without a project again, nothing is found.
	settings.SetProject({});
	CHECK(settings.GetProject().empty());
	CHECK(settings.FindGraph(door) == nullptr);
	REQUIRE(settings.Save().IsOk());
	CHECK(LoadCleanly(settings.GetFile()) == settings.GetViews());
}

TEST_CASE("VisualScriptViews: a project's entry is found by any spelling of its file, also after a rename of its directory's letter case")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const file = temporary.GetPath() / "VisualScriptViews.json";
	std::filesystem::path const game = temporary.GetPath() / "Game" / "Game.sproj";
	REQUIRE(FileSystem::WriteTextFile(game, "{}").IsOk());
	AssetHandle const player = MakeHandle(8146390251172604461ull);
	AssetHandle const door = MakeHandle(1202590842880147981ull);

	// A hand-written file names the project with a "." part, and again as it is spelled: one project, the later entry
	// skipped with a warning.
	VisualScriptViews written;
	VisualScriptProjectViews dotted;
	dotted.Project = temporary.GetPath() / "Game" / "." / "Game.sproj";
	dotted.Open = {player};
	VisualScriptProjectViews plain;
	plain.Project = game;
	plain.Open = {door};
	written.Projects = {dotted, plain};
	WriteJson(file, VisualScriptViewSettings::Serialize(written));
	VisualScriptViewSettings settings(file);
	std::vector<std::string> const warnings = settings.Load();
	CHECK(warnings.size() == 1);
	CHECK(AnyContains(warnings, "is the file of an earlier project; skipped"));
	REQUIRE(settings.GetViews().Projects.size() == 1);
	CHECK(settings.GetViews().Projects.front().Project == RecentProjects::NormalizePath(game));
	settings.SetProject(game);
	CHECK(settings.GetViews().Projects.size() == 1);
	CHECK(std::vector<AssetHandle>(settings.GetOpenGraphs().begin(), settings.GetOpenGraphs().end()) == std::vector<AssetHandle>{player});
	REQUIRE(settings.Save().IsOk());
	CHECK(LoadCleanly(file) == settings.GetViews());

	// Where file systems ignore letter case, the project's directory renamed in letter case only is the same project: its
	// entry, kept under the old spelling, is found by the new one.
	if (Testing::IgnoresLetterCase(temporary.GetPath()))
	{
		std::error_code error;
		std::filesystem::rename(temporary.GetPath() / "Game", temporary.GetPath() / "game", error);
		REQUIRE_MESSAGE(!error, error.message());
		std::filesystem::path const renamed = temporary.GetPath() / "game" / "Game.sproj";
		VisualScriptViewSettings reloaded(file);
		CHECK(reloaded.Load().empty());
		reloaded.SetProject(renamed);
		CHECK(reloaded.GetViews().Projects.size() == 1);
		CHECK(std::vector<AssetHandle>(reloaded.GetOpenGraphs().begin(), reloaded.GetOpenGraphs().end()) ==
		      std::vector<AssetHandle>{player});
	}
}

TEST_CASE("VisualScriptViews: damaged files load or start empty, and what loads saves and loads again")
{
	// Damaged documents are always JSON, so the rounds read and write the file's form in memory: Load and Save only add the
	// reading and writing of the text, which the cases above go through.
	Json const seed = ReadJson(GetViewsFixturePath());
	Testing::DocumentMutator mutator(10300);
	int loaded = 0;
	int startedEmpty = 0;
	int const rounds = Testing::GetFuzzRounds(200);
	for (int round = 0; round < rounds; round++)
	{
		CAPTURE(round);
		Json const damaged = mutator.Damage(seed, round);
		std::vector<std::string> warnings;
		VisualScriptViews const views = VisualScriptViewSettings::Deserialize(damaged, warnings);
		if (AnyContains(warnings, "the file is ignored"))
		{
			CHECK(warnings.size() == 1);
			CHECK(views == VisualScriptViews());
			startedEmpty++;
		}
		else
		{
			loaded++;
		}

		// What loaded saves, and loads again as it was, without a warning.
		Result<Json> const saved = ParseJson(DumpJson(VisualScriptViewSettings::Serialize(views)));
		REQUIRE_MESSAGE(saved.IsOk(), saved.GetError());
		std::vector<std::string> again;
		CHECK(VisualScriptViewSettings::Deserialize(saved.GetValue(), again) == views);
		CHECK_MESSAGE(again.empty(), (again.empty() ? std::string() : again.front()));
	}
	CHECK(loaded > 0);
	CHECK(startedEmpty > 0);
}
