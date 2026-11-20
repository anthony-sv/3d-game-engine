#include "Asset/TestDataAsset.h"
#include "Audio/AudioTestUtilities.h"
#include "Core/ProfilerTestUtilities.h"
#include "Core/TelemetryTestUtilities.h"
#include "EditorLocationsTestUtilities.h"
#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/GraphDebugCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/Automation/ProfilerCommands.h"
#include "Editor/Automation/ProjectContentCommands.h"
#include "Editor/Automation/ScriptCommands.h"
#include "Editor/Automation/StartCommands.h"
#include "Editor/Automation/TelemetryCommands.h"
#include "Editor/EditorCamera.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorProfiler.h"
#include "Editor/EditorScripts.h"
#include "Editor/EditorTelemetry.h"
#include "Editor/ExampleCopyTask.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/VisualScriptDebuggerPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/Viewport/ViewportOverlays.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// Commands the fuzzer leaves out: they replace the open project (project.create, project.open, project.close) or
	// need .NET builds (script.create makes the project's scripts need one, script.build runs it), which their own tests
	// cover with real projects.
	constexpr std::array<std::string_view, 5> ExcludedCommands = {"project.create", "project.open", "project.close", "script.create",
	                                                              "script.build"};

	// The visual script the fixture seeds with the feature-test sample of the format, whose sub-graphs the graph commands'
	// parameters are drawn from while it exists.
	constexpr char const* SeedGraphPath = "Scripts/Everything.sgraph";
	// The function library the seed script calls (Tests/Data/VisualScripting/Library.sgraph), so graph.interface and the
	// referrers of graph.set and graph.edit have graphs that refer to one another.
	constexpr char const* SeedLibraryPath = "Scripts/FuzzLibrary.sgraph";

	// The data asset type whose commands the command names (<domain>.create, get or set), else null.
	DataAssetType const* FindDataAssetCommandType(std::string_view command)
	{
		std::string_view const domain = command.substr(0, command.find('.'));
		for (DataAssetType const* type : DataAssetRegistry::GetTypes())
		{
			if (type->GetCommandDomain() == domain)
			{
				return type;
			}
		}
		return nullptr;
	}

	// The data asset type whose commands take the parameter (its asset parameter), else null.
	DataAssetType const* FindDataAssetParameterType(std::string_view parameter)
	{
		for (DataAssetType const* type : DataAssetRegistry::GetTypes())
		{
			if (type->GetCommandParameter() == parameter)
			{
				return type;
			}
		}
		return nullptr;
	}

	// Library.sgraph's document (its content form).
	Json LoadLibraryContent()
	{
		Result<std::string> const text =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Library.sgraph");
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		Json const file = Json::parse(text.GetValue(), nullptr, false);
		REQUIRE(file.is_object());
		return file["VisualScript"];
	}

	// The sample of the format with nodes that name other graphs: a typed Send of its custom event Hit to the seed script
	// itself, a call of the library's Clamp01 and a Get of its own public variable 1 on another entity running it.
	Json MakeSeedDocument(Json sample, std::string const& seed, std::string const& library)
	{
		Json& nodes = sample["Graph"]["Nodes"];
		nodes.push_back(Json::object({{"ID", 9001},
		                              {"Type", "Event.Send"},
		                              {"Position", Json::array({0.0, 2400.0})},
		                              {"Settings", Json::object({{"Name", "Hit"}, {"Graph", seed}})}}));
		nodes.push_back(Json::object({{"ID", 9002},
		                              {"Type", "Function.Call"},
		                              {"Position", Json::array({320.0, 2400.0})},
		                              {"Settings", Json::object({{"Function", 1}, {"Graph", library}})}}));
		nodes.push_back(Json::object({{"ID", 9003},
		                              {"Type", "Variable.Get"},
		                              {"Position", Json::array({640.0, 2400.0})},
		                              {"Settings", Json::object({{"Variable", 1}, {"Graph", seed}})}}));
		return sample;
	}

	// Input is global: the fuzzer presses keys through input.set.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	using FileSnapshot = std::map<std::filesystem::path, std::pair<uintmax_t, std::filesystem::file_time_type>>;

	// Every file below a directory (but for the excluded ones) with its size and modification time, to tell which files
	// a run created, changed or removed.
	FileSnapshot SnapshotFiles(std::filesystem::path const& directory, std::vector<std::filesystem::path> const& excluded)
	{
		FileSnapshot files;
		std::error_code errorCode;
		for (auto it = std::filesystem::recursive_directory_iterator(directory, errorCode);
		     !errorCode && it != std::filesystem::recursive_directory_iterator(); it.increment(errorCode))
		{
			if (std::ranges::find(excluded, it->path()) != excluded.end())
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->is_regular_file(errorCode))
			{
				files[it->path()] = {it->file_size(errorCode), it->last_write_time(errorCode)};
			}
		}
		REQUIRE_FALSE(static_cast<bool>(errorCode));
		return files;
	}

	using FileContents = std::map<std::filesystem::path, std::string>;

	// Every file below a directory with its bytes, to tell whether undoing and redoing gave each file back exactly.
	FileContents ReadFiles(std::filesystem::path const& directory)
	{
		FileContents files;
		std::error_code errorCode;
		for (auto it = std::filesystem::recursive_directory_iterator(directory, errorCode);
		     !errorCode && it != std::filesystem::recursive_directory_iterator(); it.increment(errorCode))
		{
			if (it->is_regular_file(errorCode))
			{
				Result<Buffer> const bytes = FileSystem::ReadBinaryFile(it->path());
				REQUIRE(bytes.IsOk());
				files[it->path()] = std::string(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
			}
		}
		REQUIRE_FALSE(static_cast<bool>(errorCode));
		return files;
	}

	// The files whose bytes differ between two readings, and the files only one of them has.
	std::string ListChangedFiles(FileContents const& before, FileContents const& after)
	{
		std::string changed;
		for (auto const& [path, bytes] : before)
		{
			auto const found = after.find(path);
			if (found == after.end() || found->second != bytes)
			{
				changed += " " + FileSystem::PathToUtf8(path);
			}
		}
		for (auto const& [path, bytes] : after)
		{
			if (!before.contains(path))
			{
				changed += " " + FileSystem::PathToUtf8(path);
			}
		}
		return changed;
	}

	// What the generator knows about the editor, so parameters name entities, components and assets that exist.
	struct EditorState
	{
		std::vector<std::string> Entities;
		std::vector<std::string> EntityNames;
		// Entity ID -> the names of its components.
		std::map<std::string, std::vector<std::string>> EntityComponents;
		std::vector<std::string> Components;
		// Component name -> { field name: default value }.
		std::map<std::string, Json> ComponentFields;
		// Asset IDs and references (asset://<path>, builtin://<name>), all of them and by asset type.
		std::vector<std::string> Assets;
		std::map<std::string, std::vector<std::string>> AssetsByType;
		// Asset ID or reference -> the asset's path, for assets with a file.
		std::map<std::string, std::string> AssetPaths;
		std::vector<std::string> ScenePaths;
		std::vector<std::string> Folders;
		Json MaterialFields = Json::object();
		// Data asset type name -> the fields of one asset of the type (<domain>.get), for <domain>.create and set.
		std::map<std::string, Json> DataAssetFields;
		// The document of the seed script while it is there, else of the first visual script that loads (graph.get), for
		// graph.set and graph.check, and that script's handle.
		Json GraphDocument = Json::object();
		std::string Graph;
		// Whether that script is the seed (SeedGraphPath).
		bool SeedGraph = false;
		// What that document holds, for the graph windows' commands and graph.find: its functions' IDs; the IDs of each
		// sub-graph's nodes and comments (0: the event graph); its node IDs; the pin names its links and literals name; its
		// variables', locals' and functions' IDs; its custom events' names.
		std::vector<int64_t> GraphFunctions;
		std::map<int64_t, std::vector<int64_t>> GraphElements;
		std::vector<int64_t> GraphNodes;
		std::vector<std::string> GraphPins;
		std::vector<int64_t> GraphReferences;
		std::vector<std::string> GraphEvents;
		// Each sub-graph's links ({ from, to } as graph.edit names them) and comment boxes; the variables' IDs and names and the
		// functions' names; the document's revisions (graph.get); the clipboard text of a few of its event graph's elements
		// (graph.copy), for graph.edit's batches.
		std::map<int64_t, std::vector<Json>> GraphLinks;
		std::map<int64_t, std::vector<int64_t>> GraphComments;
		std::vector<int64_t> GraphVariables;
		std::vector<std::string> GraphVariableNames;
		std::vector<std::string> GraphFunctionNames;
		std::string GraphRevision;
		std::string GraphProgramRevision;
		std::string GraphClipboard;
		// The visual scripts whose windows are open (graph.editors).
		std::vector<std::string> OpenGraphs;
		Json ProjectSettings = Json::object();
		Json SceneSettings = Json::object();
		// The names of the actions the project's InputActions asset holds (input.set's actions).
		std::vector<std::string> ActionNames;
	};

	// Values for a string parameter: ones that name what exists or would be accepted, and edge cases to be refused.
	struct StringChoices
	{
		std::vector<std::string> Good;
		std::vector<std::string> Edge;
	};

	// Builds parameters from a command's schema. Most name what exists, so commands get past their lookups and do their
	// work; one value in ten breaks the schema, and a quarter of the strings are edge cases, so validation is exercised
	// too. Ranges of parameters that only make commands slower (frames run at once, game seconds) are narrowed so rounds
	// stay fast.
	class ParameterGenerator
	{
	public:
		ParameterGenerator(uint64_t seed, EditorState const& state, std::filesystem::path outside, std::vector<std::string> importFiles,
		                   std::filesystem::path project)
			: m_Random(seed),
			  m_Mutator(seed ^ 0xA5A5A5A5A5A5A5A5ull),
			  m_State(state),
			  m_Outside(std::move(outside)),
			  m_ImportFiles(std::move(importFiles)),
			  m_Project(std::move(project))
		{
		}

		Json Generate(std::string_view command, Json const& schema)
		{
			m_Command = command;
			if (schema.is_null())
			{
				// Commands without parameters must refuse any.
				return m_Random.Pick(10) == 0 ? Json::object({{"unexpected", 1}}) : Json::object();
			}
			Json params = Value(schema, "", Json::object(), 0);
			if (params.is_object() && m_Command.starts_with("graph."))
			{
				AdjustGraphParameters(params);
			}
			if (params.is_object() && m_Command.starts_with("project."))
			{
				AdjustProjectParameters(params);
			}
			if (params.is_object() && m_Command == "editor.recent")
			{
				AdjustRecentParameters(params);
			}
			if (params.is_object() && m_Command == "physics.query")
			{
				AdjustPhysicsQueryParameters(params);
			}
			if (params.is_object())
			{
				AdjustDataAssetParameters(params);
			}
			if (std::optional<Json> sure = SureRequest())
			{
				params = std::move(*sure);
			}
			return params;
		}

		// A request to a command succeeded ("graph.find query" and "graph.find references" name graph.find's two kinds): the
		// generator stops sending it the request that succeeds (SureRequest).
		void SetReached(std::string const& goal) { m_Reached.insert(goal); }

	private:
		Json Value(Json const& schema, std::string_view property, Json const& siblings, int depth)
		{
			if (depth > 0 && m_Random.Pick(10) == 0)
			{
				return InvalidValue();
			}
			if (property == "actions" && !schema.contains("type"))
			{
				return ActionValue();
			}
			std::string const type = PickType(schema);
			if (type == "object")
			{
				return ObjectValue(schema, property, siblings, depth);
			}
			if (type == "array")
			{
				return ArrayValue(schema, property, siblings, depth);
			}
			if (type == "string")
			{
				return StringValue(schema, property, siblings);
			}
			if (type == "integer")
			{
				return IntegerValue(schema, property);
			}
			if (type == "number")
			{
				return NumberValue(schema, property);
			}
			if (type == "boolean")
			{
				return m_Random.Pick(2) == 0;
			}
			if (type == "null")
			{
				return Json();
			}
			return InvalidValue();
		}

		std::string PickType(Json const& schema)
		{
			auto const type = schema.find("type");
			if (type == schema.end())
			{
				return {};
			}
			if (type->is_array())
			{
				return (*type)[m_Random.Pick(type->size())].get<std::string>();
			}
			return type->get<std::string>();
		}

		Json ObjectValue(Json const& schema, std::string_view property, Json const& siblings, int depth)
		{
			if (std::optional<Json> patch = Patch(property, siblings))
			{
				return *patch;
			}
			Json object = Json::object();
			std::set<std::string> required;
			if (auto const list = schema.find("required"); list != schema.end())
			{
				for (Json const& name : *list)
				{
					required.insert(name.get<std::string>());
				}
			}
			if (auto const properties = schema.find("properties"); properties != schema.end())
			{
				for (auto const& [name, propertySchema] : properties->items())
				{
					// Required properties are left out now and then, to exercise validation.
					bool const include = required.contains(name) ? m_Random.Pick(20) != 0 : m_Random.Pick(2) == 0;
					if (include)
					{
						object[name] = Value(propertySchema, name, object, depth + 1);
					}
				}
			}
			if (auto const additional = schema.find("additionalProperties"); additional != schema.end() && *additional != false)
			{
				for (size_t count = m_Random.Pick(3); count > 0; count--)
				{
					object[Key(property)] = Value(additional->is_object() ? *additional : Json::object(), property, object, depth + 1);
				}
			}
			return object;
		}

		Json ArrayValue(Json const& schema, std::string_view property, Json const& siblings, int depth)
		{
			if (std::optional<Json> patch = Patch(property, siblings))
			{
				return *patch;
			}
			uint64_t const minimum = schema.value("minItems", uint64_t(0));
			uint64_t const maximum = std::min(schema.value("maxItems", uint64_t(4)), minimum + 4);
			Json const items = schema.value("items", Json::object());
			Json array = Json::array();
			for (uint64_t count = minimum + m_Random.Pick(maximum - minimum + 1); count > 0; count--)
			{
				array.push_back(Value(items, property, Json::object(), depth + 1));
			}
			return array;
		}

		Json StringValue(Json const& schema, std::string_view property, Json const& siblings)
		{
			if (auto const values = schema.find("enum"); values != schema.end() && m_Random.Pick(10) != 0)
			{
				return (*values)[m_Random.Pick(values->size())];
			}
			StringChoices const choices = Strings(property, siblings);
			bool const good = !choices.Good.empty() && m_Random.Pick(4) != 0;
			return PickFrom(good ? choices.Good : choices.Edge);
		}

		Json IntegerValue(Json const& schema, std::string_view property)
		{
			if (std::optional<int64_t> const id = GraphId(property))
			{
				return *id;
			}
			// telemetry.read's and telemetry.export's steps have no maximum, and the schema takes a number without a fraction
			// beyond uint64_t: now and then one such.
			if (m_Command.starts_with("telemetry.") && (property == "from" || property == "to") && m_Random.Pick(8) == 0)
			{
				return 1.0e30;
			}
			double const minimum = schema.value("minimum", -1000.0);
			double maximum = schema.value("maximum", 1000.0);
			if (property == "frames")
			{
				maximum = std::min(maximum, 5.0);
			}
			std::array<double, 6> const candidates = {minimum, minimum + 1.0, maximum,
			                                          0.0,     1.0,           minimum + static_cast<double>(m_Random.Pick(100))};
			return static_cast<int64_t>(std::clamp(candidates[m_Random.Pick(candidates.size())], minimum, maximum));
		}

		Json NumberValue(Json const& schema, std::string_view property)
		{
			// Canvas points: finite, huge and not finite (which no JSON text holds, but a request built in memory can).
			if (property == "at" || property == "center")
			{
				std::array<double, 7> const coordinates = {
					0.0, 120.5, -2400.0, 9.0e6, 1.0e8, std::numeric_limits<double>::quiet_NaN(), -std::numeric_limits<double>::infinity()};
				return coordinates[m_Random.Pick(coordinates.size())];
			}
			// Gamepad axes: the range input.set takes, and not a number, which no schema bound refuses.
			if (property == "axes")
			{
				std::array<double, 6> const positions = {-1.0, -0.25, 0.0, 0.5, 1.0, std::numeric_limits<double>::quiet_NaN()};
				return positions[m_Random.Pick(positions.size())];
			}
			double minimum = schema.value("minimum", -1.0e30);
			double maximum = schema.value("maximum", 1.0e30);
			if (property == "timeout")
			{
				maximum = std::min(maximum, 0.5);
			}
			if (property == "timestep")
			{
				minimum = std::max(minimum, 0.01);
			}
			std::array<double, 8> const candidates = {minimum, maximum, 0.0, 0.5, -1.0, 1.0e6, -1.0e6, (minimum + maximum) / 2.0};
			return std::clamp(candidates[m_Random.Pick(candidates.size())], minimum, maximum);
		}

		// project.copy-example never opens its copy (which would replace the fuzzed project), and its directory is mostly a fresh
		// one inside the project or the fixture's Outside folder, where copies may go; about one in ten stays as drawn, so bad
		// directories are still tried (they must fail without writing).
		void AdjustProjectParameters(Json& params)
		{
			if (m_Command != "project.copy-example")
			{
				return;
			}
			params["open"] = false;
			if (m_Random.Pick(10) == 0)
			{
				return;
			}
			std::string const name = "Copy" + std::to_string(m_Copies++);
			params["directory"] = m_Random.Pick(2) == 0 ? FileSystem::PathToUtf8(m_Outside / "Copies" / name) : "Copies/" + name;
		}

		// editor.recent mostly gets one of remove and clear (both are refused), and half the time removes a listed project: the
		// fixture lists one that is missing (SeedRecentProjects).
		void AdjustRecentParameters(Json& params)
		{
			if (params.contains("remove") && params.contains("clear") && m_Random.Pick(4) != 0)
			{
				params.erase(m_Random.Pick(2) == 0 ? "remove" : "clear");
			}
			if (params.contains("remove") && m_Random.Pick(2) == 0)
			{
				params["remove"] = FileSystem::PathToUtf8(m_Outside / "Missing" / "Missing.sproj");
			}
		}

		// physics.query refuses every parameter its kind has no use for, so the independent optional ones would almost never
		// make a query: four times in five the request keeps only what its kind uses and gains what it needs (a direction, a
		// shape), and one time in eight a coordinate is not finite, as a request built in memory can hold.
		void AdjustPhysicsQueryParameters(Json& params)
		{
			auto const kind = params.find("kind");
			if (kind == params.end() || !kind->is_string() || m_Random.Pick(5) == 0)
			{
				return;
			}
			std::string const kindName = kind->get<std::string>();
			bool const isRay = kindName == "raycast" || kindName == "raycastAll";
			bool const moves = kindName != "overlap";
			if (isRay)
			{
				for (char const* const name : {"shape", "radius", "halfExtents", "halfHeight", "rotation", "rotationEuler"})
				{
					params.erase(name);
				}
			}
			else
			{
				if (!params.contains("shape"))
				{
					std::array<char const*, 3> const shapes = {"sphere", "box", "capsule"};
					params["shape"] = shapes[m_Random.Pick(shapes.size())];
				}
				std::string const shape = params["shape"].is_string() ? params["shape"].get<std::string>() : std::string();
				if (shape == "box")
				{
					params.erase("radius");
				}
				if (shape != "box")
				{
					params.erase("halfExtents");
				}
				if (shape != "capsule")
				{
					params.erase("halfHeight");
				}
				if (shape == "sphere")
				{
					params.erase("rotation");
					params.erase("rotationEuler");
				}
				if (params.contains("rotation") && params.contains("rotationEuler"))
				{
					params.erase(m_Random.Pick(2) == 0 ? "rotation" : "rotationEuler");
				}
			}
			if (moves && !params.contains("direction"))
			{
				params["direction"] = Json::array({0.0, -1.0, 0.0});
			}
			if (!moves)
			{
				params.erase("direction");
				params.erase("distance");
			}
			if (kindName == "raycast" || kindName == "shapeCast")
			{
				params.erase("maxHits");
			}
			if (kindName == "raycast")
			{
				params.erase("triggers");
			}
			if (params.contains("origin") && params["origin"].is_array() && params["origin"].size() == 3 && m_Random.Pick(8) == 0)
			{
				std::array<double, 3> const unreadable = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
				                                          -std::numeric_limits<double>::infinity()};
				params["origin"][m_Random.Pick(3)] = unreadable[m_Random.Pick(unreadable.size())];
			}
		}

		// The work the case checks that the stream would otherwise reach only by chance: until a goal has been reached once, its
		// command is sent a request that succeeds. graph.find gets a text alone (every script is searched, and one that does not
		// load is reported, not refused), then the references of the state's script's first variable; graph.set the state's
		// script's own document; graph.create, every data asset type's create and profiler.capture a file of a name not given
		// before (the drawn names exist after their first success), a data asset with its type's defaults; audio.meters,
		// telemetry.start, telemetry.stop and telemetry.channels no parameters, and vehicle.state none (it answers while the
		// scene plays); telemetry.read a channel of the newest session that has one, and telemetry.export that session to a
		// fresh file; project.copy-example
		// the available example into a fresh directory, unopened; graph.palette, whose goals are refusals, the two below;
		// input.set an override of the project's first action, then a gamepad's button. A request that
		// still fails (the state can be a few rounds old, the profiler can hold no frames yet) is followed by another at the command's next
		// round, so reaching every goal needs only a round or two of its command, whatever the other draws and the commands registered.
		// Nothing is drawn here, so every other request stays as the stream made it.
		std::optional<Json> SureRequest()
		{
			std::string const command(m_Command);
			if (command == "graph.find" && !m_Reached.contains("graph.find query"))
			{
				return Json::object({{"query", "Get"}});
			}
			if (command == "graph.find" && !m_Reached.contains("graph.find references") && !m_State.Graph.empty() &&
			    !m_State.GraphVariables.empty())
			{
				return Json::object(
					{{"references",
				      Json::object({{"kind", "Variable"}, {"graph", m_State.Graph}, {"id", m_State.GraphVariables.front()}})}});
			}
			// graph.palette opens nowhere headless, so its goals are its two refusals: an at beyond the canvas (its own
			// parameter check), then a request its checks pass, which the missing runtime refuses.
			if (command == "graph.palette" && !m_State.OpenGraphs.empty())
			{
				if (!m_Reached.contains("graph.palette InvalidParams"))
				{
					return Json::object(
						{{"graph", m_State.OpenGraphs.front()}, {"at", Json::array({VisualScriptSerializer::MaxCoordinate * 2.0, 0.0})}});
				}
				if (!m_Reached.contains("graph.palette Unavailable"))
				{
					return Json::object({{"graph", m_State.OpenGraphs.front()}});
				}
			}
			// input.set's two device kinds of this track: an override of the project's first action, then a gamepad's button.
			if (command == "input.set" && !m_State.ActionNames.empty() && !m_Reached.contains("input.set actions"))
			{
				return Json::object({{"actions", Json::object({{m_State.ActionNames.front(), true}})}});
			}
			if (command == "input.set" && !m_Reached.contains("input.set gamepads"))
			{
				return Json::object({{"gamepads", Json::object({{"0", Json::object({{"buttons", Json::object({{"A", true}})}})}})}});
			}
			if (m_Reached.contains(command))
			{
				return std::nullopt;
			}
			if (command == "audio.meters" || command == "telemetry.start" || command == "telemetry.stop" ||
			    command == "telemetry.channels" || command == "vehicle.state")
			{
				return Json::object();
			}
			// telemetry.read a channel of the newest session that has one (the fixture seeds one), telemetry.export that
			// session to a file of a name not given before.
			if (command == "telemetry.read" || command == "telemetry.export")
			{
				std::deque<TelemetrySession> const& sessions = Telemetry::GetSessions();
				auto const recorded = std::find_if(sessions.rbegin(), sessions.rend(),
				                                   [](TelemetrySession const& session)
				                                   {
													   return !session.Channels.empty();
												   });
				if (recorded == sessions.rend())
				{
					return std::nullopt;
				}
				if (command == "telemetry.read")
				{
					return Json::object({{"session", recorded->Id}, {"channels", Json::array({recorded->Channels.front().Name})}});
				}
				return Json::object({{"session", recorded->Id}, {"path", "Captures/Sure" + std::to_string(m_SureFiles++) + ".csv"}});
			}
			if (command == "graph.set" && !m_State.Graph.empty())
			{
				return Json::object({{"graph", m_State.Graph}, {"document", m_State.GraphDocument}});
			}
			if (command == "graph.create")
			{
				return Json::object({{"path", "Scripts/Sure" + std::to_string(m_SureFiles++) + ".sgraph"}});
			}
			if (command == "profiler.capture" && Profiler::GetStatus().Available)
			{
				return Json::object({{"path", "Captures/Sure" + std::to_string(m_SureFiles++) + ".json"}});
			}
			// The test content's available example, into a directory not given before, never opened (an open copy would replace
			// the fuzzed project).
			if (command == "project.copy-example")
			{
				return Json::object({{"example", "tiny"}, {"directory", "Copies/Sure" + std::to_string(m_SureFiles++)}, {"open", false}});
			}
			if (DataAssetType const* type = FindDataAssetCommandType(m_Command); type != nullptr && command.ends_with(".create"))
			{
				return Json::object(
					{{"path", "Data/Sure" + std::to_string(m_SureFiles++) + type->GetExtension()}, {"fields", type->GetDefaultFields()}});
			}
			return std::nullopt;
		}

		// A data asset type's set: a third of the time an asset of the type with the fields the state read, undamaged and alone,
		// which succeeds; the independent parameters and the damaged patches otherwise leave too few that do in a run's few
		// rounds of the command.
		void AdjustDataAssetParameters(Json& params)
		{
			DataAssetType const* const type = FindDataAssetCommandType(m_Command);
			if (type == nullptr || !m_Command.ends_with(".set") || m_Random.Pick(3) != 0)
			{
				return;
			}
			auto const assets = m_State.AssetsByType.find(type->GetName());
			auto const fields = m_State.DataAssetFields.find(type->GetName());
			if (assets == m_State.AssetsByType.end() || assets->second.empty() || fields == m_State.DataAssetFields.end())
			{
				return;
			}
			params = Json::object({{type->GetCommandParameter(), PickFrom(assets->second)}, {"fields", fields->second}});
		}

		// Parameters of the graph commands that combine as the commands expect most of the time, so their work is reached in a
		// run's few rounds of each command: half of them name the state's script, graph.find gets one of query and references
		// (each half the time when it had neither), graph.open selects elements of the sub-graph it shows (and half the time
		// opens the state's script with no other parameter, which its many independent ones would refuse), graph.arrange and
		// graph.copy name nodes and elements of one sub-graph and mostly no revision, graph.set mostly no revision,
		// graph.close mostly closes one window, and graph.debug-panel is often called without a stop or a frame.
		void AdjustGraphParameters(Json& params)
		{
			// Half the time the script whose document the state read, which every graph command can work on.
			if (params.contains("graph") && !m_State.Graph.empty() && m_Random.Pick(2) == 0)
			{
				params["graph"] = m_State.Graph;
			}
			if (m_Command == "graph.find" && m_Random.Pick(4) != 0)
			{
				if (params.contains("query") && params.contains("references"))
				{
					params.erase(m_Random.Pick(2) == 0 ? "query" : "references");
				}
				else if (!params.contains("query") && !params.contains("references"))
				{
					std::optional<Json> references = m_Random.Pick(2) == 0 ? Patch("references", params) : std::nullopt;
					if (references)
					{
						params["references"] = std::move(*references);
					}
					else
					{
						std::vector<std::string> const texts = {"s", "Get", "Speed", "Hit", "e"};
						params["query"] = PickFrom(texts);
					}
				}
			}
			// A third of the time the references of a variable of the state's script alone, which graph.find finds: the other
			// draws (the schema's independent members, damaged references) leave too few that succeed in a run's few rounds.
			if (m_Command == "graph.find" && !m_State.Graph.empty() && !m_State.GraphVariables.empty() && m_Random.Pick(3) == 0)
			{
				params = Json::object(
					{{"references",
				      Json::object({{"kind", "Variable"}, {"graph", m_State.Graph}, {"id", PickFrom(m_State.GraphVariables)}})}});
			}
			// graph.open: half the time the state's script alone, since a run draws it only about a dozen times and its many
			// independent members refuse most other draws.
			if (m_Command == "graph.open" && !m_State.Graph.empty() && m_Random.Pick(2) == 0)
			{
				params = Json::object({{"graph", m_State.Graph}});
			}
			// The debug commands that change or read one graph's breakpoints, target, values or live state: a third of the time the
			// state's script alone (listing and reading), which their independent members would mostly refuse together.
			if ((m_Command == "graph.breakpoints" || m_Command == "graph.target" || m_Command == "graph.values" ||
			     m_Command == "graph.live") &&
			    !m_State.Graph.empty() && m_Random.Pick(3) == 0)
			{
				params = Json::object({{"graph", m_State.Graph}});
			}
			// graph.debug-panel: a third of the time no stop and no frame (showing, hiding or reading the panel), which are refused
			// while nothing is stopped, as it mostly is in the fuzz.
			if (m_Command == "graph.debug-panel" && m_Random.Pick(3) == 0)
			{
				params.erase("stop");
				params.erase("frame");
			}
			if (m_Command == "graph.open" && params.contains("select") && m_Random.Pick(5) != 0)
			{
				auto const function = params.find("function");
				int64_t const shown = function != params.end() && function->is_number_integer() ? function->get<int64_t>() : 0;
				auto const elements = m_State.GraphElements.find(shown);
				Json select = Json::array();
				if (elements != m_State.GraphElements.end() && !elements->second.empty())
				{
					for (size_t count = 1 + m_Random.Pick(3); count > 0; count--)
					{
						select.push_back(PickFrom(elements->second));
					}
				}
				params["select"] = std::move(select);
			}
			// graph.arrange's nodes and graph.copy's elements: mostly of a sub-graph of the script the state knows, which the call
			// then names (graph.arrange's nodes only).
			if ((m_Command == "graph.arrange" || m_Command == "graph.copy") && !m_State.Graph.empty() && m_Random.Pick(5) != 0)
			{
				std::vector<std::pair<int64_t, std::vector<int64_t>>> subGraphs;
				for (auto const& [function, elements] : m_State.GraphElements)
				{
					std::vector<int64_t> usable;
					std::copy_if(elements.begin(), elements.end(), std::back_inserter(usable),
					             [this](int64_t element)
					             {
									 return m_Command == "graph.copy" ||
						                    std::ranges::find(m_State.GraphNodes, element) != m_State.GraphNodes.end();
								 });
					if (!usable.empty())
					{
						subGraphs.emplace_back(function, std::move(usable));
					}
				}
				char const* const key = m_Command == "graph.arrange" ? "nodes" : "elements";
				if (!subGraphs.empty())
				{
					auto const& [function, elements] = subGraphs[m_Random.Pick(subGraphs.size())];
					params["graph"] = m_State.Graph;
					params["function"] = function;
					// graph.arrange arranges every node of the sub-graph without nodes.
					if (params.contains(key) || m_Command == "graph.copy")
					{
						Json listed = Json::array();
						for (size_t count = 1 + m_Random.Pick(4); count > 0; count--)
						{
							listed.push_back(PickFrom(elements));
						}
						params[key] = std::move(listed);
					}
				}
				// A revision from the schema is a guess that names no document: mostly none, so the arranging is reached.
				if (m_Random.Pick(4) != 0)
				{
					params.erase("revision");
				}
			}
			// graph.set: mostly no revision and no program revision (a guess names no document), the state's script's own now
			// and then, so the replacing and its referrers are reached.
			if (m_Command == "graph.set")
			{
				for (char const* const key : {"revision", "programRevision"})
				{
					if (m_Random.Pick(4) != 0)
					{
						params.erase(key);
					}
				}
				if (params.contains("graph") && params["graph"] == m_State.Graph && !m_State.GraphRevision.empty() && m_Random.Pick(4) == 0)
				{
					params["revision"] = m_State.GraphRevision;
				}
				// A third of the time the state's script with its own document, described anew, alone: a replacing that succeeds,
				// which the independent parameters and the damaged documents otherwise leave too rare in a run's few rounds.
				if (!m_State.Graph.empty() && !m_State.GraphDocument.empty() && m_Random.Pick(3) == 0)
				{
					Json document = m_State.GraphDocument;
					document["Description"] = "Set by the fuzz";
					params = Json::object({{"graph", m_State.Graph}, {"document", std::move(document)}});
				}
			}
			// graph.edit: mostly the state's script, whose sub-graph the batch (Patch("operations")) drew its elements from; mostly
			// no revision (a guess names no document), and the current program revision half the time.
			if (m_Command == "graph.edit")
			{
				if (!m_State.Graph.empty() && m_Random.Pick(5) != 0)
				{
					params["graph"] = m_State.Graph;
					if (m_BatchFunction != 0 || params.contains("function"))
					{
						params["function"] = m_BatchFunction;
					}
				}
				if (m_Random.Pick(4) != 0)
				{
					params.erase("revision");
				}
				if (!m_State.GraphProgramRevision.empty() && m_Random.Pick(2) == 0)
				{
					params["programRevision"] = m_State.GraphProgramRevision;
				}
				else if (m_Random.Pick(4) != 0)
				{
					params.erase("programRevision");
				}
				// The first call, and now and then another, is right in every part (one operation that needs no scripting
				// runtime), so the command's work is reached whatever else the drawn batches and parameters get wrong, and
				// whichever commands the registry holds (each one changes the order the commands are drawn in).
				if (!m_State.Graph.empty() && (!m_RightEditDrawn || m_Random.Pick(6) == 0))
				{
					m_RightEditDrawn = true;
					params =
						Json::object({{"graph", m_State.Graph},
					                  {"operations", Json::array({Json::object({{"op", "SetDescription"}, {"description", "Fuzzed"}})})}});
				}
			}
			// graph.palette opens only in an open window: mostly one, mostly without the from and at that the schema draws, which
			// rarely name a node and a point of the canvas, so the palette's own checks are reached (whatever the order of the
			// commands drawn before it).
			if (m_Command == "graph.palette" && !m_State.OpenGraphs.empty() && m_Random.Pick(4) != 0)
			{
				params["graph"] = PickFrom(m_State.OpenGraphs);
				if (m_Random.Pick(4) != 0)
				{
					params.erase("from");
				}
				if (m_Random.Pick(2) == 0)
				{
					params.erase("at");
				}
			}
			if (m_Command == "graph.close" && !params.contains("graph") && !m_State.OpenGraphs.empty() && m_Random.Pick(4) != 0)
			{
				params["graph"] = PickFrom(m_State.OpenGraphs);
			}
		}

		// The IDs graph parameters name: those of the document's sub-graphs, nodes and comments, variables and functions, and
		// IDs it does not have.
		std::optional<int64_t> GraphId(std::string_view property)
		{
			if (!m_Command.starts_with("graph."))
			{
				return std::nullopt;
			}
			std::vector<int64_t> choices;
			if (property == "function")
			{
				choices = m_State.GraphFunctions;
				choices.push_back(0);
			}
			else if (property == "select")
			{
				// Mostly the event graph's, which graph.open shows unless it is given another.
				auto const elements = m_State.GraphElements.find(
					m_Random.Pick(3) == 0 && !m_State.GraphFunctions.empty() ? PickFrom(m_State.GraphFunctions) : 0);
				if (elements != m_State.GraphElements.end())
				{
					choices = elements->second;
				}
			}
			else if (property == "node" || property == "timeline" || property == "remove" || property == "nodes" || property == "show")
			{
				// graph.open's timeline: the seed's nodes, of which those of other types answer InvalidParams. The debug commands'
				// breakpoint nodes (graph.breakpoints' add and remove) and nodes (graph.trace, graph.values) are the seed's too.
				choices = m_State.GraphNodes;
			}
			else if (property == "id")
			{
				choices = m_State.GraphReferences;
			}
			else
			{
				return std::nullopt;
			}
			// IDs the document does not have.
			choices.insert(choices.end(), {999999, 2147483647});
			return PickFrom(choices);
		}

		// An input action's value for input.set: each shape an action type takes (Button and Axis actions a number or a bool,
		// Vector2 actions [x, y]), null (clears), and values no action takes (too large for a float, not finite, the wrong
		// shape).
		Json ActionValue()
		{
			std::array<Json, 12> const values = {Json(true),
			                                     Json(false),
			                                     Json(1.0),
			                                     Json(-0.5),
			                                     Json(0.0),
			                                     Json::array({0.6, -0.8}),
			                                     Json::array({0.0, 0.0}),
			                                     Json(),
			                                     Json(1.0e308),
			                                     Json(std::numeric_limits<double>::quiet_NaN()),
			                                     Json::array({1.0}),
			                                     Json("up")};
			return values[m_Random.Pick(values.size())];
		}

		// A value of any kind, which the schema most likely rejects.
		Json InvalidValue()
		{
			std::array<Json, 11> const values = {Json(),
			                                     Json(true),
			                                     Json(-1),
			                                     Json(1.0e308),
			                                     Json(1.5),
			                                     Json(""),
			                                     Json("text"),
			                                     Json::array(),
			                                     Json::array({1, 2, 3}),
			                                     Json::object(),
			                                     Json::object({{"unexpected", 1}})};
			return values[m_Random.Pick(values.size())];
		}

		// Patches of component, material, scene and project fields: real ones, damaged now and then.
		std::optional<Json> Patch(std::string_view property, Json const& siblings)
		{
			if (property == "fields")
			{
				if (m_Command.starts_with("material."))
				{
					return Damage(m_State.MaterialFields);
				}
				if (DataAssetType const* type = FindDataAssetCommandType(m_Command))
				{
					auto const fields = m_State.DataAssetFields.find(type->GetName());
					return Damage(fields != m_State.DataAssetFields.end() ? fields->second : type->GetDefaultFields());
				}
				auto const component = siblings.find("component");
				std::string const name =
					component != siblings.end() && component->is_string() ? component->get<std::string>() : PickFrom(m_State.Components);
				auto const fields = m_State.ComponentFields.find(name);
				return Damage(fields != m_State.ComponentFields.end() ? fields->second : Json::object());
			}
			if (property == "components")
			{
				Json components = Json::object();
				for (size_t count = 1 + m_Random.Pick(2); count > 0; count--)
				{
					std::string const name = PickFrom(m_State.Components);
					auto const fields = m_State.ComponentFields.find(name);
					components[name] = Damage(fields != m_State.ComponentFields.end() ? fields->second : Json::object());
				}
				return components;
			}
			if (property == "settings" && m_Command != "graph.edit")
			{
				return Damage(m_Command == "project.settings" ? m_State.ProjectSettings : m_State.SceneSettings);
			}
			// graph.edit's batch, drawn from the state's script.
			if (property == "operations" && m_Command == "graph.edit")
			{
				return GraphOperations();
			}
			// graph.edit's Paste: the text of the last graph.copy, damaged half the time.
			if (property == "clipboard")
			{
				std::string text = m_State.GraphClipboard;
				if (!text.empty() && m_Random.Pick(2) == 0)
				{
					text.resize(m_Random.Pick(text.size()));
				}
				return Json(std::move(text));
			}
			// graph.find's references: an element of the document, mostly with the ID of an element of its kind (a variable's, a
			// function's), a member left out now and then.
			if (property == "references" && m_Random.Pick(4) != 0)
			{
				auto const graphs = m_State.AssetsByType.find("VisualScript");
				std::array<char const*, 3> const kinds = {"Variable", "Function", "CustomEvent"};
				size_t const kind = m_Random.Pick(kinds.size());
				Json reference = Json::object({{"kind", kinds[kind]}});
				std::vector<int64_t> const& ofKind = kind == 0   ? m_State.GraphVariables
				                                     : kind == 1 ? m_State.GraphFunctions
				                                                 : m_State.GraphReferences;
				// Mostly the state's script, which has the IDs below: the project has a library and other scripts too.
				if (!m_State.Graph.empty() && m_Random.Pick(4) != 0)
				{
					reference["graph"] = m_State.Graph;
				}
				else if (graphs != m_State.AssetsByType.end() && m_Random.Pick(8) != 0)
				{
					reference["graph"] = PickFrom(graphs->second);
				}
				if (!m_State.GraphReferences.empty() && m_Random.Pick(8) != 0)
				{
					reference["id"] = PickFrom(ofKind.empty() || m_Random.Pick(8) == 0 ? m_State.GraphReferences : ofKind);
				}
				if (!m_State.GraphEvents.empty() && m_Random.Pick(8) != 0)
				{
					reference["name"] = PickFrom(m_State.GraphEvents);
				}
				return reference;
			}
			// graph.open's reveal: a variable or local, a function or a custom event's node of the state's script, now and then
			// an element of another kind or one it does not have.
			if (property == "reveal" && m_Random.Pick(4) != 0)
			{
				std::array<char const*, 3> const kinds = {"Variable", "Function", "CustomEvent"};
				size_t const kind = m_Random.Pick(kinds.size());
				std::vector<int64_t> choices = kind == 2 ? m_State.GraphNodes : m_State.GraphReferences;
				choices.push_back(999999);
				return Json::object({{"kind", kinds[kind]}, {"id", PickFrom(choices)}});
			}
			// A whole visual script document (graph.set, graph.check), damaged half the time.
			if (property == "document")
			{
				Json document = m_State.GraphDocument;
				if (m_Random.Pick(2) == 0)
				{
					m_Mutator.Mutate(document);
				}
				return document;
			}
			return std::nullopt;
		}

		// A graph.edit batch of 1 to 6 operations on one sub-graph of the state's script (which AdjustGraphParameters names):
		// the operations that need no scripting runtime (the fixture has none) and those that run degraded without it
		// (deletions, measured layout), with labels for what they add, and now and then a damaged one (a field the operation
		// does not take, a label used before it is defined, an ID an earlier operation deleted).
		Json GraphOperations()
		{
			m_BatchFunction = !m_State.GraphFunctions.empty() && m_Random.Pick(4) == 0 ? PickFrom(m_State.GraphFunctions) : 0;
			std::vector<int64_t> const none;
			auto const found = m_State.GraphElements.find(m_BatchFunction);
			std::vector<int64_t> const& elements = found != m_State.GraphElements.end() ? found->second : none;
			auto const foundComments = m_State.GraphComments.find(m_BatchFunction);
			std::vector<int64_t> const& comments = foundComments != m_State.GraphComments.end() ? foundComments->second : none;
			std::vector<int64_t> nodes;
			std::copy_if(elements.begin(), elements.end(), std::back_inserter(nodes),
			             [&comments](int64_t element)
			             {
							 return std::ranges::find(comments, element) == comments.end();
						 });
			auto const links = m_State.GraphLinks.find(m_BatchFunction);
			std::vector<std::string> labels;
			std::vector<int64_t> deleted;
			auto const node = [&]() -> Json
			{
				if (!labels.empty() && m_Random.Pick(4) == 0)
				{
					return PickFrom(labels);
				}
				return nodes.empty() ? Json(999999) : Json(PickFrom(nodes));
			};
			auto const element = [&]() -> Json
			{
				return !comments.empty() && m_Random.Pick(4) == 0 ? Json(PickFrom(comments)) : node();
			};
			auto const label = [&](Json& operation)
			{
				if (m_Random.Pick(2) == 0)
				{
					labels.push_back("$fuzz" + std::to_string(labels.size()));
					operation["as"] = labels.back();
				}
			};
			auto const point = [&]
			{
				return Json::array({static_cast<double>(m_Random.Pick(4000)) - 2000.0, static_cast<double>(m_Random.Pick(4000)) - 2000.0});
			};
			std::vector<std::string> const types = {
				"Flow.Branch", "Flow.Sequence", "Operator.Add", "Event.Custom", "Call:Strada.Log.Info(string)",
				"Flow.Delay",  "Expression",    "Array.Make",   "Flow.Timeline"};
			std::vector<std::string> const kinds = {
				"AddNode",    "Move",       "SetPosition",      "SetInput",    "SetSetting",  "Disconnect",     "InsertReroute",
				"AddComment", "SetComment", "AddVariable",      "SetVariable", "Delete",      "DeleteVariable", "DeleteFunction",
				"Arrange",    "Paste",      "SetArrayElements", "SetFormula",  "SetTimeline", "Retarget",       "SetKind"};
			std::vector<std::string> const formulas = {"a * b + sin(t)", "speed * dt", "velocity * dt", "x + (", ""};
			Json batch = Json::array();
			for (size_t count = 1 + m_Random.Pick(6); count > 0; count--)
			{
				std::string const& kind = PickFrom(kinds);
				Json operation = Json::object({{"op", kind}});
				if (kind == "AddNode")
				{
					operation["type"] = PickFrom(types);
					if (operation["type"] == "Expression")
					{
						operation["settings"] = Json::object({{"Formula", PickFrom(formulas)}});
					}
					if (m_Random.Pick(2) == 0)
					{
						operation["position"] = point();
					}
					label(operation);
				}
				else if (kind == "SetArrayElements")
				{
					// A node of another type answers InvalidParams, as does a count the node cannot take.
					operation["node"] = node();
					operation["count"] = m_Random.Pick(6);
					if (m_Random.Pick(3) == 0)
					{
						operation[m_Random.Pick(2) == 0 ? "insertAt" : "removeAt"] = m_Random.Pick(3);
					}
				}
				else if (kind == "SetFormula")
				{
					operation["node"] = node();
					operation["formula"] = PickFrom(formulas);
				}
				else if (kind == "Retarget")
				{
					// The seed's typed Send, library call and remote Get, and nodes that name no graph (InvalidParams): another
					// graph, the library, this script itself, or none.
					operation["node"] = node();
					std::array<Json, 4> const graphs = {Json(), Json(std::string("asset://") + SeedLibraryPath),
					                                    m_State.Graph.empty() ? Json() : Json(m_State.Graph), Json("8146390251172604461")};
					operation["settings"] = Json::object({{"Graph", graphs[m_Random.Pick(graphs.size())]}});
				}
				else if (kind == "SetKind")
				{
					operation["kind"] = m_Random.Pick(2) == 0 ? "Script" : "Library";
				}
				else if (kind == "SetTimeline")
				{
					// A node of another type answers InvalidParams, as does a damaged timeline.
					std::vector<std::string> const timelines = {
						R"({"Length": 2, "Tracks": [{"Name": "Lift", "Type": "float", "Keys": [{"Time": 0, "Value": 1}]}]})",
						R"({"Length": 1, "Loop": true, "Tracks": [{"Name": "Step", "Type": "exec", "Keys": [{"Time": 0.5}]}]})",
						R"({"Length": 0})"};
					operation["node"] = node();
					operation["timeline"] = Json::parse(PickFrom(timelines));
				}
				else if (kind == "Move")
				{
					operation["elements"] = Json::array({element(), element()});
					operation["offset"] = point();
				}
				else if (kind == "SetPosition")
				{
					operation["element"] = element();
					operation["position"] = point();
				}
				else if (kind == "SetInput")
				{
					operation["node"] = node();
					operation["pin"] = m_State.GraphPins.empty() ? std::string("Value") : PickFrom(m_State.GraphPins);
					std::array<Json, 4> const values = {Json(1), Json("Fuzz"), Json(true), Json()};
					operation["value"] = values[m_Random.Pick(values.size())];
				}
				else if (kind == "SetSetting")
				{
					operation["node"] = node();
					operation["name"] = m_Random.Pick(2) == 0 ? "Count" : "Name";
					operation["value"] = operation["name"] == "Count" ? Json(2 + m_Random.Pick(3)) : Json("Fuzzed");
				}
				else if (kind == "Disconnect" || kind == "InsertReroute")
				{
					Json const link =
						links != m_State.GraphLinks.end() && !links->second.empty()
							? PickFrom(links->second)
							: Json::object({{"from", Json::array({node(), "Then"})}, {"to", Json::array({node(), "Execute"})}});
					if (kind == "Disconnect")
					{
						operation["from"] = link["from"];
						operation["to"] = link["to"];
					}
					else
					{
						operation["link"] = link;
						operation["position"] = point();
						label(operation);
					}
				}
				else if (kind == "AddComment")
				{
					operation["text"] = "Fuzz";
					if (m_Random.Pick(2) == 0)
					{
						operation["elements"] = Json::array({element(), element()});
					}
					label(operation);
				}
				else if (kind == "SetComment")
				{
					operation["comment"] = comments.empty() ? Json(999999) : Json(PickFrom(comments));
					operation["text"] = "Fuzzed";
				}
				else if (kind == "AddVariable")
				{
					operation["name"] = "Fuzz" + std::to_string(m_Random.Pick(1000));
					operation["type"] = m_Random.Pick(2) == 0 ? "int" : "float";
					operation["value"] = 1;
				}
				else if (kind == "SetVariable" || kind == "DeleteVariable")
				{
					bool const named = !m_State.GraphVariableNames.empty() && m_Random.Pick(2) == 0;
					operation["variable"] = named                            ? Json(PickFrom(m_State.GraphVariableNames))
					                        : m_State.GraphVariables.empty() ? Json("Nope")
					                                                         : Json(PickFrom(m_State.GraphVariables));
					if (kind == "SetVariable")
					{
						operation["tooltip"] = "Fuzzed";
					}
				}
				else if (kind == "DeleteFunction")
				{
					operation["function"] = m_State.GraphFunctionNames.empty() ? Json("Nope") : Json(PickFrom(m_State.GraphFunctionNames));
				}
				else if (kind == "Delete")
				{
					operation["elements"] = Json::array({element()});
					if (operation["elements"][0].is_number_integer())
					{
						deleted.push_back(operation["elements"][0].get<int64_t>());
					}
				}
				else if (kind == "Arrange")
				{
					operation["scope"] = m_Random.Pick(2) == 0 ? "Graph" : "Created";
					operation["extents"] = m_Random.Pick(2) == 0 ? "Measured" : "Uniform";
				}
				else
				{
					operation["clipboard"] = *Patch("clipboard", operation);
				}
				// Now and then a damaged operation.
				if (m_Random.Pick(10) == 0)
				{
					switch (m_Random.Pick(3))
					{
						case 0:
							operation["nodeId"] = 1;
							break;
						case 1:
							operation["node"] = "$forward";
							break;
						default:
							operation["elements"] = Json::array({deleted.empty() ? int64_t(999999) : deleted.back()});
							break;
					}
				}
				batch.push_back(std::move(operation));
			}
			return batch;
		}

		// Some of the members of a document, damaged half the time.
		Json Damage(Json const& document)
		{
			Json patch = Json::object();
			for (auto const& [name, value] : document.items())
			{
				if (m_Random.Pick(3) == 0)
				{
					patch[name] = value;
				}
			}
			if (!patch.empty() && m_Random.Pick(2) == 0)
			{
				m_Mutator.Mutate(patch);
			}
			return patch;
		}

		std::string Key(std::string_view property)
		{
			if (property == "keys")
			{
				return PickFrom(KeyNames);
			}
			if (property == "mouseButtons")
			{
				return PickFrom(MouseButtonNames);
			}
			if (property == "actions")
			{
				return !m_State.ActionNames.empty() && m_Random.Pick(4) != 0 ? PickFrom(m_State.ActionNames) : PickFrom(BadActionNames);
			}
			if (property == "gamepads")
			{
				return m_Random.Pick(5) != 0 ? PickFrom(GamepadSlots) : PickFrom(BadGamepadSlots);
			}
			if (property == "buttons")
			{
				return PickFrom(GamepadButtonNames);
			}
			if (property == "axes")
			{
				return PickFrom(GamepadAxisNames);
			}
			return m_Random.Pick(2) == 0 ? PickFrom(m_State.Components) : PickFrom(GenericStrings);
		}

		StringChoices Strings(std::string_view property, Json const& siblings)
		{
			std::vector<std::string> const invalidIds = {"", "0", "not-an-id", "18446744073709551615", "-1"};
			std::vector<std::string> const invalidAssets = {"", "0", "asset://Missing.smat", "builtin://Nothing", "asset://../Escape.smat"};
			std::vector<std::string> const invalidFolders = {"", ".", "..", "../Escape", "../../Escape", "Materials/../..", "C:Escape"};
			if (property == "entity" || property == "parent" || property == "primary" || property == "entities")
			{
				return {m_State.Entities, invalidIds};
			}
			// telemetry.read's and telemetry.export's channels (the recorder's, the fixture's seeded session among them) and laps
			// (the seeded session's, the best one, and ones no session has).
			if (m_Command.starts_with("telemetry.") && property == "channels")
			{
				return {TelemetryChannelNames(), {"", "Missing", std::string(70, 'c'), "fuzz.speed"}};
			}
			if (m_Command.starts_with("telemetry.") && property == "lap")
			{
				return {{"best", "Lap 1"}, {"", "Lap 9", "Lap end", "best ", "Lap 01"}};
			}
			// project.copy-example's examples are the test content's (Tests/Data/FrontDoor/Content/Examples).
			if (property == "example")
			{
				return {{"tiny", "fetched"}, {"", "nope", "Tiny", "../tiny", "showcase"}};
			}
			// Its directories as drawn (AdjustProjectParameters replaces most): ones taken, climbing out, the project's, and its
			// asset directory in another letter case (refused where the file system ignores case, a new folder elsewhere).
			if (property == "directory")
			{
				return {
					{"Copies/Drawn", FileSystem::PathToUtf8(m_Outside / "Drawn")},
					{"", ".", "..", "../..", "Assets", "Assets/Copy", "assets/Copy", FileSystem::PathToUtf8(m_Project), "C:Escape", "/"}};
			}
			if (property == "component")
			{
				std::vector<std::string> const unknown = {"Nope", "", "transform"};
				// The components of the entity the command names (except for adding one).
				auto const entity = siblings.find("entity");
				if (entity != siblings.end() && entity->is_string() && m_Command != "component.add")
				{
					auto const components = m_State.EntityComponents.find(entity->get<std::string>());
					if (components != m_State.EntityComponents.end())
					{
						return {components->second, unknown};
					}
				}
				return {m_State.Components, unknown};
			}
			if (property == "asset")
			{
				return {m_State.Assets, invalidAssets};
			}
			// A data asset type's commands: mostly its assets, now and then one of another type.
			if (DataAssetType const* type = FindDataAssetParameterType(property))
			{
				auto const typed = m_State.AssetsByType.find(type->GetName());
				std::vector<std::string> others = invalidAssets;
				auto const materials = m_State.AssetsByType.find("Material");
				if (materials != m_State.AssetsByType.end())
				{
					others.insert(others.end(), materials->second.begin(), materials->second.end());
				}
				return {typed != m_State.AssetsByType.end() ? typed->second : std::vector<std::string>(), others};
			}
			if (property == "material" || property == "prefab")
			{
				auto const typed = m_State.AssetsByType.find(property == "material" ? "Material" : "Prefab");
				return {typed != m_State.AssetsByType.end() ? typed->second : std::vector<std::string>(), invalidAssets};
			}
			// The palette needs an open window, and closing one that is open does more than closing one that is not.
			if (property == "graph" && (m_Command == "graph.palette" || m_Command == "graph.close") && !m_State.OpenGraphs.empty())
			{
				return {m_State.OpenGraphs, invalidAssets};
			}
			if (property == "graph")
			{
				auto const graphs = m_State.AssetsByType.find("VisualScript");
				return {graphs != m_State.AssetsByType.end() ? graphs->second : std::vector<std::string>(), invalidAssets};
			}
			if (property == "scene")
			{
				return {m_State.ScenePaths, {"Missing.sscene", "../Escape.sscene", ""}};
			}
			if (property == "folder")
			{
				return {m_State.Folders, invalidFolders};
			}
			if (property == "newFolder")
			{
				return {{"New", "New/Deeper", "Moved", "Materials/Sub"}, invalidFolders};
			}
			if (property == "files")
			{
				return {{m_ImportFiles[0], m_ImportFiles[1]}, {m_ImportFiles[2], "Relative.png", ""}};
			}
			// asset.drop: files and the folder holding them.
			if (property == "paths")
			{
				return {{m_ImportFiles[0], m_ImportFiles[1], m_ImportFiles[3]}, {m_ImportFiles[2], "Relative.png", ""}};
			}
			if (property == "path")
			{
				return Paths(siblings);
			}
			// The debug commands' breakpoint conditions and evaluated formulas: formulas of the seed's names, the comparisons only
			// conditions have, and garbage.
			if (property == "condition" || property == "evaluate")
			{
				std::vector<std::string> formulas = {"",     "Speed > 1", "Index == 3", "State == \"Serve\"", "Target != None",
				                                     "true", "1 + 2"};
				for (std::string const& name : m_State.GraphVariableNames)
				{
					formulas.push_back(name + " > 0");
				}
				return {formulas, {"((", "Speed >", "nope", std::string(1100, 'x'), "\"unterminated"}};
			}
			if (property == "graphs")
			{
				auto const graphs = m_State.AssetsByType.find("VisualScript");
				return {graphs != m_State.AssetsByType.end() ? graphs->second : std::vector<std::string>(), invalidAssets};
			}
			// graph.palette's pins and graph.find's texts and custom events.
			if (property == "pin")
			{
				return {m_State.GraphPins, {"", "Hit.", ".Point", "A..B", "Nope"}};
			}
			if (property == "query" && m_Command == "graph.find")
			{
				return {{"s", "Get", "Speed", "Hit", "e"}, {"", "名前", "nothing like it"}};
			}
			if (property == "name" && m_Command == "graph.find")
			{
				return {m_State.GraphEvents, {"Nope", "  "}};
			}
			// graph.open's detailsRow: labels of Details' rows (for the node selected, or the script), and labels it draws for no
			// subject.
			if (property == "detailsRow")
			{
				return {{"Formula", "Comment", "Settings", "Length", "Name", "Description", "Then1"},
				        {"No Such Row", " ", std::string(300, 'x')}};
			}
			if (property == "name")
			{
				std::vector<std::string> names = m_State.EntityNames;
				names.push_back("Fuzz");
				return {names, GenericStrings};
			}
			return {{"Fuzz"}, GenericStrings};
		}

		// Paths the command at hand accepts (in the asset directory with the right extension, or absolute where that is
		// allowed), and ones it must refuse.
		StringChoices Paths(Json const& siblings)
		{
			std::vector<std::string> const refused = {"../Escape.sscene",       "../../Escape.smat", "../../../Escape.sprefab",
			                                          "../../../../Escape.png", "C:Escape.sscene",   "Fuzz.txt"};
			std::string const outside = FileSystem::PathToUtf8(m_Outside / "Fuzz.sscene");
			if (m_Command == "scene.open")
			{
				std::vector<std::string> scenes = m_State.ScenePaths;
				scenes.push_back(outside);
				return {scenes, refused};
			}
			if (m_Command == "scene.save")
			{
				return {{"Scenes/Fuzz.sscene", "Fuzz.sscene", outside}, refused};
			}
			if (m_Command == "material.create")
			{
				return {{"Materials/Fuzz.smat", "Fuzz.smat"}, refused};
			}
			if (m_Command == "prefab.create")
			{
				return {{"Prefabs/Fuzz.sprefab", "Fuzz.sprefab"}, refused};
			}
			if (m_Command == "graph.create")
			{
				return {{"Scripts/Fuzz.sgraph", "Fuzz.sgraph"}, refused};
			}
			// Captures go into the project (relative paths) or where an absolute path names; the fixture seeds one to open.
			std::vector<std::string> const refusedCaptures = {"../Escape.json", "../../Escape.json", "Captures/../../Escape.json",
			                                                  "Fuzz.txt",       "Captures",          ""};
			std::string const outsideCapture = FileSystem::PathToUtf8(m_Outside / "Fuzz.json");
			if (m_Command == "profiler.capture")
			{
				return {{"Captures/Fuzz.json", "Fuzz.json", "Captures/Seed.json", outsideCapture}, refusedCaptures};
			}
			if (m_Command == "telemetry.export")
			{
				std::vector<std::string> refusedExports = {"../Escape.csv", "../../Escape.csv", "Captures/../../Escape.csv",
				                                           "Fuzz.txt",      "Captures",         ""};
				return {{"Captures/Fuzz.csv", "Fuzz.csv", FileSystem::PathToUtf8(m_Outside / "Fuzz.csv")}, std::move(refusedExports)};
			}
			if (m_Command == "profiler.open")
			{
				std::vector<std::string> refusedOpens = refusedCaptures;
				refusedOpens.push_back("Missing.json");
				refusedOpens.push_back("Fuzz.sproj");
				return {{"Captures/Seed.json", "Captures/Fuzz.json", outsideCapture}, std::move(refusedOpens)};
			}
			if (DataAssetType const* type = FindDataAssetCommandType(m_Command); type != nullptr && m_Command.ends_with(".create"))
			{
				return {{"Data/Fuzz" + type->GetExtension(), "Fuzz" + type->GetExtension()}, refused};
			}
			// asset.move: a new place for the asset, under the same name.
			auto const asset = siblings.find("asset");
			if (asset != siblings.end() && asset->is_string())
			{
				if (auto const path = m_State.AssetPaths.find(asset->get<std::string>()); path != m_State.AssetPaths.end())
				{
					std::string const name = path->second.substr(path->second.rfind('/') + 1);
					return {{"Moved/" + name, "Moved/Deeper/" + name, name}, refused};
				}
			}
			return {{}, refused};
		}

		// The names of the channels of the sessions the recorder holds, each once.
		static std::vector<std::string> TelemetryChannelNames()
		{
			std::set<std::string> names;
			for (TelemetrySession const& session : Telemetry::GetSessions())
			{
				for (TelemetryChannel const& channel : session.Channels)
				{
					names.insert(channel.Name);
				}
			}
			return {names.begin(), names.end()};
		}

		template<typename T>
		T const& PickFrom(std::vector<T> const& values)
		{
			REQUIRE_FALSE(values.empty());
			return values[m_Random.Pick(values.size())];
		}

		static inline std::vector<std::string> const GenericStrings = {
			"", "x", "Fuzz", "  ", "名前", std::string(300, 'a'), std::string("nul\0inside", 10), "18446744073709551615", "../.."};
		static inline std::vector<std::string> const KeyNames = {"A", "Space", "Escape", "Left", "F12", "NotAKey", ""};
		static inline std::vector<std::string> const MouseButtonNames = {"Left", "Right", "Middle", "Button8", ""};
		// input.set's actions and gamepads: names the project lacks or spells otherwise, and slots, buttons and axes that are
		// not.
		static inline std::vector<std::string> const BadActionNames = {"", "Missing", "jump", "Jump ", std::string(70, 'a')};
		static inline std::vector<std::string> const GamepadSlots = {"0", "1", "7", "15"};
		static inline std::vector<std::string> const BadGamepadSlots = {"16", "01", "-1", " 1", "x", ""};
		static inline std::vector<std::string> const GamepadButtonNames = {"A", "B", "Start", "DPadUp", "RightBumper", "Nope", ""};
		static inline std::vector<std::string> const GamepadAxisNames = {"LeftX", "LeftY", "RightTrigger", "LeftTrigger", "Nope"};

		Testing::FuzzRandom m_Random;
		Testing::DocumentMutator m_Mutator;
		EditorState const& m_State;
		std::filesystem::path m_Outside;
		std::vector<std::string> m_ImportFiles;
		// The fuzzed project's directory, which project.copy-example's directories are relative to.
		std::filesystem::path m_Project;
		// The fresh directories project.copy-example was given so far.
		uint64_t m_Copies = 0;
		std::string_view m_Command;
		// The sub-graph the last graph.edit batch drew its elements from.
		int64_t m_BatchFunction = 0;
		// A graph.edit right in every part was drawn.
		bool m_RightEditDrawn = false;
		// The goals reached (SetReached), and the files SureRequest named.
		std::set<std::string> m_Reached;
		uint64_t m_SureFiles = 0;
	};

	// A project with something of everything, open in an editor model with the commands the editor layer registers.
	struct FuzzFixture
	{
		// The recorder, as the editor's application has it: Tick runs its frames.
		Testing::ProfilerScope Recorder;
		// Telemetry's recorder, as the application has it: playing scenes' steps record while a recording runs.
		Testing::TelemetryScope TelemetryRecorder;
		// The tests' data asset type, so its generated commands are fuzzed: registered before the commands, and unregistered
		// after the asset directory closed.
		Testing::TestDataTypeScope DataType;
		Testing::PhysicsSystemScope Physics;
		Testing::AudioEngineScope Audio;
		Testing::AssetManagerScope Assets;
		InputScope Inputs;
		Testing::TemporaryDirectory Directory;
		// Nested, so paths that climb out of the project with "../" still land in the temporary directory.
		std::filesystem::path ProjectDirectory = Directory.GetPath() / "One" / "Two" / "Project";
		// Where absolute scene paths point: scenes may be saved anywhere.
		std::filesystem::path Outside = Directory.GetPath() / "Outside";
		std::filesystem::path Imports = Directory.GetPath() / "Imports";
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		EditorCamera Camera;
		SceneViewOverlays Overlays;
		ContentBrowserPanel ContentBrowser;
		// The commands' folders besides the project: the test content, and the user data in the temporary directory.
		EditorLocations Locations = Testing::MakeEditorLocations(Directory.GetPath());
		// The graph windows, never drawn and without the scripting runtime (degraded): every command but graph.find only records
		// state between frames, which needs no ImGui. Closing a window writes the views file, in the commands' user data.
		std::filesystem::path ViewsDirectory = Locations.UserDataDirectory;
		VisualScriptViewSettings ViewSettings{ViewsDirectory / "VisualScriptViews.json"};
		EngineVisualScriptSemantics Semantics;
		VisualScriptEditors GraphEditors{Operations, Semantics, ViewSettings};
		// The debug session, as play mode's observer, with the views settings: graph.breakpoints writes the views file.
		VisualScriptDebugSession DebugSession{Context, Play};
		// The Debugger panel graph.debug-panel reaches, never drawn.
		VisualScriptDebuggerPanel DebuggerPanel{DebugSession, Context};
		// Copies examples of the test content for project.copy-example; a running copy finishes within the frame it started in
		// (Tick).
		ExampleCopyTask Copies;
		// The start screen's state behind start.info, start.show and editor.recent: the recent projects file lies in the
		// commands' user data, which the fuzz may write. Headless: start.show and editor.dialog are Unavailable.
		StartScreenModel StartScreen{Locations};
		// Who records, for the profiler commands; there is no panel to show captures in.
		EditorProfiler ProfilerSession;
		// Who records telemetry, for its commands.
		EditorTelemetry TelemetryOwner;
		CommandRegistry Registry;
		bool QuitRequested = false;
		// The entities of the running scene that held a character controller, and those that held a dynamic body, when a
		// frame began (Tick), by UUID: the simulation moved them, so their poses are not the history's. The running scene
		// they belong to is the one GetScene returned at that scene version (playing starting or stopping, or the game
		// loading another scene, gives it a new version, and the records start over).
		std::set<std::string> SimulatedCharacters;
		std::set<std::string> SimulatedBodies;
		std::set<std::string> SimulatedWheels;
		std::optional<uint64_t> SimulatedSceneVersion;

		FuzzFixture()
		{
			// Files and folders dropped onto the content browser go to its folders, C# sources to the scripts.
			ContentBrowser.SetScripts(&Scripts);
			EditorCommandEnvironment environment;
			environment.Locations = Locations;
			environment.RequestQuit = [this]
			{
				QuitRequested = true;
			};
			environment.ViewportCamera = [this]() -> EditorCamera&
			{
				return Camera;
			};
			environment.ViewportOverlays = [this]() -> SceneViewOverlays&
			{
				return Overlays;
			};
			environment.ViewportStatistics = []() -> SceneRendererStatistics const*
			{
				return nullptr;
			};
			environment.ContentBrowser = [this]() -> ContentBrowserPanel&
			{
				return ContentBrowser;
			};
			environment.GraphEditors = [this]() -> VisualScriptEditors&
			{
				return GraphEditors;
			};
			environment.DebuggerPanel = [this]() -> VisualScriptDebuggerPanel&
			{
				return DebuggerPanel;
			};
			// There is no inspector or menu bar to show the menus, but inspector.add-component-menu and editor.entity-menu still
			// check and list everything.
			environment.ShowAddComponentMenu = [](bool, std::string const&, std::string const&) {};
			environment.ShowEntityMenu = [](bool, std::string const&) {};
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			REQUIRE(RegisterScriptCommands(Registry, Scripts, Context).IsOk());
			REQUIRE(RegisterGraphDebugCommands(Registry, DebugSession, Play, Context).IsOk());
			REQUIRE(RegisterProjectContentCommands(Registry, Operations, Copies, Locations, {}).IsOk());
			REQUIRE(RegisterStartCommands(Registry, StartScreen, {}).IsOk());
			REQUIRE(RegisterProfilerCommands(Registry, ProfilerSession, Context, {}).IsOk());
			REQUIRE(RegisterTelemetryCommands(Registry, TelemetryOwner, Context).IsOk());
			DebugSession.SetViewSettings(&ViewSettings);
			Play.SetObserver(&DebugSession);

			REQUIRE(FileSystem::CreateDirectories(Outside).IsOk());
			REQUIRE(FileSystem::CreateDirectories(Imports).IsOk());
			REQUIRE(Image(2, 2, 4).WritePNG(Imports / "Texture.png").IsOk());
			std::array<float, 64> const silence{};
			REQUIRE(FileSystem::WriteBinaryFile(Imports / "Sound.wav", Testing::MakeWav(silence).GetSpan()).IsOk());
			REQUIRE(Operations.CreateProject(ProjectDirectory, "Fuzz", Scene("Main")).IsOk());
			REQUIRE(FileSystem::WriteTextFile(Directory.GetPath() / "NotAProject.sproj", "{ not json").IsOk());
			SeedRecentProjects();

			// An entity for each component, in one hierarchy, a material, a visual script, a prefab, a folder and imported files.
			std::string parent;
			std::string visualScript;
			Json const types = Ok("component.types");
			for (Json const& type : types["components"])
			{
				std::string const name = type["Name"].get<std::string>();
				if (type["Core"] == true)
				{
					continue;
				}
				std::string const id =
					Ok("entity.create", Json::object({{"name", name}, {"components", {{name, Json::object()}}}}))["id"].get<std::string>();
				if (name == "VisualScript")
				{
					visualScript = id;
				}
				if (parent.empty())
				{
					parent = id;
				}
				else
				{
					Ok("entity.reparent", Json::object({{"entity", id}, {"parent", parent}}));
				}
			}
			Ok("material.create", Json::object({{"path", "Materials/Painted.smat"}, {"fields", {{"Roughness", 0.25}}}}));
			ReseedDataAssets(EditorState());
			ReseedInputActions();
			// The VisualScript component was created without a graph: it gets one.
			REQUIRE_FALSE(visualScript.empty());
			std::string const graph = Ok("graph.create", Json::object({{"path", SeedGraphPath}}))["asset"].get<std::string>();
			std::string const library =
				Ok("graph.create", Json::object({{"path", SeedLibraryPath}, {"template", "Library"}}))["asset"].get<std::string>();
			Ok("graph.set", Json::object({{"graph", library}, {"document", LoadLibraryContent()}}));
			// The mutator only damages what the seed holds: the feature-test sample of the format has functions, locals, comments,
			// a custom event and every node key of version 2; the nodes added name the seed itself and the library.
			Json const sample = Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph");
			Ok("graph.set", Json::object({{"graph", graph}, {"document", MakeSeedDocument(sample["VisualScript"], graph, library)}}));
			Ok("component.set",
			   Json::object({{"entity", visualScript}, {"component", "VisualScript"}, {"fields", Json::object({{"Graph", graph}})}}));
			Ok("prefab.create", Json::object({{"entity", parent}, {"path", "Prefabs/Everything.sprefab"}}));
			Ok("asset.create-folder", Json::object({{"folder", "Empty"}}));
			Ok("asset.import", Json::object({{"files", Json::array({ImportFiles()[0], ImportFiles()[1]})}, {"folder", "Imported"}}));
			Ok("scene.save");
			// A capture for profiler.open, and the first frame for the recordings profiler.start begins.
			Testing::CaptureBuilder capture;
			capture.AddFrame(0, 16000000);
			capture.AddZone(0, "Fuzz.Seed", 0, 0, 1000000);
			REQUIRE(ProfilerCaptureSerializer::Save(capture.Get(), ProjectDirectory / "Captures" / "Seed.json").IsOk());
			Profiler::BeginFrame();
			// A telemetry session of four steps with a channel and a lap, for telemetry.read and telemetry.export.
			REQUIRE(Telemetry::Start().IsOk());
			for (uint64_t step = 1; step <= 4; ++step)
			{
				Telemetry::Record("Fuzz.Speed", static_cast<float>(step));
				if (step % 2 == 0)
				{
					Telemetry::Mark(step == 2 ? "Lap 1" : "Lap 2");
				}
				Telemetry::EndStep(Testing::MakeTelemetryStep(step));
			}
			Telemetry::Stop();
		}

		~FuzzFixture()
		{
			// The session goes before play mode, which stops playing when it goes.
			Play.Stop();
			Play.SetObserver(nullptr);
		}

		FuzzFixture(FuzzFixture const&) = delete;
		FuzzFixture& operator=(FuzzFixture const&) = delete;

		// The files asset.import and asset.drop are given: two that exist, one that does not and (asset.drop only) the folder
		// holding them.
		std::vector<std::string> ImportFiles() const
		{
			return {FileSystem::PathToUtf8(Imports / "Texture.png"), FileSystem::PathToUtf8(Imports / "Sound.wav"),
			        FileSystem::PathToUtf8(Imports / "Missing.png"), FileSystem::PathToUtf8(Imports)};
		}

		// Runs a synchronous command.
		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		// Lists the fuzzed project, a missing one and a file that is no project as the recent projects (all three states), as
		// at the start and whenever the commands cleared the list.
		void SeedRecentProjects()
		{
			RecentProjects& recent = StartScreen.GetRecentProjects();
			recent.Add(Directory.GetPath() / "NotAProject.sproj");
			recent.Add(Outside / "Missing" / "Missing.sproj");
			recent.Add(Context.GetProject()->GetFilePath());
			REQUIRE(recent.Save().IsOk());
		}

		// Puts entities with components back into a scene the fuzzer emptied: an instance of the prefab of everything, or
		// a few entities when the prefab was deleted or moved.
		void Replenish()
		{
			if (Run("prefab.instantiate", Json::object({{"prefab", "asset://Prefabs/Everything.sprefab"}})).IsOk())
			{
				return;
			}
			for (char const* const component : {"RigidBody", "PointLight", "AudioSource"})
			{
				static_cast<void>(Run("entity.create", Json::object({{"name", component}, {"components", {{component, Json::object()}}}})));
			}
		}

		// Writes the seed script again when the commands deleted, moved or broke it (as Replenish puts entities back), so the
		// graph commands keep a script with every element to work on, and the library it calls when that went too.
		void ReseedGraph()
		{
			std::string const libraryReference = std::string("asset://") + SeedLibraryPath;
			if (Run("graph.get", Json::object({{"graph", libraryReference}, {"analyze", false}})).IsError())
			{
				static_cast<void>(Run("graph.create", Json::object({{"path", SeedLibraryPath}, {"template", "Library"}})));
				static_cast<void>(Run("graph.set", Json::object({{"graph", libraryReference}, {"document", LoadLibraryContent()}})));
			}
			Json const sample = Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph");
			std::string const reference = std::string("asset://") + SeedGraphPath;
			if (Run("graph.get", Json::object({{"graph", reference}, {"analyze", false}})).IsError())
			{
				static_cast<void>(Run("graph.create", Json::object({{"path", SeedGraphPath}})));
			}
			AssetHandle const seed = AssetManager::FindByPath(SeedGraphPath);
			AssetHandle const library = AssetManager::FindByPath(SeedLibraryPath);
			Json const document = seed.IsValid() && library.IsValid()
			                          ? MakeSeedDocument(sample["VisualScript"], seed.ToString(), library.ToString())
			                          : sample["VisualScript"];
			static_cast<void>(Run("graph.set", Json::object({{"graph", reference}, {"document", document}})));
		}

		// Writes an asset of every data asset type the state has none of (as Replenish puts entities back), so that each type's
		// get and set have one to work on.
		void ReseedDataAssets(EditorState const& state)
		{
			for (DataAssetType const* type : DataAssetRegistry::GetTypes())
			{
				if (!state.AssetsByType.contains(type->GetName()))
				{
					static_cast<void>(Run(type->GetCommandDomain() + ".create", Json::object({{"path", "Data/Seed" + type->GetExtension()},
					                                                                          {"fields", type->GetDefaultFields()}})));
				}
			}
		}

		// Names input actions of each type in the project again when it names none that load (as Replenish puts entities
		// back): Input/Fuzz.sinput written, or its fields set again, and named by the project's settings, so that input.set's
		// actions reach their work.
		void ReseedInputActions()
		{
			Json const fields = Json::parse(R"({ "Actions": [
				{ "Name": "Jump", "Bindings": [ { "Controls": "Key/Space" }, { "Controls": "Gamepad/A" } ] },
				{ "Name": "Steer", "Type": "Axis", "Bindings": [ { "Controls": "Key/A Key/D" } ] },
				{ "Name": "Move", "Type": "Vector2", "Bindings": [ { "Controls": "Gamepad/LeftStick" } ] } ] })");
			if (Run("input-actions.create", Json::object({{"path", "Input/Fuzz.sinput"}, {"fields", fields}})).IsError())
			{
				static_cast<void>(
					Run("input-actions.set", Json::object({{"inputActions", "asset://Input/Fuzz.sinput"}, {"fields", fields}})));
			}
			static_cast<void>(
				Run("project.settings", Json::object({{"settings", Json::object({{"InputActions", "asset://Input/Fuzz.sinput"}})}})));
		}

		// Opens a visual script's window again when none is open (as Replenish puts entities back), so the palette has a window
		// to open in.
		void ReopenGraphWindow(EditorState const& state)
		{
			auto const graphs = state.AssetsByType.find("VisualScript");
			if (graphs != state.AssetsByType.end() && !graphs->second.empty())
			{
				static_cast<void>(Run("graph.open", Json::object({{"graph", graphs->second.front()}})));
			}
		}

		// One editor frame, from the command that ran to the next one: scripts, play mode's update and the debug session's end of
		// the frame, then the start of the next frame's (its queued resumes).
		void Tick()
		{
			// A copy is a few small files: it ends in this frame, so the history checks never meet one writing.
			while (Copies.IsRunning())
			{
				Copies.Update();
				if (Copies.IsRunning())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			Scripts.Update();
			RecordSimulatedEntities();
			Play.Update(Timestep(1.0f / 60.0f));
			DebugSession.EndFrame();
			DebugSession.BeginFrame();
			// The frame ends and the next begins, as the editor's main loop has them: a recording collects the frame.
			{
				ST_PROFILE_SCOPE("Fuzz.Tick");
			}
			Profiler::EndFrame();
			Profiler::BeginFrame();
		}

		// Records the running scene's entities that the frame about to run simulates or places (SimulatedCharacters,
		// SimulatedBodies, SimulatedWheels);
		// forgets them when no game plays, or when GetScene returns another scene than the one they were recorded in.
		void RecordSimulatedEntities()
		{
			std::optional<uint64_t> const version = Play.IsPlaying() ? std::optional<uint64_t>(Context.GetSceneVersion()) : std::nullopt;
			if (version != SimulatedSceneVersion)
			{
				SimulatedCharacters.clear();
				SimulatedBodies.clear();
				SimulatedWheels.clear();
				SimulatedSceneVersion = version;
			}
			if (!version)
			{
				return;
			}
			Scene& scene = Context.GetScene();
			for (entt::entity const handle : scene.GetAllEntitiesWith<IDComponent, CharacterControllerComponent>())
			{
				SimulatedCharacters.insert(scene.GetRegistry().get<IDComponent>(handle).ID.ToString());
			}
			for (entt::entity const handle : scene.GetAllEntitiesWith<IDComponent, RigidBodyComponent>())
			{
				if (scene.GetRegistry().get<RigidBodyComponent>(handle).Type == RigidBodyType::Dynamic)
				{
					SimulatedBodies.insert(scene.GetRegistry().get<IDComponent>(handle).ID.ToString());
				}
			}
			for (entt::entity const handle : scene.GetAllEntitiesWith<IDComponent, VehicleWheelComponent>())
			{
				SimulatedWheels.insert(scene.GetRegistry().get<IDComponent>(handle).ID.ToString());
			}
		}

		// The edited scene (or the running copy) saves and loads again.
		void CheckSceneLoads()
		{
			Json const dump = Ok("scene.dump");
			Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(dump, AssetManager::CreateDeserializationContext());
			CHECK_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		}

		// While playing, the simulation moves characters and turns and moves dynamic bodies between commands: where they are
		// is not part of the history (redoing an entity's creation places it where it was created). That holds for an entity
		// that held a controller or a dynamic body earlier in this running scene too: it keeps the pose the simulation gave it
		// when the component leaves it (an undo, a removal, a body made static), and no step of the history ever held that
		// pose. Vehicles place their wheels' entities alike (VehicleWheel), turning and moving them. It changes nothing else
		// of their transforms, which the history still covers: the scale of all, the rotation of characters.
		Json WithoutSimulatedPoses(Json scene) const
		{
			if (!Play.IsPlaying())
			{
				return scene;
			}
			bool const recorded = SimulatedSceneVersion == Context.GetSceneVersion();
			for (Json& entity : scene["Entities"])
			{
				Json& components = entity["Components"];
				if (!components.contains("Transform"))
				{
					continue;
				}
				std::string const id = entity.value("ID", std::string());
				Json& transform = components["Transform"];
				if (components.contains("CharacterController") || (recorded && SimulatedCharacters.contains(id)))
				{
					transform.erase("Translation");
				}
				else if ((components.contains("RigidBody") && components["RigidBody"].value("Type", "") == "Dynamic") ||
				         components.contains("VehicleWheel") ||
				         (recorded && (SimulatedBodies.contains(id) || SimulatedWheels.contains(id))))
				{
					transform.erase("Translation");
					transform.erase("Rotation");
				}
			}
			return scene;
		}

		// Runs editor.undo or editor.redo until it fails: how many steps it took, and the message of the failure that ended it.
		std::pair<int, std::string> RunAll(std::string_view command)
		{
			int steps = 0;
			while (true)
			{
				CommandResult result = Run(command);
				if (result.IsError())
				{
					return {steps, result.GetError().Message};
				}
				steps++;
			}
		}

		// Undoing every step, then redoing them all, brings the scene back (once what was undone before is redone), and every
		// file of the asset directory byte for byte: undo and redo rewrite graph and material files, the steps carried across
		// Stop and scene changes included. Every step undoes and redoes: each loop ends because nothing is left, never at a
		// step that fails, where both loops would stop alike and hide it.
		bool CheckHistory()
		{
			std::pair<int, std::string> const before = RunAll("editor.redo");
			CHECK(before.second == "there is nothing to redo");
			Json const edited = WithoutSimulatedPoses(Ok("scene.dump"));
			FileContents const files = ReadFiles(AssetManager::GetAssetDirectory());
			std::pair<int, std::string> const undone = RunAll("editor.undo");
			CHECK(undone.second == "there is nothing to undo");
			CheckSceneLoads();
			std::pair<int, std::string> const redone = RunAll("editor.redo");
			CHECK(redone.second == "there is nothing to redo");
			Json const restored = WithoutSimulatedPoses(Ok("scene.dump"));
			std::string const changed = ListChangedFiles(files, ReadFiles(AssetManager::GetAssetDirectory()));
			CHECK(redone.first == undone.first);
			CHECK(restored == edited);
			CHECK_MESSAGE(changed.empty(), "files undo and redo did not give back:" << changed);
			return before.second == "there is nothing to redo" && undone.second == "there is nothing to undo" &&
			       redone.second == "there is nothing to redo" && redone.first == undone.first && restored == edited && changed.empty();
		}

		// What the graph commands name in the document graph.get gave.
		static void ReadGraph(EditorState& state)
		{
			Json const& document = state.GraphDocument;
			std::set<std::string> pins;
			std::set<std::string> events;
			auto const readGraph = [&](Json const& graph, int64_t function)
			{
				std::vector<int64_t>& elements = state.GraphElements[function];
				Json const nodes = graph.value("Nodes", Json::array());
				for (Json const& node : nodes)
				{
					elements.push_back(node.value("ID", int64_t(0)));
					state.GraphNodes.push_back(elements.back());
					Json const inputs = node.value("Inputs", Json::object());
					for (auto const& [pin, value] : inputs.items())
					{
						pins.insert(pin);
					}
					Json const settings = node.value("Settings", Json::object());
					if (node.value("Type", std::string()) == "Event.Custom" && settings.contains("Name") && settings["Name"].is_string())
					{
						events.insert(settings["Name"].get<std::string>());
					}
				}
				Json const comments = graph.value("Comments", Json::array());
				for (Json const& comment : comments)
				{
					elements.push_back(comment.value("ID", int64_t(0)));
					state.GraphComments[function].push_back(elements.back());
				}
				Json const links = graph.value("Links", Json::array());
				for (Json const& link : links)
				{
					if (link.contains("From") && link.contains("To"))
					{
						state.GraphLinks[function].push_back(Json::object({{"from", link["From"]}, {"to", link["To"]}}));
					}
					for (char const* const end : {"From", "To"})
					{
						if (link.contains(end) && link[end].is_array() && link[end].size() == 2 && link[end][1].is_string())
						{
							pins.insert(link[end][1].get<std::string>());
						}
					}
				}
			};
			readGraph(document.value("Graph", Json::object()), 0);
			Json const variables = document.value("Variables", Json::array());
			for (Json const& variable : variables)
			{
				state.GraphReferences.push_back(variable.value("ID", int64_t(0)));
				state.GraphVariables.push_back(state.GraphReferences.back());
				if (variable.contains("Name") && variable["Name"].is_string())
				{
					state.GraphVariableNames.push_back(variable["Name"].get<std::string>());
				}
			}
			Json const functions = document.value("Functions", Json::array());
			for (Json const& function : functions)
			{
				int64_t const id = function.value("ID", int64_t(0));
				state.GraphFunctions.push_back(id);
				state.GraphReferences.push_back(id);
				if (function.contains("Name") && function["Name"].is_string())
				{
					state.GraphFunctionNames.push_back(function["Name"].get<std::string>());
				}
				Json const locals = function.value("Locals", Json::array());
				for (Json const& local : locals)
				{
					state.GraphReferences.push_back(local.value("ID", int64_t(0)));
				}
				readGraph(function.value("Graph", Json::object()), id);
			}
			state.GraphPins.assign(pins.begin(), pins.end());
			state.GraphEvents.assign(events.begin(), events.end());
		}

		EditorState ReadState()
		{
			EditorState state;
			std::vector<Json> nodes = Ok("scene.hierarchy")["entities"].get<std::vector<Json>>();
			while (!nodes.empty())
			{
				Json const node = std::move(nodes.back());
				nodes.pop_back();
				std::string const id = node["id"].get<std::string>();
				state.Entities.push_back(id);
				state.EntityNames.push_back(node["name"].get<std::string>());
				state.EntityComponents[id] = node["components"].get<std::vector<std::string>>();
				for (Json const& child : node["children"])
				{
					nodes.push_back(child);
				}
			}
			Json const types = Ok("component.types");
			for (Json const& type : types["components"])
			{
				Json fields = Json::object();
				for (Json const& field : type["Fields"])
				{
					fields[field["Name"].get<std::string>()] = field["Default"];
				}
				state.Components.push_back(type["Name"].get<std::string>());
				state.ComponentFields[type["Name"].get<std::string>()] = std::move(fields);
			}
			// Script fields and graph variables of one value and lists, which component.set, component.add and entity.create
			// are sent damaged now and then: their maps' defaults are empty.
			Json const scriptValues = Json::object(
				{{"Speed", Json::object({{"Type", "Float"}, {"Value", 2.5}})},
			     {"Speeds", Json::object({{"Type", "Float[]"}, {"Value", Json::array({1.5, 2.5})}})},
			     {"Targets", Json::object({{"Type", "Entity[]"}, {"Value", Json::array({"0"})}})},
			     {"Titles", Json::object({{"Type", "String[]"}, {"Value", Json::array()}})},
			     {"Turns", Json::object({{"Type", "Quaternion[]"}, {"Value", Json::array({Json::array({0.0, 0.0, 0.0, 1.0})})}})}});
			state.ComponentFields["Script"]["Fields"] = scriptValues;
			state.ComponentFields["VisualScript"]["Variables"] = scriptValues;
			std::set<std::string> folders;
			bool seedRead = false;
			Json const assets = Ok("asset.list", Json::object({{"builtIn", true}}));
			for (Json const& asset : assets["assets"])
			{
				std::vector<std::string> names = {asset["id"].is_string() ? asset["id"].get<std::string>() : asset["id"].dump()};
				if (asset["reference"].is_string())
				{
					names.push_back(asset["reference"].get<std::string>());
				}
				std::string const type = asset["type"].get<std::string>();
				for (std::string const& name : names)
				{
					state.Assets.push_back(name);
					state.AssetsByType[type].push_back(name);
				}
				if (!asset["path"].is_string())
				{
					continue;
				}
				std::string const path = asset["path"].get<std::string>();
				for (std::string const& name : names)
				{
					state.AssetPaths[name] = path;
				}
				if (type == "Scene")
				{
					state.ScenePaths.push_back(path);
				}
				if (type == "Material" && state.MaterialFields.empty())
				{
					CommandResult material = Run("material.get", Json::object({{"material", names.front()}}));
					if (material.IsOk())
					{
						state.MaterialFields = material.GetValue()["fields"];
					}
				}
				if (DataAssetType const* dataType = DataAssetRegistry::FindByName(type);
				    dataType != nullptr && !state.DataAssetFields.contains(type))
				{
					CommandResult fields =
						Run(dataType->GetCommandDomain() + ".get", Json::object({{dataType->GetCommandParameter(), names.front()}}));
					if (fields.IsOk())
					{
						state.DataAssetFields[type] = fields.GetValue()["fields"];
					}
				}
				// The seed script while it is there (the scripts the commands create hold little), else the first that loads.
				if (type == "VisualScript" && (state.GraphDocument.empty() || (path == SeedGraphPath && !seedRead)))
				{
					CommandResult graph = Run("graph.get", Json::object({{"graph", names.front()}, {"analyze", false}}));
					if (graph.IsOk())
					{
						state.GraphDocument = graph.GetValue()["document"];
						state.GraphRevision = graph.GetValue()["revision"].get<std::string>();
						state.GraphProgramRevision = graph.GetValue()["programRevision"].get<std::string>();
						state.Graph = names.front();
						seedRead = path == SeedGraphPath;
					}
				}
				for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
				{
					folders.insert(path.substr(0, slash));
				}
			}
			state.Folders.assign(folders.begin(), folders.end());
			if (!state.GraphDocument.empty())
			{
				ReadGraph(state);
				// What graph.edit's Paste takes: the clipboard text of up to three of the event graph's elements.
				std::vector<int64_t> const& elements = state.GraphElements[0];
				if (!elements.empty())
				{
					Json copied = Json::array();
					for (size_t index = 0; index < std::min<size_t>(3, elements.size()); index++)
					{
						copied.push_back(elements[index]);
					}
					CommandResult copy = Run("graph.copy", Json::object({{"graph", state.Graph}, {"elements", std::move(copied)}}));
					if (copy.IsOk())
					{
						state.GraphClipboard = copy.GetValue()["text"].get<std::string>();
					}
				}
			}
			state.SeedGraph = seedRead;
			Json const editors = Ok("graph.editors")["editors"];
			for (Json const& editor : editors)
			{
				state.OpenGraphs.push_back(editor["asset"].get<std::string>());
			}
			state.ProjectSettings = Ok("project.info")["settings"];
			state.SceneSettings = Ok("scene.settings")["settings"];
			// The project's actions as input.set checks names against them (the fuzz may have renamed or removed them).
			if (Json const named = state.ProjectSettings.value("InputActions", Json());
			    named.is_string() && !named.get<std::string>().empty())
			{
				CommandResult actions = Run("input-actions.get", Json::object({{"inputActions", named}}));
				if (actions.IsOk() && actions.GetValue()["fields"].value("Actions", Json()).is_array())
				{
					for (Json const& action : actions.GetValue()["fields"]["Actions"])
					{
						if (action.value("Name", Json()).is_string())
						{
							state.ActionNames.push_back(action["Name"].get<std::string>());
						}
					}
				}
			}
			return state;
		}
	};
}

