#include "Script/ScriptTestUtilities.h"
#include "Script/VisualScriptTestGraphs.h"
#include "TestUtilities.h"

#include "Editor/VisualScripting/VisualScriptArrange.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptHeadlessCanvas.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Strada;

// Spec-slice4 section 18.6: the sample projects' graphs read well, as their authoring tools arrange and frame them.
namespace
{
	// The graphs of a sample project that must read well: every .sgraph below a folder of its Assets.
	struct SampleGraphSet
	{
		// The project's folder in Projects.
		std::string_view Project;
		// The folder below Assets that holds the graphs, with forward slashes; empty for all of Assets.
		std::string_view Folder;
	};

	constexpr SampleGraphSet SampleGraphSets[] = {
		{"PrismBreak", ""},
	};

	// The longest title a region's comment box may have.
	constexpr size_t MaxBoxTitleLength = 72;
	// The least room between two nodes of a sub-graph.
	constexpr float MinNodeGap = 16.0f;

	size_t CountCodePoints(std::string_view text)
	{
		return static_cast<size_t>(std::count_if(text.begin(), text.end(),
		                                         [](char byte)
		                                         {
													 return (static_cast<unsigned char>(byte) & 0xC0u) != 0x80u;
												 }));
	}

	bool Encloses(VisualScriptRect const& outer, VisualScriptRect const& inner)
	{
		return inner.Min.x >= outer.Min.x && inner.Min.y >= outer.Min.y && inner.Max.x <= outer.Max.x && inner.Max.y <= outer.Max.y;
	}

	// The distance between two rectangles: zero when they touch or overlap.
	float GetDistance(VisualScriptRect const& a, VisualScriptRect const& b)
	{
		glm::vec2 const gap = glm::max(glm::max(a.Min - b.Max, b.Min - a.Max), glm::vec2(0.0f));
		return glm::length(gap);
	}

	// The graph's files below the folder of the project's Assets, as asset paths, sorted.
	std::vector<std::string> FindGraphPaths(std::filesystem::path const& assets, std::string_view folder)
	{
		std::filesystem::path const root = folder.empty() ? assets : assets / FileSystem::PathFromUtf8(folder);
		REQUIRE_MESSAGE(FileSystem::IsDirectory(root), FileSystem::PathToUtf8(root));
		std::vector<std::string> paths;
		for (std::filesystem::directory_entry const& entry : std::filesystem::recursive_directory_iterator(root))
		{
			if (entry.is_regular_file() && entry.path().extension() == ".sgraph")
			{
				paths.push_back(FileSystem::PathToUtf8(FileSystem::GetRelativePath(entry.path(), assets)));
			}
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}

	// The sub-graph of a document: 0 for the event graph, else a function's ID, which the document has.
	VisualScriptGraph const& GetSubGraph(VisualScriptDocument const& document, uint32_t function)
	{
		if (function == 0)
		{
			return document.Graph;
		}
		auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
		                                [function](VisualScriptFunction const& candidate)
		                                {
											return candidate.ID == function;
										});
		REQUIRE(found != document.Functions.end());
		return found->Graph;
	}

	// A graph of a sample set, read strictly from the project's own file.
	struct SampleGraph
	{
		std::string Path;
		AssetHandle Handle;
		VisualScriptDocument Document;
	};

