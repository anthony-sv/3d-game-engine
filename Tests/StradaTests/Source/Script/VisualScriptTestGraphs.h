#pragma once

#include "Script/ScriptTestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// The visual scripts the engine's tests run: the shared fixture documents and the graphs projects run.
namespace Strada::Testing
{
	// The shared fixture documents (Tests/Data/VisualScripting/Documents) that VisualScriptEngineTests.cpp runs on entities
	// with and without children and bodies; the node coverage test accepts them as covering the nodes they run.
	inline constexpr std::array<std::string_view, 8> SharedVisualScriptFixtures = {
		"Data.json", "Editor.json", "Events.json", "Flow.json", "Functions.json", "Language.json", "Reflected.json", "Variables.json"};

	// Strict about the format, as automation loads documents: unknown keys are issues.
	inline DeserializationContext MakeStrictContext()
	{
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		return context;
	}

	inline std::string DescribeIssues(std::vector<VisualScriptIssue> const& issues)
	{
		std::string described;
		for (VisualScriptIssue const& issue : issues)
		{
			described += (described.empty() ? "" : "; ") + issue.Path + ": " + issue.Message;
		}
		return described;
	}

	// A graph of a project's run set: one its scenes' VisualScript components attach, or a function library such a graph
	// calls (directly or through other libraries), read strictly from the project's file.
	struct RunSetGraph
	{
		std::string Path;
		AssetHandle Handle;
		VisualScriptDocument Document;
	};

	// The run set of the project whose Assets directory this is, while the AssetManager has a copy of its registry open
	// (OpenVisualScriptsCopy): the graphs its scenes attach, by handle, then the libraries those reach, each once. Scenes
	// and graphs are read from the project's own files.
	inline std::vector<RunSetGraph> FindVisualScriptRunSet(std::filesystem::path const& assets)
	{
		std::vector<AssetHandle> attached;
		for (AssetMetadata const& scene : AssetManager::GetAssets(AssetType::Scene))
		{
			CAPTURE(scene.Path);
			Result<std::string> text = FileSystem::ReadTextFile(assets / FileSystem::PathFromUtf8(scene.Path));
			REQUIRE(text.IsOk());
			Result<Json> document = ParseJson(text.GetValue());
			REQUIRE_MESSAGE(document.IsOk(), (document ? std::string() : document.GetError()));
			for (Json const& entity : document.GetValue().at("Entities"))
			{
				auto const component = entity.at("Components").find("VisualScript");
				if (component == entity.at("Components").end())
				{
					continue;
				}
				AssetHandle handle;
				REQUIRE(JsonTraits<AssetHandle>::FromJson(component->at("Graph"), handle, DeserializationContext{}).IsOk());
				if (handle.IsValid())
				{
					CHECK_MESSAGE(AssetManager::GetAssetType(handle) == AssetType::VisualScript, handle.ToString());
					attached.push_back(handle);
				}
			}
		}
		std::sort(attached.begin(), attached.end(),
		          [](AssetHandle a, AssetHandle b)
		          {
					  return a.GetUUID().GetValue() < b.GetUUID().GetValue();
				  });
		attached.erase(std::unique(attached.begin(), attached.end()), attached.end());

		// Each attached graph, then the libraries the graphs before it reach.
		std::vector<RunSetGraph> graphs;
		std::unordered_set<AssetHandle> seen(attached.begin(), attached.end());
		std::vector<AssetHandle> pending = attached;
		for (size_t index = 0; index < pending.size(); index++)
		{
			AssetHandle const handle = pending[index];
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
			REQUIRE_MESSAGE(metadata.has_value(), handle.ToString());
			CAPTURE(metadata->Path);
			std::vector<VisualScriptIssue> issues;
			Result<VisualScriptDocument> document =
				VisualScriptSerializer::LoadFromFile(assets / FileSystem::PathFromUtf8(metadata->Path), MakeStrictContext(), &issues);
			REQUIRE_MESSAGE(document.IsOk(), (document ? std::string() : document.GetError()));
			CHECK_MESSAGE(issues.empty(), DescribeIssues(issues));
			for (VisualScriptGraphReference const& reference : CollectVisualScriptReferences(document.GetValue()))
			{
				if (reference.Use == VisualScriptReferenceUse::Library && seen.insert(reference.Graph).second)
				{
					pending.push_back(reference.Graph);
				}
			}
			graphs.push_back({metadata->Path, handle, document.TakeValue()});
		}
		return graphs;
	}
}