TEST_CASE("Automation: commands with any parameters answer once, keep the editor consistent and write only in the project")
{
	FuzzFixture fixture;
	// Closing graph windows writes the per-user views file.
	std::vector<std::filesystem::path> const writable = {fixture.ProjectDirectory, fixture.Outside, fixture.ViewsDirectory};
	FileSnapshot const filesBefore = SnapshotFiles(fixture.Directory.GetPath(), writable);
	// The engine content the commands read (the examples project.copy-example copies) is never written.
	FileSnapshot const contentBefore = SnapshotFiles(Testing::GetTestContentDirectory(), {});

	std::vector<CommandDefinition const*> commands;
	for (CommandDefinition const* command : fixture.Registry.GetCommands())
	{
		if (std::ranges::find(ExcludedCommands, command->Name) == ExcludedCommands.end())
		{
			commands.push_back(command);
		}
	}

	EditorState state = fixture.ReadState();
	ParameterGenerator generator(7, state, fixture.Outside, fixture.ImportFiles(), fixture.ProjectDirectory);
	Testing::FuzzRandom random(11);
	std::set<int> unanswered;
	// Command name -> how often it succeeded and failed; graph.find's by query and by references, and graph.palette's errors.
	std::map<std::string, std::pair<int, int>> outcomes;
	std::map<std::string, int> findSuccesses;
	// input.set's successes that set an action override, and that injected a gamepad.
	std::map<std::string, int> inputSuccesses;
	std::map<AutomationErrorCode, int> paletteErrors;
	// The debug commands' errors: without the scripting runtime they reach their parameter checks, and are otherwise refused
	// for the state (Unavailable, InvalidOperation).
	std::map<std::string, std::set<AutomationErrorCode>> debugErrors;
	// The requests since the last history check, listed when it fails, to find the step that breaks the history.
	std::vector<std::string> recent;
	for (int round = 0, rounds = Testing::GetFuzzRounds(1500); round < rounds; round++)
	{
		if (round % 5 == 0)
		{
			state = fixture.ReadState();
			if (fixture.StartScreen.GetRecentProjects().GetProjects().empty())
			{
				fixture.SeedRecentProjects();
			}
			if (state.Entities.size() < 3)
			{
				fixture.Replenish();
				state = fixture.ReadState();
			}
			if (!state.SeedGraph)
			{
				fixture.ReseedGraph();
				state = fixture.ReadState();
			}
			if (state.OpenGraphs.empty())
			{
				fixture.ReopenGraphWindow(state);
				state = fixture.ReadState();
			}
			if (state.ActionNames.empty())
			{
				fixture.ReseedInputActions();
				state = fixture.ReadState();
			}
			if (std::ranges::any_of(DataAssetRegistry::GetTypes(),
			                        [&state](DataAssetType const* dataType)
			                        {
										return !state.AssetsByType.contains(dataType->GetName());
									}))
			{
				fixture.ReseedDataAssets(state);
				state = fixture.ReadState();
			}
		}
		CommandDefinition const& command = *commands[random.Pick(commands.size())];
		Json const params = generator.Generate(command.Name, command.Parameters);
		std::string const request = command.Name + " " + params.dump();
		std::string const findKind = params.is_object() && params.contains("references") ? "references" : "query";
		// input.set's device kinds the request names (an override, an injected gamepad), for its goals.
		std::vector<std::string> inputKinds;
		for (char const* const kind : {"actions", "gamepads"})
		{
			if (command.Name == "input.set" && params.is_object() && params.contains(kind) && params[kind].is_object() &&
			    !params[kind].empty())
			{
				inputKinds.emplace_back(kind);
			}
		}
		CAPTURE(round);
		CAPTURE(request);
		// project.copy-example refuses before it copies (a bad directory, an unknown or unavailable example), and then writes
		// nothing anywhere, also where copies may go.
		bool const copiesExample = command.Name == "project.copy-example";
		std::optional<FileSnapshot> const beforeCopy =
			copiesExample ? std::optional<FileSnapshot>(SnapshotFiles(fixture.Directory.GetPath(), {})) : std::nullopt;
		std::optional<int> refusedRound;
		unanswered.insert(round);
		fixture.Registry.Execute(command.Name, params,
		                         [&unanswered, &outcomes, &findSuccesses, &inputSuccesses, &generator, &paletteErrors, &debugErrors,
		                          &recent, &refusedRound, name = command.Name, round, request, findKind, inputKinds](CommandResult result)
		                         {
									 CHECK_MESSAGE(unanswered.erase(round) == 1, "answered twice: " << request);
									 recent.push_back(request + (result.IsOk() ? " -> ok" : " -> " + result.GetError().Message));
									 if (result.IsError())
									 {
										 refusedRound = round;
									 }
									 if (result.IsOk())
									 {
										 outcomes[name].first++;
										 generator.SetReached(name);
										 if (name == "graph.find")
										 {
											 findSuccesses[findKind]++;
											 generator.SetReached(name + " " + findKind);
										 }
										 for (std::string const& kind : inputKinds)
										 {
											 inputSuccesses[kind]++;
											 generator.SetReached(name + " " + kind);
										 }
										 // Graph edits say what the running game took: without the scripting runtime, nothing.
										 if (name == "graph.edit" || name == "graph.set")
										 {
											 Json const& live = result.GetValue()["live"];
											 CHECK_MESSAGE(live["edits"] == Json::array(), request);
											 CHECK(live["playState"].is_string());
										 }
										 return;
									 }
									 outcomes[name].second++;
									 if (name == "graph.palette")
									 {
										 paletteErrors[result.GetError().Code]++;
										 if (result.GetError().Code == AutomationErrorCode::InvalidParams)
										 {
											 generator.SetReached("graph.palette InvalidParams");
										 }
										 if (result.GetError().Code == AutomationErrorCode::Unavailable)
										 {
											 generator.SetReached("graph.palette Unavailable");
										 }
									 }
									 debugErrors[name].insert(result.GetError().Code);
									 // InternalError means a handler threw: it relied on something it did not check.
									 CHECK_MESSAGE(result.GetError().Code != AutomationErrorCode::InternalError,
			                                       request << ": " << result.GetError().Message);
									 CHECK_FALSE(result.GetError().Message.empty());
								 });
		if (!command.IsAsync())
		{
			CHECK_MESSAGE(!unanswered.contains(round), "a synchronous command did not answer");
		}
		if (beforeCopy && refusedRound == round)
		{
			CHECK_MESSAGE(SnapshotFiles(fixture.Directory.GetPath(), {}) == *beforeCopy, "a refused copy wrote files: " << request);
			CHECK(SnapshotFiles(Testing::GetTestContentDirectory(), {}) == contentBefore);
		}
		fixture.Tick();
		if (round % 50 == 49)
		{
			fixture.CheckSceneLoads();
		}
		if (round % 100 == 99)
		{
			if (!fixture.CheckHistory())
			{
				for (std::string const& line : recent)
				{
					MESSAGE(line);
				}
			}
			recent.clear();
		}
	}

	// Every command answers once playing stops.
	if (fixture.Play.IsPlaying())
	{
		fixture.Ok("play.stop");
	}
	for (int tick = 0; tick < 100 && !unanswered.empty(); tick++)
	{
		fixture.Tick();
	}
	CHECK(unanswered.empty());
	fixture.CheckSceneLoads();
	fixture.CheckHistory();

	// The parameters reach the commands' work, not only their validation: most commands succeed at least once (a few
	// cannot without a window, a GPU or script classes).
	size_t const exercised = static_cast<size_t>(std::ranges::count_if(outcomes,
	                                                                   [](auto const& outcome)
	                                                                   {
																		   return outcome.second.first > 0;
																	   }));
	CHECK_MESSAGE(exercised * 4 >= commands.size() * 3, exercised << " of " << commands.size() << " commands succeeded");
	// The visual script commands reach their work too (graph.library and graph.types need the scripting runtime, which the
	// fixture does not start).
	for (std::string const graphCommand : {"graph.create", "graph.get", "graph.set", "graph.check", "graph.edit", "graph.arrange",
	                                       "graph.copy", "graph.interface", "graph.open", "graph.close", "graph.editors"})
	{
		CAPTURE(graphCommand);
		CHECK(outcomes[graphCommand].first > 0);
	}
	// Every data asset type's generated commands reach their work.
	REQUIRE_FALSE(DataAssetRegistry::GetTypes().empty());
	for (DataAssetType const* type : DataAssetRegistry::GetTypes())
	{
		for (char const* const action : {".create", ".get", ".set"})
		{
			std::string const dataCommand = type->GetCommandDomain() + action;
			CAPTURE(dataCommand);
			CHECK(outcomes[dataCommand].first > 0);
		}
	}
	// The project content commands reach their work: an example is copied.
	for (std::string const contentCommand : {"project.templates", "project.examples", "project.copy-example"})
	{
		CAPTURE(contentCommand);
		CHECK(outcomes[contentCommand].first > 0);
	}
	CHECK(outcomes["project.copy-example"].second > 0);
	// The profiler commands record, write captures, open them and stop (Dist builds record nothing).
	for (std::string const profilerCommand : {"profiler.start", "profiler.stop", "profiler.status", "profiler.capture", "profiler.open"})
	{
		CAPTURE(profilerCommand);
		bool const records = profilerCommand == "profiler.start" || profilerCommand == "profiler.capture";
		CHECK((outcomes[profilerCommand].first > 0 || (records && !Profiler::GetStatus().Available)));
	}
	// The telemetry commands record, list and stop; read and export reach their work by their sure requests (the seeded
	// session), counted with every command above.
	for (std::string const telemetryCommand : {"telemetry.start", "telemetry.stop", "telemetry.channels"})
	{
		CAPTURE(telemetryCommand);
		CHECK(outcomes[telemetryCommand].first > 0);
	}
	CHECK(findSuccesses["query"] > 0);
	CHECK(findSuccesses["references"] > 0);
	// input.set reaches its work for the project's input actions and for injected gamepads.
	CHECK(inputSuccesses["actions"] > 0);
	CHECK(inputSuccesses["gamepads"] > 0);
	// The debug commands that need no game work; the others are refused, for their parameters or for the state.
	for (std::string const debugCommand : {"graph.breakpoints", "graph.target", "graph.debug", "graph.debug-panel", "graph.live"})
	{
		CAPTURE(debugCommand);
		CHECK(outcomes[debugCommand].first > 0);
	}
	std::set<AutomationErrorCode> const debugRefusals = {AutomationErrorCode::InvalidParams,  AutomationErrorCode::AssetNotFound,
	                                                     AutomationErrorCode::EntityNotFound, AutomationErrorCode::FileError,
	                                                     AutomationErrorCode::Unavailable,    AutomationErrorCode::InvalidOperation};
	for (std::string const debugCommand :
	     {"graph.breakpoints", "graph.target", "graph.debug", "graph.debug-panel", "graph.continue", "graph.step", "graph.break",
	      "graph.wait", "graph.trace", "graph.values", "graph.instances", "graph.live"})
	{
		CAPTURE(debugCommand);
		CHECK(outcomes[debugCommand].first + outcomes[debugCommand].second > 0);
		for (AutomationErrorCode const code : debugErrors[debugCommand])
		{
			CAPTURE(GetAutomationErrorCodeName(code));
			CHECK(debugRefusals.contains(code));
		}
	}
	// The audio meters answer whatever plays, and are refused only for parameters they do not take.
	CHECK(outcomes["audio.meters"].first > 0);
	for (AutomationErrorCode const code : debugErrors["audio.meters"])
	{
		CAPTURE(GetAutomationErrorCodeName(code));
		CHECK(code == AutomationErrorCode::InvalidParams);
	}
	// physics.query answers, or is refused for its parameters or because nothing plays (its own case below reaches every kind).
	for (AutomationErrorCode const code : debugErrors["physics.query"])
	{
		CAPTURE(GetAutomationErrorCodeName(code));
		CHECK((code == AutomationErrorCode::InvalidParams || code == AutomationErrorCode::Unavailable));
	}
	// vehicle.state answers, or is refused for its parameters, for the entity it names, or because nothing plays (its own
	// case below reads a riding bike).
	for (AutomationErrorCode const code : debugErrors["vehicle.state"])
	{
		CAPTURE(GetAutomationErrorCodeName(code));
		CHECK((code == AutomationErrorCode::InvalidParams || code == AutomationErrorCode::InvalidOperation ||
		       code == AutomationErrorCode::EntityNotFound || code == AutomationErrorCode::ComponentNotFound ||
		       code == AutomationErrorCode::Unavailable));
	}
	// The agent kit's commands set the project up when confirmed, refuse without the user's consent and answer dry runs.
	CHECK(outcomes["project.agents"].first > 0);
	CHECK(outcomes["project.setup-agents"].first > 0);
	CHECK(debugErrors["project.setup-agents"].contains(AutomationErrorCode::ConsentRequired));
	// The start commands: what the start screen shows is read and the recent projects change; a headless editor shows no
	// start screen.
	CHECK(outcomes["start.info"].first > 0);
	CHECK(outcomes["editor.recent"].first > 0);
	CHECK(outcomes["start.show"].first == 0);
	CHECK(debugErrors["start.show"].contains(AutomationErrorCode::Unavailable));
	// Nor any dialog: editor.dialog is Unavailable headless.
	CHECK(outcomes["editor.dialog"].first == 0);
	CHECK(debugErrors["editor.dialog"].contains(AutomationErrorCode::Unavailable));
	CHECK(fixture.Ok("editor.recent")["projects"].is_array());
	// The palette needs the runtime, so it opens nowhere: its parameters are still checked before that.
	CHECK(paletteErrors[AutomationErrorCode::InvalidParams] > 0);
	CHECK(paletteErrors[AutomationErrorCode::Unavailable] > 0);

	// The views file the windows wrote loads without a warning.
	fixture.Ok("graph.close");
	VisualScriptViewSettings views(fixture.ViewsDirectory / "VisualScriptViews.json");
	CHECK(FileSystem::Exists(fixture.ViewsDirectory / "VisualScriptViews.json"));
	CHECK(views.Load().empty());

	// Nothing outside the project changed, but where absolute scene paths pointed, and nothing of the engine content.
	CHECK(SnapshotFiles(fixture.Directory.GetPath(), writable) == filesBefore);
	CHECK(SnapshotFiles(Testing::GetTestContentDirectory(), {}) == contentBefore);
}