	// Checks one sub-graph (0: the event graph, else a function's ID) of a sample graph as section 18.6 reads it.
	void CheckSubGraph(SampleGraph const& graph, uint32_t function, VisualScriptGraph const& subGraph, VisualScriptAnalysis const& analysis,
	                   VisualScriptLibrary const& library, VisualScriptGraphResolver const& resolve)
	{
		bool placed = true;
		for (VisualScriptNode const& node : subGraph.Nodes)
		{
			CAPTURE(node.ID);
			CHECK_MESSAGE(node.Position.has_value(), "the node has no position");
			placed = placed && node.Position.has_value();
		}
		if (!placed)
		{
			return;
		}

		// Measured as a window draws the nodes, with the graph's analysis.
		VisualScriptHeadlessCanvas canvas;
		canvas.Sync(graph.Document, function, &analysis, &graph.Document, &library, resolve);
		VisualScriptNodeExtents const extents = canvas.GetExtents();
		std::vector<VisualScriptRect> nodes;
		nodes.reserve(subGraph.Nodes.size());
		for (VisualScriptNode const& node : subGraph.Nodes)
		{
			nodes.push_back({*node.Position, *node.Position + extents(node.ID)});
		}

		// Every node lies inside exactly one comment box.
		std::unordered_map<uint32_t, size_t> boxOf;
		std::vector<std::vector<uint32_t>> framed(subGraph.Comments.size());
		for (size_t index = 0; index < subGraph.Nodes.size(); index++)
		{
			VisualScriptNode const& node = subGraph.Nodes[index];
			CAPTURE(node.ID);
			CAPTURE(node.Type);
			size_t boxes = 0;
			for (size_t box = 0; box < subGraph.Comments.size(); box++)
			{
				VisualScriptComment const& comment = subGraph.Comments[box];
				if (Encloses({comment.Position, comment.Position + comment.Size}, nodes[index]))
				{
					boxes++;
					boxOf[node.ID] = box;
					framed[box].push_back(node.ID);
				}
			}
			CHECK_MESSAGE(boxes == 1, fmt::format("the node lies inside {} comment boxes, not exactly one", boxes));
		}

		// Every box has a short title and frames two nodes at least.
		for (size_t box = 0; box < subGraph.Comments.size(); box++)
		{
			VisualScriptComment const& comment = subGraph.Comments[box];
			CAPTURE(comment.ID);
			CAPTURE(comment.Text);
			CHECK_FALSE(comment.Text.empty());
			CHECK(CountCodePoints(comment.Text) <= MaxBoxTitleLength);
			CHECK(framed[box].size() >= 2);
		}

		// No two nodes overlap or come closer than the least gap.
		for (size_t first = 0; first < nodes.size(); first++)
		{
			for (size_t second = first + 1; second < nodes.size(); second++)
			{
				if (float const distance = GetDistance(nodes[first], nodes[second]); distance < MinNodeGap)
				{
					FAIL_CHECK(fmt::format("nodes {} and {} are {} cu apart, closer than {}", subGraph.Nodes[first].ID,
					                       subGraph.Nodes[second].ID, distance, MinNodeGap));
				}
			}
		}

		// No link joins the nodes of two boxes.
		for (VisualScriptLink const& link : subGraph.Links)
		{
			auto const from = boxOf.find(link.From.Node);
			auto const to = boxOf.find(link.To.Node);
			if (from != boxOf.end() && to != boxOf.end() && from->second != to->second)
			{
				FAIL_CHECK(fmt::format("the link {}.{} -> {}.{} joins the boxes '{}' and '{}'", link.From.Node, link.From.Pin, link.To.Node,
				                       link.To.Pin, subGraph.Comments[from->second].Text, subGraph.Comments[to->second].Text));
			}
		}

		// Arranging each box's nodes from their top-left corner, as the authoring tool did, moves none of them.
		for (size_t box = 0; box < subGraph.Comments.size(); box++)
		{
			if (framed[box].empty())
			{
				continue;
			}
			CAPTURE(subGraph.Comments[box].Text);
			glm::vec2 corner(std::numeric_limits<float>::max());
			for (uint32_t const id : framed[box])
			{
				auto const node = std::find_if(subGraph.Nodes.begin(), subGraph.Nodes.end(),
				                               [id](VisualScriptNode const& candidate)
				                               {
												   return candidate.ID == id;
											   });
				corner = glm::min(corner, *node->Position);
			}
			VisualScriptDocument arranged = graph.Document;
			Result<void> const done = VisualScriptEditing::ArrangeNodes(arranged, function, framed[box], extents, corner);
			REQUIRE_MESSAGE(done.IsOk(), (done ? std::string() : done.GetError()));
			VisualScriptGraph const& result = GetSubGraph(arranged, function);
			REQUIRE(result.Nodes.size() == subGraph.Nodes.size());
			std::string moved;
			for (size_t index = 0; index < subGraph.Nodes.size(); index++)
			{
				if (result.Nodes[index].Position != subGraph.Nodes[index].Position)
				{
					moved += fmt::format("{}{}", moved.empty() ? "" : ", ", subGraph.Nodes[index].ID);
				}
			}
			CHECK_MESSAGE(
				moved.empty(),
				fmt::format("arranging the box moves the nodes {}: re-run the project's Tools/Author.py after a change to arranging",
			                moved));
		}
	}
}

