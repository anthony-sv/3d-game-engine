#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// Makes this scope the source of the project's input actions (VisualScriptEngine::SetInputActionsSource), answering what
	// Project holds, as an editor's context does for its project.
	class InputActionsSourceScope
	{
	public:
		InputActionsSourceScope()
		{
			VisualScriptEngine::SetInputActionsSource(this,
			                                          [this]()
			                                          {
														  return Project;
													  });
		}
		~InputActionsSourceScope() { VisualScriptEngine::ClearInputActionsSource(this); }

		InputActionsSourceScope(InputActionsSourceScope const&) = delete;
		InputActionsSourceScope& operator=(InputActionsSourceScope const&) = delete;

		// The project's InputActions setting; nothing while no project is open.
		std::optional<AssetHandle> Project;
	};

	InputAction MakeAction(std::string name, std::string controls)
	{
		InputAction action;
		action.Name = std::move(name);
		InputActionBinding binding;
		binding.Controls = std::move(controls);
		action.Bindings.push_back(std::move(binding));
		return action;
	}

	InputActionMap MakeMap(std::vector<std::pair<std::string, std::string>> const& actions)
	{
		InputActionMap map;
		for (auto const& [name, controls] : actions)
		{
			map.Actions.push_back(MakeAction(name, controls));
		}
		return map;
	}

	Ref<TypedDataAsset<InputActionMap>> MakeAsset(InputActionMap const& map)
	{
		TypedDataAssetType<InputActionMap> const* const type = DataAssetRegistry::Find<InputActionMap>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<InputActionMap>> const asset = std::static_pointer_cast<TypedDataAsset<InputActionMap>>(type->CreateAsset());
		Result<void> const set = asset->SetData(map);
		REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
		return asset;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	// Event.ActionPressed of Jmp (a typo of Jump) and a read of Steer.
	VisualScriptDocument MakeActionsDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(1, "Event.ActionPressed", Json::object({{"Action", "Jmp"}})));
		document.Graph.Nodes.push_back(
			MakeNode(2, "Call:Strada.Input.GetAction(string)", Json::object(), Json::object({{"Action", "Steer"}})));
		return document;
	}

	std::vector<VisualScriptDiagnostic> FindFindings(VisualScriptAnalysis const& analysis)
	{
		std::vector<VisualScriptDiagnostic> findings;
		std::copy_if(analysis.Diagnostics.begin(), analysis.Diagnostics.end(), std::back_inserter(findings),
		             [](VisualScriptDiagnostic const& diagnostic)
		             {
						 return diagnostic.Code == "UnknownAction" || diagnostic.Code == "NoInputActions";
					 });
		return findings;
	}

	Ref<VisualScriptAnalysis const> AnalyzeOrFail(VisualScriptDocument const& document)
	{
		Result<Ref<VisualScriptAnalysis const>> analysis = VisualScriptEngine::Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
		return analysis.TakeValue();
	}
}

TEST_CASE("VisualScriptLibrary: the runtime's library marks the settings and inputs that name input actions")
{
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	VisualScriptNodeTemplate const* const pressed = library->FindNode("Event.ActionPressed");
	REQUIRE(pressed != nullptr);
	REQUIRE(pressed->Settings.size() == 1);
	CHECK(pressed->Settings[0].NamesInputAction);
	VisualScriptNodeTemplate const* const rebind = library->FindNode("Call:Strada.Input.Rebind(string,int,string,int)");
	REQUIRE(rebind != nullptr);
	for (VisualScriptPin const& pin : rebind->Pins)
	{
		CAPTURE(pin.Name);
		CHECK(pin.NamesInputAction == (pin.Name == "Action"));
	}
	VisualScriptNodeTemplate const* const key = library->FindNode("Call:Strada.Input.IsKeyDown(Strada.KeyCode)");
	REQUIRE(key != nullptr);
	CHECK(std::none_of(key->Pins.begin(), key->Pins.end(),
	                   [](VisualScriptPin const& pin)
	                   {
						   return pin.NamesInputAction;
					   }));
}