TEST_CASE("Automation: physics.query answers any parameters while the scene plays, and every kind reaches its query")
{
	FuzzFixture fixture;
	// A floor, a ball and a trigger capsule below the origin, where the fuzzed rays mostly start and go down, beside the
	// prefab of everything the project already holds.
	fixture.Ok("entity.create", Json::object({{"name", "Floor"},
	                                          {"components",
	                                           {{"Transform", Json::object({{"Translation", {0.0, -2.0, 0.0}}})},
	                                            {"BoxCollider", Json::object({{"HalfExtents", {50.0, 0.5, 50.0}}})}}}}));
	fixture.Ok("entity.create", Json::object({{"name", "Ball"},
	                                          {"components",
	                                           {{"Transform", Json::object({{"Translation", {0.0, -1.0, 0.0}}})},
	                                            {"RigidBody", Json::object({{"Type", "Static"}})},
	                                            {"SphereCollider", Json::object()}}}}));
	fixture.Ok("entity.create", Json::object({{"name", "Pillar"},
	                                          {"components",
	                                           {{"Transform", Json::object({{"Translation", {0.5, 0.0, 0.5}}})},
	                                            {"CapsuleCollider", Json::object({{"IsTrigger", true}})}}}}));
	EditorState const state = fixture.ReadState();
	ParameterGenerator generator(13, state, fixture.Outside, {}, fixture.ProjectDirectory);
	CommandDefinition const* const command = fixture.Registry.Find("physics.query");
	REQUIRE(command != nullptr);

	REQUIRE(fixture.Play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	fixture.Play.SetPaused(true);
	std::map<std::string, int> answered;
	std::map<AutomationErrorCode, int> refused;
	// The playing world's state after the first query since the last frame (which applies what changed before it): the
	// queries after it, until the next frame, must leave it as it is.
	std::optional<uint64_t> queriedState;
	int stateChecks = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(600); round < rounds; round++)
	{
		// Now and then a frame runs, so queries see a world that stepped.
		if (round % 50 == 49)
		{
			fixture.Play.Advance(1, Timestep(1.0f / 60.0f));
			queriedState.reset();
		}
		Json const params = generator.Generate(command->Name, command->Parameters);
		std::string const request = params.dump();
		CAPTURE(round);
		CAPTURE(request);
		CommandResult const result = fixture.Run(command->Name, params);
		PhysicsScene const* const physics = fixture.Context.GetScene().GetPhysicsScene();
		REQUIRE(physics != nullptr);
		if (!queriedState && result.IsOk())
		{
			queriedState = physics->ComputeStateHash();
		}
		else if (queriedState)
		{
			CHECK(physics->ComputeStateHash() == *queriedState);
			stateChecks++;
		}
		if (result.IsOk())
		{
			Json const& value = result.GetValue();
			answered[value["kind"].get<std::string>()]++;
			CHECK(value["hits"].size() <= value["count"].get<size_t>());
			CHECK(value["truncated"] == (value["hits"].size() < value["count"].get<size_t>()));
			for (Json const& hit : value["hits"])
			{
				CHECK(hit["entity"].is_string());
				if (hit.contains("distance"))
				{
					CHECK(std::isfinite(hit["distance"].get<double>()));
					CHECK(hit["distance"].get<double>() >= 0.0);
				}
			}
			continue;
		}
		refused[result.GetError().Code]++;
		CHECK_MESSAGE(result.GetError().Code == AutomationErrorCode::InvalidParams, result.GetError().Message);
	}
	// Every kind reached its query, and bad parameters were refused.
	for (char const* const kind : {"raycast", "raycastAll", "shapeCast", "overlap"})
	{
		CAPTURE(kind);
		CHECK(answered[kind] > 0);
	}
	CHECK(refused[AutomationErrorCode::InvalidParams] > 0);
	CHECK(stateChecks > 0);
	// The scene the editor edits comes back as it was.
	fixture.Play.Stop();
	CommandResult const stopped =
		fixture.Run(command->Name, Json::object({{"kind", "raycast"}, {"origin", {0.0, 0.0, 0.0}}, {"direction", {0.0, -1.0, 0.0}}}));
	REQUIRE(stopped.IsError());
	CHECK(stopped.GetError().Code == AutomationErrorCode::Unavailable);
	fixture.CheckSceneLoads();
	fixture.CheckHistory();
}