TEST_CASE("VisualScriptEditor: the sample projects' graphs read well: every node placed in a titled box, none overlapping, each box "
          "arranged")
{
	Testing::ScriptRuntimeScope runtime;
	for (SampleGraphSet const& set : SampleGraphSets)
	{
		CAPTURE(set.Project);
		std::filesystem::path const project = FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR) / FileSystem::PathFromUtf8(set.Project);
		std::filesystem::path const assets = project / "Assets";
		// The graphs are analyzed with a copy of the project's registry and graphs as the asset directory, where the graphs
		// they name are found, and with the project's game scripts when it has some: its graphs may use their members.
		Testing::TemporaryDirectory directory;
		Testing::AssetManagerScope manager;
		Testing::OpenVisualScriptsCopy(assets, directory.GetPath());
		Testing::ProjectScriptsScope scripts(project);
		Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
		REQUIRE(library != nullptr);

		std::vector<SampleGraph> graphs;
		for (std::string const& path : FindGraphPaths(assets, set.Folder))
		{
			CAPTURE(path);
			AssetHandle const handle = AssetManager::FindByPath(path);
			REQUIRE(handle.IsValid());
			std::vector<VisualScriptIssue> issues;
			Result<VisualScriptDocument> document =
				VisualScriptSerializer::LoadFromFile(assets / FileSystem::PathFromUtf8(path), Testing::MakeStrictContext(), &issues);
			REQUIRE_MESSAGE(document.IsOk(), (document ? std::string() : document.GetError()));
			CHECK_MESSAGE(issues.empty(), Testing::DescribeIssues(issues));
			graphs.push_back({path, handle, document.TakeValue()});
		}
		REQUIRE_FALSE(graphs.empty());
		VisualScriptGraphResolver const resolve = [&graphs](AssetHandle other) -> VisualScriptDocument const*
		{
			auto const found = std::find_if(graphs.begin(), graphs.end(),
			                                [other](SampleGraph const& candidate)
			                                {
												return candidate.Handle == other;
											});
			return found != graphs.end() ? &found->Document : nullptr;
		};

		for (SampleGraph const& graph : graphs)
		{
			CAPTURE(graph.Path);
			// A description per graph and a tooltip per public variable.
			CHECK_FALSE(graph.Document.Description.empty());
			for (VisualScriptVariable const& variable : graph.Document.Variables)
			{
				CAPTURE(variable.Name);
				CHECK((!variable.Public || !variable.Tooltip.empty()));
			}

			Result<Ref<VisualScriptAnalysis const>> const analysis = VisualScriptEngine::Analyze(graph.Document, graph.Handle);
			REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
			{
				std::string_view const subGraph = "the event graph";
				CAPTURE(subGraph);
				CheckSubGraph(graph, 0, graph.Document.Graph, *analysis.GetValue(), *library, resolve);
			}
			for (VisualScriptFunction const& function : graph.Document.Functions)
			{
				CAPTURE(function.Name);
				CheckSubGraph(graph, function.ID, function.Graph, *analysis.GetValue(), *library, resolve);
			}
		}
	}
}