TEST_CASE("VisualScriptEngine: an analysis of a document naming actions checks them against the project's input actions it records")
{
	Testing::AssetManagerScope const assets;
	Testing::ScriptRuntimeScope const runtime;
	InputActionsSourceScope source;
	Ref<TypedDataAsset<InputActionMap>> const asset = MakeAsset(MakeMap({{"Jump", "Key/Space"}, {"Steer", "Key/A Key/D"}}));
	AssetHandle const actions = AssetManager::AddMemoryAsset(asset, "Actions.sinput");
	source.Project = actions;

	Ref<VisualScriptInputActions const> const told = VisualScriptEngine::GetInputActions();
	REQUIRE(told != nullptr);
	CHECK(told->Names == std::vector<std::string>{"Jump", "Steer"});
	CHECK(told->Missing.empty());
	CHECK(VisualScriptEngine::GetInputActions() == told);

	Ref<VisualScriptAnalysis const> const analysis = AnalyzeOrFail(MakeActionsDocument());
	CHECK(analysis->NamesInputActions);
	CHECK(analysis->InputActions == told);
	std::vector<VisualScriptDiagnostic> const findings = FindFindings(*analysis);
	REQUIRE(findings.size() == 1);
	CHECK(findings[0].Code == "UnknownAction");
	CHECK(findings[0].Severity == VisualScriptSeverity::Warning);
	CHECK(findings[0].Node == 1u);
	CHECK(findings[0].Setting == "Action");
	CHECK(findings[0].Message == "Action: the project has no input action \"Jmp\"");
	CHECK(findings[0].Suggestions == std::vector<std::string>{"Jump", "Steer"});

	SUBCASE("a document without an action node is analyzed as before: told nothing, and not out of date when actions change")
	{
		VisualScriptDocument keys;
		keys.Graph.Nodes.push_back(MakeNode(1, "Event.Update"));
		keys.Graph.Nodes.push_back(
			MakeNode(2, "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", Json::object(), Json::object({{"Key", "Space"}})));
		Ref<VisualScriptAnalysis const> const plain = AnalyzeOrFail(keys);
		CHECK_FALSE(plain->NamesInputActions);
		CHECK(plain->InputActions == nullptr);
		CHECK(plain->Diagnostics.empty());
	}

	SUBCASE("an edit that keeps the names keeps the same actions; a renamed action makes others, which the next analysis reads")
	{
		InputActionMap rebound = MakeMap({{"Jump", "Key/J"}, {"Steer", "Key/A Key/D"}});
		REQUIRE(asset->SetData(rebound).IsOk());
		CHECK(VisualScriptEngine::GetInputActions() == told);

		REQUIRE(asset->SetData(MakeMap({{"Leap", "Key/Space"}, {"Steer", "Key/A Key/D"}})).IsOk());
		Ref<VisualScriptInputActions const> const renamed = VisualScriptEngine::GetInputActions();
		REQUIRE(renamed != nullptr);
		CHECK(renamed != told);
		CHECK(renamed->Names == std::vector<std::string>{"Leap", "Steer"});
		VisualScriptDocument jump = MakeActionsDocument();
		jump.Graph.Nodes[0].Settings["Action"] = "Jump";
		Ref<VisualScriptAnalysis const> const again = AnalyzeOrFail(jump);
		CHECK(again->InputActions == renamed);
		std::vector<VisualScriptDiagnostic> const unknown = FindFindings(*again);
		REQUIRE(unknown.size() == 1);
		CHECK(unknown[0].Message == "Action: the project has no input action \"Jump\"");
		CHECK(unknown[0].Suggestions.front() == "Leap");
	}

	SUBCASE("an asset removed, or a project naming none, is one NoInputActions per document")
	{
		AssetManager::RemoveMemoryAsset(actions);
		Ref<VisualScriptInputActions const> const removed = VisualScriptEngine::GetInputActions();
		REQUIRE(removed != nullptr);
		CHECK(removed->Names.empty());
		CHECK(removed->Missing == "the project's input actions, " + actions.ToString() + ", do not load as an InputActions asset");
		std::vector<VisualScriptDiagnostic> missing = FindFindings(*AnalyzeOrFail(MakeActionsDocument()));
		REQUIRE(missing.size() == 1);
		CHECK(missing[0].Code == "NoInputActions");
		CHECK(missing[0].Node == 1u);
		CHECK(missing[0].Message == removed->Missing + ": action names are not checked");

		source.Project = AssetHandle();
		Ref<VisualScriptInputActions const> const none = VisualScriptEngine::GetInputActions();
		REQUIRE(none != nullptr);
		CHECK(none->Missing == "the project names no input actions (Project Settings, Input Actions)");
		missing = FindFindings(*AnalyzeOrFail(MakeActionsDocument()));
		REQUIRE(missing.size() == 1);
		CHECK(missing[0].Message == "the project names no input actions (Project Settings, Input Actions): action names are not checked");
	}

	SUBCASE("without a project nothing is told nor checked")
	{
		source.Project.reset();
		CHECK(VisualScriptEngine::GetInputActions() == nullptr);
		Ref<VisualScriptAnalysis const> const untold = AnalyzeOrFail(MakeActionsDocument());
		CHECK(untold->NamesInputActions);
		CHECK(untold->InputActions == nullptr);
		CHECK(FindFindings(*untold).empty());
	}

	SUBCASE("only the owner that set the source removes it")
	{
		int const other = 0;
		VisualScriptEngine::ClearInputActionsSource(&other);
		CHECK(VisualScriptEngine::GetInputActions() == told);
		VisualScriptEngine::ClearInputActionsSource(&source);
		CHECK(VisualScriptEngine::GetInputActions() == nullptr);
	}
}

TEST_CASE("VisualScriptEngine: the source of the project's input actions outlives the scripting runtime and works without it")
{
	Testing::AssetManagerScope const assets;
	InputActionsSourceScope source;
	source.Project = AssetManager::AddMemoryAsset(MakeAsset(MakeMap({{"Fire", "Key/F"}})), "Fire.sinput");
	{
		Testing::ScriptRuntimeScope const runtime;
		CHECK(FindFindings(*AnalyzeOrFail(MakeActionsDocument())).size() == 2);
	}
	Ref<VisualScriptInputActions const> const actions = VisualScriptEngine::GetInputActions();
	REQUIRE(actions != nullptr);
	CHECK(actions->Names == std::vector<std::string>{"Fire"});
	Testing::ScriptRuntimeScope const again;
	CHECK(FindFindings(*AnalyzeOrFail(MakeActionsDocument())).size() == 2);
}