TEST_CASE("Automation: vehicle.state answers any parameters while a bike rides, and changes nothing")
{
	FuzzFixture fixture;
	// A floor and a bike on it, of a definition with its type's defaults, beside the prefab of everything the project holds.
	Json const created = fixture.Ok("vehicle.create", Json::object({{"path", "Vehicles/Fuzz Bike.svehicle"}}));
	std::string const definition = created["vehicle"]["id"].get<std::string>();
	fixture.Ok("entity.create", Json::object({{"name", "Floor"},
	                                          {"components",
	                                           {{"Transform", Json::object({{"Translation", {0.0, -0.5, 0.0}}})},
	                                            {"BoxCollider", Json::object({{"HalfExtents", {50.0, 0.5, 50.0}}})}}}}));
	Json const bike = fixture.Ok(
		"entity.create", Json::object({{"name", "Bike"},
	                                   {"components",
	                                    {{"Transform", Json::object({{"Translation", {0.0, 0.05, 0.0}}})},
	                                     {"RigidBody", Json::object({{"Type", "Dynamic"}, {"Mass", 250.0}})},
	                                     {"BoxCollider", Json::object({{"HalfExtents", {0.25, 0.5, 0.6}}, {"Offset", {0.0, 0.75, 0.0}}})},
	                                     {"Vehicle", Json::object({{"Definition", definition}})}}}}));
	std::string const bikeId = bike["id"].get<std::string>();
	std::optional<UUID> const bikeUuid = UUID::FromString(bikeId);
	REQUIRE(bikeUuid);
	EditorState const state = fixture.ReadState();
	ParameterGenerator generator(17, state, fixture.Outside, {}, fixture.ProjectDirectory);
	// The stream's requests, not the one the main case sends until vehicle.state has answered once (SureRequest).
	generator.SetReached("vehicle.state");
	CommandDefinition const* const command = fixture.Registry.Find("vehicle.state");
	REQUIRE(command != nullptr);

	REQUIRE(fixture.Play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	fixture.Play.SetPaused(true);
	fixture.Context.GetScene().ApplyPhysicsChanges();
	PhysicsScene* const physics = fixture.Context.GetScene().GetPhysicsScene();
	REQUIRE(physics != nullptr);
	REQUIRE(physics->HasVehicle(*bikeUuid));
	VehicleInputs inputs;
	inputs.Throttle = 0.4f;
	physics->SetVehicleInputs(*bikeUuid, inputs);
	int answered = 0;
	int bikes = 0;
	std::map<AutomationErrorCode, int> refused;
	std::optional<uint64_t> readState;
	int stateChecks = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		// Now and then a frame runs, so the bike rides between the reads.
		if (round % 25 == 24)
		{
			fixture.Play.Advance(1, Timestep(1.0f / 60.0f));
			readState.reset();
		}
		Json const params = round % 10 == 0 ? Json::object({{"entity", bikeId}}) : generator.Generate(command->Name, command->Parameters);
		std::string const request = params.dump();
		CAPTURE(round);
		CAPTURE(request);
		CommandResult const result = fixture.Run(command->Name, params);
		// The first read since the last frame applies what changed before it; the reads after it leave the world as it is.
		if (!readState)
		{
			readState = physics->ComputeStateHash();
		}
		else
		{
			CHECK(physics->ComputeStateHash() == *readState);
			stateChecks++;
		}
		if (result.IsOk())
		{
			answered++;
			for (Json const& vehicle : result.GetValue()["vehicles"])
			{
				bikes += vehicle["entity"] == bikeId ? 1 : 0;
				CHECK(std::isfinite(vehicle["engineSpeed"].get<double>()));
				CHECK(std::isfinite(vehicle["forwardSpeed"].get<double>()));
				CHECK(vehicle["wheels"].size() == 2);
			}
			continue;
		}
		// Refused for its parameters, or for the entity it names: none, one without a Vehicle, or one whose Vehicle makes no
		// vehicle.
		refused[result.GetError().Code]++;
		CHECK_MESSAGE((result.GetError().Code == AutomationErrorCode::InvalidParams ||
		               result.GetError().Code == AutomationErrorCode::EntityNotFound ||
		               result.GetError().Code == AutomationErrorCode::ComponentNotFound ||
		               result.GetError().Code == AutomationErrorCode::InvalidOperation),
		              result.GetError().Message);
	}
	CHECK(answered > 0);
	CHECK(bikes > 0);
	CHECK(stateChecks > 0);
	CHECK(refused[AutomationErrorCode::InvalidParams] + refused[AutomationErrorCode::EntityNotFound] +
	          refused[AutomationErrorCode::ComponentNotFound] >
	      0);
	// Stopped, it refuses; the scene the editor edits comes back as it was.
	fixture.Play.Stop();
	CommandResult const stopped = fixture.Run(command->Name, Json::object());
	REQUIRE(stopped.IsError());
	CHECK(stopped.GetError().Code == AutomationErrorCode::InvalidOperation);
	fixture.CheckSceneLoads();
	fixture.CheckHistory();
}

