#include "Audio/AudioTestUtilities.h"
#include "Core/ProfilerTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

#ifdef ST_ENABLE_PROFILING

namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Settings = std::move(settings);
		return node;
	}

	// Adds 1 to Updates on every update.
	VisualScriptDocument MakeCounterDocument()
	{
		VisualScriptDocument document;
		VisualScriptVariable updates;
		updates.ID = 1;
		updates.Name = "Updates";
		updates.Type = "int";
		updates.Value = 0;
		document.Variables.push_back(updates);
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Add"));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 1}})));
		document.Graph.Nodes[2].Inputs = Json::object({{"B", 1}});
		document.Graph.Links.push_back({{10, "Then"}, {11, "Execute"}});
		document.Graph.Links.push_back({{13, "Value"}, {12, "A"}});
		document.Graph.Links.push_back({{12, "Result"}, {11, "Value"}});
		return document;
	}

	// The zone's parent in its frame: the nearest zone before it on its thread with a smaller depth.
	std::string_view FindParentName(ProfilerCapture const& capture, ProfilerFrame const& frame, size_t zoneIndex)
	{
		ProfilerZone const& zone = frame.Zones[zoneIndex];
		for (size_t index = zoneIndex; index-- > 0;)
		{
			ProfilerZone const& earlier = frame.Zones[index];
			if (earlier.Thread == zone.Thread && earlier.Depth < zone.Depth)
			{
				return capture.Names[earlier.Name];
			}
		}
		return {};
	}

	// Every parent the zones of a name have, over the capture's frames.
	std::vector<std::string> FindParentNames(ProfilerCapture const& capture, std::string_view zoneName)
	{
		std::vector<std::string> parents;
		for (ProfilerFrame const& frame : capture.Frames)
		{
			for (size_t index = 0; index < frame.Zones.size(); ++index)
			{
				if (capture.Names[frame.Zones[index].Name] == zoneName)
				{
					std::string const parent(FindParentName(capture, frame, index));
					if (std::find(parents.begin(), parents.end(), parent) == parents.end())
					{
						parents.push_back(parent);
					}
				}
			}
		}
		return parents;
	}
}

TEST_CASE("Profiler: a running scene records its phases, its scripts by class, its graphs with their steps, physics and audio")
{
	Testing::ProfilerScope const profiler;
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::AudioEngineScope const audio;
	Testing::ScriptEngineScope const scripts;

	Scene scene("Profiled");
	Entity mover = scene.CreateEntity("Mover");
	mover.AddComponent<ScriptComponent>().ClassName = "Strada.Tests.Mover";
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeCounterDocument()), "Graphs/Counter.sgraph");
	scene.CreateEntity("Counter").AddComponent<VisualScriptComponent>().Graph = graph;
	Entity box = scene.CreateEntity("Box");
	box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	box.AddComponent<BoxColliderComponent>();
	scene.OnRuntimeStart();

	REQUIRE(Profiler::Start().IsOk());
	std::vector<float> samples(2 * 512);
	constexpr int Frames = 5;
	for (int frame = 0; frame < Frames; ++frame)
	{
		Profiler::BeginFrame();
		scene.OnUpdateRuntime(Timestep(FrameTime));
		CHECK(AudioEngine::ReadFrames(samples) > 0);
		Profiler::EndFrame();
	}
	Profiler::Stop();
	scene.OnRuntimeStop();

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == Frames);
	// Each phase once a frame, in the order the scene runs them, inside the scene's update.
	for (std::string_view const phase : {"Scene.UpdateUI", "Scene.UpdateScripts", "Scene.UpdatePhysics", "Scene.ContactEvents",
	                                     "Scene.UpdateAudio", "Scene.UpdateParticles", "Scene.Destroy"})
	{
		CAPTURE(phase);
		CHECK(Testing::FindZones(capture, phase).size() == Frames);
		CHECK(FindParentNames(capture, phase) == std::vector<std::string>{"Scene.Update"});
	}
	CHECK(Testing::FindZones(capture, "Scene.Update").size() == Frames);

	// The C# script by its class and the graph by its asset path, in the update and in each fixed step, the graph with the
	// steps its VM ran: those of its Event.Update, none in fixed steps it has no event for.
	std::vector<std::string> const fixedOrFrame = {"Scene.UpdateScripts", "Scene.FixedUpdateScripts"};
	CHECK(FindParentNames(capture, "Script.Strada.Tests.Mover") == fixedOrFrame);
	CHECK(FindParentNames(capture, "Graph.Graphs/Counter.sgraph") == fixedOrFrame);
	std::vector<ProfilerZone> const graphs = Testing::FindZones(capture, "Graph.Graphs/Counter.sgraph");
	size_t updates = 0;
	for (ProfilerZone const& zone : graphs)
	{
		REQUIRE(zone.ValueLabel != ProfilerNoName);
		CHECK(capture.Names[zone.ValueLabel] == "Steps");
		updates += zone.Value > 0 ? 1 : 0;
	}
	CHECK(updates == Frames);

	// A step at 60 Hz in frames of 1/60 s; the fixed update's scripts and the body's sync around it.
	CHECK(FindParentNames(capture, "Physics.Step") == std::vector<std::string>{"Scene.UpdatePhysics"});
	CHECK(Testing::FindZones(capture, "Physics.Step").size() >= Frames - 1);
	CHECK(FindParentNames(capture, "Scene.FixedUpdateScripts") == std::vector<std::string>{"Scene.UpdatePhysics"});
	CHECK(FindParentNames(capture, "Audio.Mix") == std::vector<std::string>{""});
	CHECK(Testing::FindZones(capture, "Audio.Mix").size() == Frames);

	// Everything above is the main thread's, so the subsystems' medians name each.
	ProfilerMedians const medians = ComputeProfilerMedians(capture);
	std::vector<std::string> subsystems;
	for (ProfilerMedian const& median : medians.Subsystems)
	{
		subsystems.push_back(median.Name);
	}
	for (std::string_view const subsystem : {"Audio", "Graph", "Physics", "Scene", "Script"})
	{
		CAPTURE(subsystem);
		CHECK(std::find(subsystems.begin(), subsystems.end(), subsystem) != subsystems.end());
	}
}

#endif