TEST_CASE("Automation: the history check leaves out the pose the simulation gave an entity whose controller or body left it")
{
	// What the fuzz reached at ten times its rounds, made by hand so that it stays covered whatever the random stream
	// reaches: while playing, the simulation moves an entity, then an undo takes its character controller or its dynamic
	// body away. The entity keeps the pose the simulation gave it, which no step of the history holds, so undoing and
	// redoing everything gives back the pose it was created with.
	FuzzFixture fixture;
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	auto const translationOf = [&fixture](std::string const& entity)
	{
		Json const transform = fixture.Ok("component.get", Json::object({{"entity", entity}, {"component", "Transform"}}));
		return transform["fields"]["Translation"].get<std::vector<double>>();
	};
	auto const create = [&fixture](std::string const& name, Json components)
	{
		return fixture.Ok("entity.create", Json::object({{"name", name}, {"components", std::move(components)}}))["id"].get<std::string>();
	};

	SUBCASE("a character held at the edge of the physics world")
	{
		double const beyond = static_cast<double>(MaxPhysicsCoordinate) * 1.0e3;
		std::string const distant = create("Far", {{"Transform", Json::object({{"Translation", {0.0, 0.0, beyond}}})}});
		fixture.Ok("component.add", Json::object({{"entity", distant}, {"component", "CharacterController"}}));
		// Physics holds the character at the edge of its world, and the entity takes the clamped translation once the
		// character's pose comes back to it.
		for (int frame = 0; frame < 120 && translationOf(distant).at(2) >= beyond; frame++)
		{
			fixture.Tick();
		}
		REQUIRE(translationOf(distant).at(2) < beyond);
		fixture.Ok("editor.undo");
	}
	SUBCASE("a dynamic body that fell")
	{
		std::string const faller =
			create("Faller", {{"Transform", Json::object({{"Translation", {100.0, 10.0, 0.0}}})}, {"SphereCollider", Json::object()}});
		fixture.Ok("component.add",
		           Json::object({{"entity", faller}, {"component", "RigidBody"}, {"fields", Json::object({{"Type", "Dynamic"}})}}));
		for (int frame = 0; frame < 120 && translationOf(faller).at(1) > 9.0; frame++)
		{
			fixture.Tick();
		}
		REQUIRE(translationOf(faller).at(1) <= 9.0);
		fixture.Ok("editor.undo");
	}

	// An edit after the undo leaves nothing to redo, as the fuzz's later commands did: the history no longer holds the
	// component, only the entity's creation.
	fixture.Ok("entity.create", Json::object({{"name", "After"}}));
	REQUIRE(fixture.Play.IsPlaying());
	CHECK(fixture.CheckHistory());
}
