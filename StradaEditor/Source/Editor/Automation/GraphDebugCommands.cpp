#include "Editor/Automation/GraphDebugCommands.h"

#include "Editor/Automation/JsonSchema.h"
#include "Editor/VisualScripting/VisualScriptBreakpoints.h"
#include "Editor/VisualScripting/VisualScriptDebugSession.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptDebugger.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr double DefaultTimestep = 1.0 / 60.0;
		constexpr int64_t MaxNodeID = 2147483647;
		constexpr uint32_t MaxTraceFrames = 600;
		constexpr uint32_t MaxRecorded = 100000;
		constexpr uint32_t DefaultRecorded = 500;
		constexpr uint32_t MaxEvaluated = 16;
		constexpr uint32_t MaxInstancesListed = 500;
		constexpr uint32_t DefaultInstancesListed = 100;
		constexpr double MinWaitSeconds = 0.1;
		constexpr double MaxWaitSeconds = 60.0;
		constexpr double DefaultWaitSeconds = 10.0;

		constexpr std::string_view PausedHint = "the scene is paused: play.advance runs frames";
		constexpr std::string_view NarrowHint = "pass graphs and nodes, or entity";
		// How frames name the event graph (the runtime names only functions), as the graph windows' breadcrumb does.
		constexpr std::string_view EventGraphName = "Event Graph";

		std::string_view GetReasonName(VisualScriptStopReason reason)
		{
			switch (reason)
			{
				case VisualScriptStopReason::Breakpoint:
					return "Breakpoint";
				case VisualScriptStopReason::Step:
					return "Step";
				case VisualScriptStopReason::Break:
					return "Break";
				case VisualScriptStopReason::RunTo:
					return "RunTo";
				case VisualScriptStopReason::Error:
					return "Error";
				case VisualScriptStopReason::Passed:
					return "Passed";
			}
			return "";
		}

		std::string_view GetPlaceName(VisualScriptStopPlace place)
		{
			switch (place)
			{
				case VisualScriptStopPlace::Enter:
					return "Enter";
				case VisualScriptStopPlace::Continue:
					return "Continue";
				case VisualScriptStopPlace::Error:
					return "Error";
			}
			return "";
		}

		std::string_view GetFrameKindName(VisualScriptStopFrameKind kind)
		{
			switch (kind)
			{
				case VisualScriptStopFrameKind::Graph:
					return "Graph";
				case VisualScriptStopFrameKind::Call:
					return "Call";
				case VisualScriptStopFrameKind::Delivery:
					return "Delivery";
				case VisualScriptStopFrameKind::Native:
					return "Native";
			}
			return "";
		}

		std::string_view GetStartKindName(VisualScriptStartKind kind)
		{
			switch (kind)
			{
				case VisualScriptStartKind::Event:
					return "Event";
				case VisualScriptStartKind::Tick:
					return "Tick";
				case VisualScriptStartKind::Timeline:
					return "Timeline";
				case VisualScriptStartKind::Resume:
					return "Resume";
				case VisualScriptStartKind::SendEvent:
					return "SendEvent";
				case VisualScriptStartKind::Delivery:
					return "Delivery";
			}
			return "";
		}

		// As graph.step's kind names the steps.
		std::string_view GetActionName(VisualScriptDebugAction action)
		{
			switch (action)
			{
				case VisualScriptDebugAction::Continue:
					return "Continue";
				case VisualScriptDebugAction::StepInto:
					return "Into";
				case VisualScriptDebugAction::StepOver:
					return "Over";
				case VisualScriptDebugAction::StepOut:
					return "Out";
			}
			return "";
		}

		std::string_view GetOutcomeName(VisualScriptResumeResult result)
		{
			switch (result)
			{
				case VisualScriptResumeResult::Stopped:
					return "Stopped";
				case VisualScriptResumeResult::Ended:
					return "Ended";
				case VisualScriptResumeResult::Waiting:
					return "Waiting";
				case VisualScriptResumeResult::Dismissed:
					return "Dismissed";
				case VisualScriptResumeResult::Armed:
					return "Armed";
				case VisualScriptResumeResult::Resumed:
					return "Resumed";
			}
			return "";
		}

		std::string_view GetCurrencyName(VisualScriptValueCurrency currency)
		{
			switch (currency)
			{
				case VisualScriptValueCurrency::Current:
					return "Current";
				case VisualScriptValueCurrency::Earlier:
					return "Earlier";
				case VisualScriptValueCurrency::NotComputed:
					return "NotComputed";
				case VisualScriptValueCurrency::Unknown:
					return "Unknown";
			}
			return "";
		}

		std::string_view GetSourceName(VisualScriptValueSource source)
		{
			switch (source)
			{
				case VisualScriptValueSource::Frame:
					return "Frame";
				case VisualScriptValueSource::LastCall:
					return "LastCall";
				case VisualScriptValueSource::None:
					return "None";
			}
			return "";
		}

		std::string_view GetInstanceStateName(VisualScriptInstanceState state)
		{
			switch (state)
			{
				case VisualScriptInstanceState::Idle:
					return "Idle";
				case VisualScriptInstanceState::Waiting:
					return "Waiting";
				case VisualScriptInstanceState::Stopped:
					return "Stopped";
			}
			return "";
		}

		CommandError MakeParameterError(std::string_view parameter, std::string const& message)
		{
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "{}: {}", parameter, message);
			error.Data = Json::object({{"path", std::string(parameter)}});
			return error;
		}

		// A graph's file relative to Assets; null for a graph without a file of its own (or an asset no longer registered).
		Json GetAssetPath(AssetHandle asset)
		{
			if (!AssetManager::IsInitialized())
			{
				return Json();
			}
			std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(asset);
			return metadata && metadata->IsFileAsset() ? Json(metadata->Path) : Json();
		}

		// How messages name a graph: its path, else its handle.
		std::string NameGraph(AssetHandle graph)
		{
			Json const path = GetAssetPath(graph);
			return path.is_string() ? path.get<std::string>() : graph.ToString();
		}

		Json DescribeHandle(AssetHandle asset)
		{
			return asset.IsValid() ? Json(asset.ToString()) : Json();
		}

		// A visual script parameter: a registered asset of that type.
		CommandValue<AssetHandle> ParseGraph(Json const& value, std::string_view parameter)
		{
			if (!AssetManager::IsInitialized())
			{
				return MakeCommandError(AutomationErrorCode::Unavailable, "assets are not available in this editor host");
			}
			std::string const& text = value.get_ref<std::string const&>();
			Result<AssetHandle> const handle = AssetManager::ResolveReference(text);
			if (!handle || !AssetManager::IsValid(handle.GetValue()))
			{
				CommandError error =
					MakeCommandError(AutomationErrorCode::AssetNotFound, "{}: no asset '{}' is registered", parameter, text);
				error.Data = Json::object({{"path", std::string(parameter)}});
				return error;
			}
			if (AssetType const type = AssetManager::GetAssetType(handle.GetValue()); type != AssetType::VisualScript)
			{
				return MakeParameterError(parameter,
				                          fmt::format("asset {} is a {}, not a visual script", handle.GetValue(), AssetTypeToString(type)));
			}
			return handle.GetValue();
		}

		// The graph's document as stored (the asset as loaded).
		CommandValue<Ref<VisualScriptDocument const>> LoadStoredDocument(AssetHandle graph)
		{
			if (AssetManager::IsMissing(graph))
			{
				return MakeCommandError(AutomationErrorCode::FileError, "{} does not load: its file is missing", NameGraph(graph));
			}
			Result<Ref<VisualScriptAsset>> asset = AssetManager::TryGetAsset<VisualScriptAsset>(graph);
			if (!asset)
			{
				return MakeCommandError(AutomationErrorCode::FileError, "{} does not load: {}", NameGraph(graph), asset.GetError());
			}
			return asset.GetValue()->GetSharedDocument();
		}

		// The scripting runtime's analysis of a stored document, for the titles of its nodes and their pins; null without the
		// runtime or when it fails.
		Ref<VisualScriptAnalysis const> AnalyzeStored(VisualScriptDocument const& document, AssetHandle graph)
		{
			if (!ScriptEngine::IsInitialized())
			{
				return nullptr;
			}
			Result<Ref<VisualScriptAnalysis const>> analysis = VisualScriptEngine::Analyze(document, graph);
			return analysis ? analysis.TakeValue() : nullptr;
		}

		VisualScriptNode const* FindNode(VisualScriptGraph const& graph, uint32_t node)
		{
			auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                                [node](VisualScriptNode const& candidate)
			                                {
												return candidate.ID == node;
											});
			return found != graph.Nodes.end() ? &*found : nullptr;
		}

		// The node of an ID in whichever sub-graph holds it, with that sub-graph's function (0: the event graph); null when none
		// does.
		std::pair<VisualScriptNode const*, uint32_t> FindDocumentNode(VisualScriptDocument const& document, uint32_t node)
		{
			if (VisualScriptNode const* found = FindNode(document.Graph, node))
			{
				return {found, 0};
			}
			for (VisualScriptFunction const& function : document.Functions)
			{
				if (VisualScriptNode const* found = FindNode(function.Graph, node))
				{
					return {found, function.ID};
				}
			}
			return {nullptr, 0};
		}

		VisualScriptGraph const* FindSubGraph(VisualScriptDocument const& document, uint32_t function)
		{
			if (function == 0)
			{
				return &document.Graph;
			}
			auto const found = std::find_if(document.Functions.begin(), document.Functions.end(),
			                                [function](VisualScriptFunction const& candidate)
			                                {
												return candidate.ID == function;
											});
			return found != document.Functions.end() ? &found->Graph : nullptr;
		}

		std::string GetNodeTitle(VisualScriptAnalysis const* analysis, VisualScriptNode const& node)
		{
			if (analysis != nullptr)
			{
				if (VisualScriptAnalyzedNode const* analyzed = analysis->FindNode(node.ID); analyzed != nullptr && !analyzed->Title.empty())
				{
					return analyzed->Title;
				}
			}
			return node.Type;
		}

		// Whether the flow enters the node (it has an exec pin): breakpoints on nodes no flow enters (pure nodes, a pure
		// function's Entry and Return) take no condition and no hit count. Unknown without the analysis.
		std::optional<bool> EntersFlow(VisualScriptAnalysis const* analysis, uint32_t node)
		{
			VisualScriptAnalyzedNode const* analyzed = analysis != nullptr ? analysis->FindNode(node) : nullptr;
			if (analyzed == nullptr)
			{
				return std::nullopt;
			}
			return std::any_of(analyzed->Pins.begin(), analyzed->Pins.end(),
			                   [](VisualScriptPin const& pin)
			                   {
								   return pin.Type == VisualScriptExecType || pin.CarriesExec;
							   });
		}

		// The decimal entity ID of a parameter; an error naming the parameter otherwise.
		CommandValue<UUID> ParseEntity(Json const& value, std::string_view parameter)
		{
			std::string const& text = value.get_ref<std::string const&>();
			std::optional<UUID> const id = UUID::FromString(text);
			if (!id || !id->IsValid())
			{
				return MakeParameterError(parameter, fmt::format("'{}' is not a valid entity ID", text));
			}
			return *id;
		}

		// How the debug commands write what the debugger reports: camelCase members, entities with their names in the scene
		// shown, and assets with their paths.
		class DebugJson
		{
		public:
			explicit DebugJson(Scene& scene)
				: m_Scene(scene),
				  m_Library(ScriptEngine::IsInitialized() ? VisualScriptEngine::GetLibrary() : nullptr)
			{
			}

			Json NameOf(UUID entity) const
			{
				Entity const found = entity.IsValid() ? m_Scene.GetEntityByUUID(entity) : Entity();
				return found ? Json(found.GetName()) : Json();
			}

			Json DescribeEntity(UUID entity) const { return entity.IsValid() ? Json(entity.ToString()) : Json(); }

			// A value with its name and type and exactly one of value (with path for an asset), entity (with entityName and
			// valid; a component or script instance has text too) and text, length and truncated for a cut string or array;
			// with currency, changed, and edited with before (a literal a live edit changed) for a stop's values; id and public
			// for variables.
			Json Value(VisualScriptDebugValue const& value, bool withCurrency) const
			{
				Json described = Json::object();
				if (!value.Name.empty())
				{
					described["name"] = value.Name;
				}
				if (!value.Type.empty())
				{
					described["type"] = value.Type;
				}
				AddContents(described, value);
				if (withCurrency)
				{
					described["currency"] = GetCurrencyName(value.Currency);
				}
				if (value.Changed)
				{
					described["changed"] = true;
				}
				if (value.Edited)
				{
					described["edited"] = true;
					described["before"] = Bare(value.Before);
				}
				if (value.ID != 0)
				{
					described["id"] = value.ID;
					described["public"] = value.Public;
				}
				return described;
			}

			Json Values(std::span<VisualScriptDebugValue const> values, bool withCurrency) const
			{
				Json described = Json::array();
				for (VisualScriptDebugValue const& value : values)
				{
					described.push_back(Value(value, withCurrency));
				}
				return described;
			}

			// A value without its type (a summary's first, last, min and max): the literal itself when it is one and whole, else
			// an object as Value writes its contents.
			Json BareValue(VisualScriptDebugValue const& value) const
			{
				if (!value.Entity && value.Text.empty() && !value.Truncated)
				{
					return Elements(value.Value);
				}
				Json described = Json::object();
				AddContents(described, value);
				return described;
			}

			// A value object the debugger writes without its type ({"Value"}, {"Entity"}, {"Text"} or {"Length"}, as array
			// elements and Before are), as BareValue writes it.
			Json Bare(Json const& bare) const
			{
				if (!bare.is_object())
				{
					return bare;
				}
				auto const truncated = bare.find("Truncated");
				bool const cut = truncated != bare.end() && truncated->is_boolean() && truncated->get<bool>();
				auto const value = bare.find("Value");
				if (value != bare.end() && !cut)
				{
					return Elements(*value);
				}
				Json described = Json::object();
				if (value != bare.end())
				{
					described["value"] = Elements(*value);
				}
				if (auto const text = bare.find("Text"); text != bare.end() && text->is_string())
				{
					described["text"] = *text;
				}
				if (auto const entity = bare.find("Entity"); entity != bare.end() && entity->is_string())
				{
					std::optional<UUID> const id = UUID::FromString(entity->get_ref<std::string const&>());
					auto const valid = bare.find("Valid");
					described["entity"] = *entity;
					described["entityName"] = id ? NameOf(*id) : Json();
					described["valid"] = valid == bare.end() || !valid->is_boolean() || valid->get<bool>();
				}
				if (auto const length = bare.find("Length"); length != bare.end() && length->is_number_unsigned())
				{
					described["length"] = *length;
				}
				if (cut)
				{
					described["truncated"] = true;
				}
				return described;
			}

			Json Stop(VisualScriptStop const& stop) const
			{
				Json frames = Json::array();
				for (VisualScriptStopFrame const& frame : stop.Frames)
				{
					frames.push_back(Frame(frame));
				}
				Json changed = Json::array();
				for (VisualScriptChangedValue const& value : stop.Changed)
				{
					changed.push_back(Json::object({{"node", value.Node}, {"name", value.Name}}));
				}
				Json breakpoint;
				if (stop.Breakpoint)
				{
					breakpoint = Json::object({{"asset", stop.Breakpoint->first.ToString()},
					                           {"path", GetAssetPath(stop.Breakpoint->first)},
					                           {"node", stop.Breakpoint->second}});
				}
				Json start = Json::object({{"kind", GetStartKindName(stop.Start.Kind)},
				                           {"graph", DescribeHandle(stop.Start.Graph)},
				                           {"document", DescribeHandle(stop.Start.Document)},
				                           {"path", GetAssetPath(stop.Start.Document)},
				                           {"node", stop.Start.Node},
				                           {"title", stop.Start.Title},
				                           {"pin", stop.Start.Pin}});
				start["origin"] = stop.Start.Origin ? Json::object({{"document", DescribeHandle(stop.Start.Origin->Document)},
				                                                    {"path", GetAssetPath(stop.Start.Origin->Document)},
				                                                    {"node", stop.Start.Origin->Node},
				                                                    {"title", stop.Start.Origin->Title}})
				                                    : Json();
				return Json::object({{"id", stop.ID},
				                     {"reason", GetReasonName(stop.Reason)},
				                     {"resumable", stop.Resumable},
				                     {"entity", DescribeEntity(stop.Entity)},
				                     {"name", NameOf(stop.Entity)},
				                     {"graph", DescribeHandle(stop.Graph)},
				                     {"path", GetAssetPath(stop.Graph)},
				                     {"place", GetPlaceName(stop.Place)},
				                     {"message", stop.Message},
				                     {"migrated", stop.Migrated},
				                     {"changed", std::move(changed)},
				                     {"changedMore", stop.ChangedMore},
				                     {"breakpoint", std::move(breakpoint)},
				                     {"frames", std::move(frames)},
				                     {"start", std::move(start)}});
			}

			Json StepWait(VisualScriptStepWait const& wait) const
			{
				return Json::object({{"asset", DescribeHandle(wait.Document)},
				                     {"path", GetAssetPath(wait.Document)},
				                     {"function", wait.Function},
				                     {"node", wait.Node},
				                     {"title", wait.Title},
				                     {"entity", DescribeEntity(wait.Entity)}});
			}

		private:
			// An array's elements as bare values; any other value as it is.
			Json Elements(Json const& value) const
			{
				if (!value.is_array())
				{
					return value;
				}
				Json elements = Json::array();
				for (Json const& element : value)
				{
					elements.push_back(Bare(element));
				}
				return elements;
			}

			void AddContents(Json& described, VisualScriptDebugValue const& value) const
			{
				if (value.Entity)
				{
					described["entity"] = value.Entity->ToString();
					described["entityName"] = NameOf(*value.Entity);
					described["valid"] = value.EntityValid;
					if (!value.Text.empty())
					{
						described["text"] = value.Text;
					}
				}
				else if (!value.Text.empty())
				{
					described["text"] = value.Text;
				}
				else
				{
					described["value"] = Elements(value.Value);
					if (value.Value.is_string() && IsAssetType(value.Type))
					{
						described["path"] = Json();
						if (AssetManager::IsInitialized())
						{
							if (Result<AssetHandle> const asset = AssetManager::ResolveReference(value.Value.get_ref<std::string const&>());
							    asset && asset.GetValue().IsValid())
							{
								described["path"] = GetAssetPath(asset.GetValue());
							}
						}
					}
				}
				if (value.Truncated)
				{
					described["truncated"] = true;
					if (value.Length)
					{
						described["length"] = *value.Length;
					}
				}
			}

			bool IsAssetType(std::string_view type) const
			{
				VisualScriptTypeInfo const* info = m_Library != nullptr && !type.empty() ? m_Library->FindType(type) : nullptr;
				return info != nullptr && info->Kind == VisualScriptTypeKind::Asset;
			}

			Json Frame(VisualScriptStopFrame const& frame) const
			{
				if (frame.Kind == VisualScriptStopFrameKind::Native)
				{
					return Json::object({{"kind", GetFrameKindName(frame.Kind)}, {"title", frame.Title}});
				}
				return Json::object(
					{{"kind", GetFrameKindName(frame.Kind)},
				     {"entity", DescribeEntity(frame.Entity)},
				     {"name", NameOf(frame.Entity)},
				     {"graph", DescribeHandle(frame.Graph)},
				     {"document", DescribeHandle(frame.Document)},
				     {"path", GetAssetPath(frame.Document)},
				     {"function", frame.Function},
				     {"functionName", frame.Function == 0 && frame.FunctionName.empty() ? std::string(EventGraphName) : frame.FunctionName},
				     {"node", frame.Node},
				     {"title", frame.Title},
				     {"pin", frame.Pin},
				     {"note", frame.Note}});
			}

			Scene& m_Scene;
			Ref<VisualScriptLibrary const> m_Library;
		};

		Result<void> Add(CommandRegistry& registry, std::string name, std::string description, Json parameters, bool readOnly,
		                 CommandHandler handler)
		{
			CommandDefinition definition;
			definition.Name = std::move(name);
			definition.Description = std::move(description);
			definition.Parameters = std::move(parameters);
			definition.ReadOnly = readOnly;
			definition.Handler = std::move(handler);
			return registry.Register(std::move(definition));
		}

		Result<void> AddAsync(CommandRegistry& registry, std::string name, std::string description, Json parameters,
		                      AsyncCommandHandler handler)
		{
			CommandDefinition definition;
			definition.Name = std::move(name);
			definition.Description = std::move(description);
			definition.Parameters = std::move(parameters);
			definition.AsyncHandler = std::move(handler);
			return registry.Register(std::move(definition));
		}

		SchemaBuilder GraphSchema(std::string_view description)
		{
			return SchemaBuilder::String(description).MinLength(1);
		}

		SchemaBuilder NodeSchema(std::string_view description)
		{
			return SchemaBuilder::Integer(description).Minimum(1).Maximum(MaxNodeID);
		}

		SchemaBuilder StopSchema(std::string_view description)
		{
			return SchemaBuilder::Integer(description).Minimum(1);
		}

		SchemaBuilder EntitySchema(std::string_view description)
		{
			return SchemaBuilder::String(description).MinLength(1);
		}

		// The commands, over the session, play mode and the context the editor layer owns.
		class GraphDebugCommands final : public std::enable_shared_from_this<GraphDebugCommands>
		{
		public:
			GraphDebugCommands(CommandRegistry& registry, VisualScriptDebugSession& session, PlayMode& playMode, EditorContext& context)
				: m_Registry(registry),
				  m_Session(session),
				  m_PlayMode(playMode),
				  m_Context(context)
			{
			}

			Result<void> Register()
			{
				Result<void> result = RegisterBreakpoints();
				result = result ? RegisterTarget() : result;
				result = result ? RegisterDebug() : result;
				result = result ? RegisterContinue() : result;
				result = result ? RegisterStep() : result;
				result = result ? RegisterBreak() : result;
				result = result ? RegisterWait() : result;
				result = result ? RegisterTrace() : result;
				result = result ? RegisterValues() : result;
				return result ? RegisterInstances() : result;
			}

		private:
			// Why the game cannot be debugged now: not playing or simulating (InvalidOperation), no scripting runtime, or (with
			// needsDebugger) no debugger attached (Unavailable).
			std::optional<CommandError> RequirePlaying(bool needsDebugger) const
			{
				if (!m_Context.IsPlaying())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "the editor is not playing (play.start)");
				}
				if (m_Context.GetPlayState() == EditorPlayState::Simulate)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "visual scripts do not run while simulating");
				}
				if (!ScriptEngine::IsInitialized())
				{
					return MakeCommandError(AutomationErrorCode::Unavailable,
					                        "the scripting runtime is not available, so no visual script runs");
				}
				if (needsDebugger && !m_Session.IsAttached())
				{
					return MakeCommandError(AutomationErrorCode::Unavailable,
					                        "the visual script debugger is not attached (test runs attach none)");
				}
				return std::nullopt;
			}

			// The stop of a parameter: an error naming the parameter when the session has none of the ID.
			CommandValue<VisualScriptStop const*> FindStop(Json const& value, std::string_view parameter) const
			{
				uint64_t const id = value.get<uint64_t>();
				VisualScriptStop const* stop = m_Session.FindStop(id);
				if (stop == nullptr)
				{
					return MakeParameterError(parameter, fmt::format("no stop has the ID {} (graph.debug lists the stops)", id));
				}
				return stop;
			}

			// The scene's entities that run the graph, by name, then ID.
			std::vector<std::pair<UUID, std::string>> FindInstances(AssetHandle graph) const
			{
				std::vector<std::pair<UUID, std::string>> instances;
				Scene& scene = m_Context.GetScene();
				for (entt::entity const handle : scene.GetAllEntitiesWith<VisualScriptComponent>())
				{
					Entity const entity(handle, &scene);
					if (entity.GetComponent<VisualScriptComponent>().Graph == graph)
					{
						instances.emplace_back(entity.GetUUID(), entity.GetName());
					}
				}
				std::sort(instances.begin(), instances.end(),
				          [](std::pair<UUID, std::string> const& a, std::pair<UUID, std::string> const& b)
				          {
							  return a.second != b.second ? a.second < b.second : a.first.GetValue() < b.first.GetValue();
						  });
				return instances;
			}

			bool RunsGraph(UUID entity, AssetHandle graph) const
			{
				Entity const found = m_Context.GetScene().GetEntityByUUID(entity);
				return found && found.HasComponent<VisualScriptComponent>() && found.GetComponent<VisualScriptComponent>().Graph == graph;
			}

			// An entity parameter that names an entity of the scene shown which runs the graph.
			CommandValue<UUID> ParseRunner(Json const& value, std::string_view parameter, AssetHandle graph) const
			{
				CommandValue<UUID> entity = ParseEntity(value, parameter);
				if (!entity)
				{
					return entity;
				}
				if (!m_Context.GetScene().HasEntity(entity.GetValue()))
				{
					return MakeCommandError(AutomationErrorCode::EntityNotFound, "entity {} does not exist", entity.GetValue());
				}
				if (!RunsGraph(entity.GetValue(), graph))
				{
					return MakeParameterError(parameter, fmt::format("entity {} does not run {}", entity.GetValue(), NameGraph(graph)));
				}
				return entity;
			}

			// graph.debug's result: the session's state as the runtime last reported it.
			Json DescribeState() const
			{
				DebugJson const json(m_Context.GetScene());
				VisualScriptDebugState const& state = m_Session.GetState();
				Json stops = Json::array();
				for (VisualScriptStop const& stop : state.Stops)
				{
					stops.push_back(json.Stop(stop));
				}
				Json stepping;
				if (state.Stepping)
				{
					stepping = Json::object({{"stop", state.Stepping->Stop},
					                         {"action", GetActionName(state.Stepping->Action)},
					                         {"waitsIn", state.Stepping->WaitsIn ? json.StepWait(*state.Stepping->WaitsIn) : Json()},
					                         {"needsFrames", m_PlayMode.IsPaused()}});
				}
				Json targets = Json::array();
				for (VisualScriptDebugTarget const& target : m_Session.GetTargets())
				{
					targets.push_back(Json::object({{"asset", target.Graph.ToString()},
					                                {"path", GetAssetPath(target.Graph)},
					                                {"entity", json.DescribeEntity(target.Entity)}}));
				}
				Json breakpoints = Json::array();
				for (Json& breakpoint : DescribeBreakpoints(std::nullopt, true))
				{
					breakpoints.push_back(Json::object({{"asset", std::move(breakpoint["asset"])},
					                                    {"path", std::move(breakpoint["path"])},
					                                    {"node", std::move(breakpoint["node"])},
					                                    {"title", std::move(breakpoint["title"])},
					                                    {"active", std::move(breakpoint["active"])},
					                                    {"inactiveReason", std::move(breakpoint["inactiveReason"])},
					                                    {"error", std::move(breakpoint["error"])}}));
				}
				return Json::object({{"attached", m_Session.IsAttached()},
				                     {"state", VisualScriptDebugSessionStateToString(m_Session.GetSessionState())},
				                     {"pauseHeld", m_Session.HasPausedScene()},
				                     {"current", state.Current ? Json(*state.Current) : Json()},
				                     {"stops", std::move(stops)},
				                     {"passed", state.Passed},
				                     {"stepping", std::move(stepping)},
				                     {"breakOnErrors", m_Session.IsBreakOnErrors()},
				                     {"targets", std::move(targets)},
				                     {"breakpoints", std::move(breakpoints)}});
			}

			// What the runtime reports of a breakpoint: its counters, why it cannot stop (null when it can), whether its condition
			// was checked against a running program, and the condition's error (null for none).
			struct BreakpointReport
			{
				VisualScriptBreakpointState const* Counted = nullptr;
				Json Inactive;
				bool Checked = false;
				Json Error;
			};

			// The breakpoints of a graph, or of every graph, by path and node, each with what the runtime counts for it; with
			// onlyNotWorking only those inactive or with an error, whose titles alone then need the graph's analysis.
			std::vector<Json> DescribeBreakpoints(std::optional<AssetHandle> only, bool onlyNotWorking = false) const
			{
				VisualScriptBreakpointStore const& store = m_Session.GetBreakpoints();
				std::vector<AssetHandle> graphs = only ? std::vector<AssetHandle>{*only} : store.GetGraphs();
				std::sort(graphs.begin(), graphs.end(),
				          [](AssetHandle a, AssetHandle b)
				          {
							  std::string const first = NameGraph(a);
							  std::string const second = NameGraph(b);
							  return first != second ? first < second : a.GetUUID().GetValue() < b.GetUUID().GetValue();
						  });
				// The runtime's counters change with every pass of a stop point, without a new stop: they are read now.
				std::optional<VisualScriptDebugState> state;
				if (m_Session.IsAttached())
				{
					if (Result<VisualScriptDebugState> read = VisualScriptDebugger::GetState(); read)
					{
						state = read.TakeValue();
					}
				}
				std::vector<Json> described;
				for (AssetHandle const graph : graphs)
				{
					std::span<VisualScriptBreakpoint const> const breakpoints = store.Get(graph);
					if (breakpoints.empty())
					{
						continue;
					}
					Ref<VisualScriptDocument const> document;
					if (AssetManager::IsInitialized() && AssetManager::IsValid(graph) && !AssetManager::IsMissing(graph))
					{
						if (Result<Ref<VisualScriptAsset>> asset = AssetManager::TryGetAsset<VisualScriptAsset>(graph); asset)
						{
							document = asset.GetValue()->GetSharedDocument();
						}
					}
					bool const library = document && document->Kind == VisualScriptDocumentKind::Library;
					UUID const target = m_Session.GetTarget(graph);
					// Analyzed for the titles, once per graph and only when one of its breakpoints is described.
					std::optional<Ref<VisualScriptAnalysis const>> analysis;
					for (VisualScriptBreakpoint const& breakpoint : breakpoints)
					{
						BreakpointReport report = ReportBreakpoint(graph, breakpoint, library, target, state ? &*state : nullptr);
						if (onlyNotWorking && report.Inactive.is_null() && report.Error.is_null())
						{
							continue;
						}
						if (!analysis)
						{
							analysis = document ? AnalyzeStored(*document, graph) : nullptr;
						}
						auto const [node, function] = document != nullptr ? FindDocumentNode(*document, breakpoint.Node)
						                                                  : std::pair<VisualScriptNode const*, uint32_t>(nullptr, 0);
						described.push_back(
							Json::object({{"asset", graph.ToString()},
						                  {"path", GetAssetPath(graph)},
						                  {"node", breakpoint.Node},
						                  {"function", function},
						                  {"title", node != nullptr ? GetNodeTitle(analysis->get(), *node) : breakpoint.Type},
						                  {"type", breakpoint.Type},
						                  {"enabled", breakpoint.Enabled},
						                  {"condition", breakpoint.Condition},
						                  {"hitCount", breakpoint.HitCount},
						                  {"reached", report.Counted != nullptr ? report.Counted->Reached : 0},
						                  {"hits", report.Counted != nullptr ? report.Counted->Hits : 0},
						                  {"active", report.Inactive.is_null()},
						                  {"inactiveReason", std::move(report.Inactive)},
						                  {"checked", report.Checked},
						                  {"error", std::move(report.Error)}}));
					}
				}
				return described;
			}

			BreakpointReport ReportBreakpoint(AssetHandle graph, VisualScriptBreakpoint const& breakpoint, bool library, UUID target,
			                                  VisualScriptDebugState const* state) const
			{
				BreakpointReport report;
				if (state != nullptr)
				{
					auto const found = std::find_if(state->Breakpoints.begin(), state->Breakpoints.end(),
					                                [graph, &breakpoint](VisualScriptBreakpointState const& candidate)
					                                {
														return candidate.Document == graph && candidate.Node == breakpoint.Node;
													});
					report.Counted = found != state->Breakpoints.end() ? &*found : nullptr;
				}
				VisualScriptBreakpointState const* counted = report.Counted;

				// Inactive: not in the program that runs, a target that is gone, or nothing runs it.
				if (counted != nullptr && counted->Reason == VisualScriptBreakpointInactivity::NotInProgram)
				{
					report.Inactive = "not in the running program: the graph has errors";
				}
				else if (!library && target.IsValid() && m_Session.IsAttached() && !m_Context.GetScene().HasEntity(target))
				{
					report.Inactive = fmt::format("the target {} was destroyed", target);
				}
				else if (counted == nullptr || !counted->Active)
				{
					report.Inactive = library ? "no running graph calls this library" : "no entity runs it";
				}

				// The condition is checked against a program that compiles the document, when one is loaded.
				report.Checked = breakpoint.Condition.empty();
				if (counted != nullptr && !counted->Error.empty())
				{
					report.Error = counted->Error;
				}
				if (!breakpoint.Condition.empty() && ScriptEngine::IsInitialized())
				{
					if (Result<VisualScriptConditionCheck> check =
					        VisualScriptDebugger::CheckCondition(graph, breakpoint.Node, breakpoint.Condition);
					    check)
					{
						report.Checked = check.GetValue().Checked;
						if (report.Error.is_null() && check.GetValue().Error)
						{
							report.Error = *check.GetValue().Error;
						}
					}
				}
				return report;
			}

			// graph.continue's and graph.step's result.
			Json DescribeResume(VisualScriptResumeOutcome const& outcome) const
			{
				DebugJson const json(m_Context.GetScene());
				VisualScriptStop const* stop = outcome.Stop ? m_Session.FindStop(*outcome.Stop) : nullptr;
				return Json::object({{"outcome", GetOutcomeName(outcome.Result)},
				                     {"failed", outcome.Failed},
				                     {"stop", stop != nullptr ? json.Stop(*stop) : Json()},
				                     {"paused", m_PlayMode.IsPaused()},
				                     {"state", DescribeState()}});
			}

			// --- graph.breakpoints ---

			struct BreakpointAddition
			{
				uint32_t Node = 0;
				std::optional<bool> Enabled;
				std::optional<std::string> Condition;
				std::optional<uint32_t> HitCount;
			};

			Result<void> RegisterBreakpoints()
			{
				return Add(
					m_Registry, "graph.breakpoints",
					"Sets, changes and lists the visual-script breakpoints (per user and project, never in the graph files; not "
					"undoable). graph: the graph whose breakpoints change (omitted: lists every graph's). Applied in this order: clear "
					"(with graph that graph's, without it every graph's), remove (node IDs), add ([{ node, enabled, condition, "
					"hitCount }]: merges, so for a node that has a breakpoint only the members given change; a new one is enabled with "
					"no condition and hit count 0), breakOnErrors (stop at a targeted execution's aborting error). A condition is a "
					"formula that must be true for the breakpoint to stop (\"Health < 0\", \"Index == 3\", State == \"Serve\", "
					"Target != None), checked against the running program when one is loaded; hitCount n lets hits 1 to n - 1 run on. "
					"Nodes no flow enters (pure nodes) take neither. Returns breakpoints: [{ asset, path, node, function, title, type, "
					"enabled, condition, hitCount, reached, hits, active, inactiveReason, checked, error }] and breakOnErrors.",
					SchemaBuilder::Object()
						.Property("graph", GraphSchema("Graph handle or \"asset://<path>.sgraph\""))
						.Property("clear",
				                  SchemaBuilder::Boolean("Remove the graph's breakpoints (every graph's without graph)").Default(false))
						.Property("remove", SchemaBuilder::Array(NodeSchema("Node ID"), "Node IDs whose breakpoints go")
				                                .MaxItems(VisualScriptBreakpointStore::MaxBreakpointsPerGraph))
						.Property("add", SchemaBuilder::Array(
											 SchemaBuilder::Object("A breakpoint on a node of the graph's stored document")
												 .Property("node", NodeSchema("Node ID"), true)
												 .Property("enabled", SchemaBuilder::Boolean("Whether it stops"))
												 .Property("condition", SchemaBuilder::String("A formula that must hold; empty: every hit")
				                                                            .MaxLength(VisualScriptBreakpointStore::MaxConditionBytes))
												 .Property("hitCount", SchemaBuilder::Integer("Stop from the nth hit on; 0: every hit")
				                                                           .Minimum(0)
				                                                           .Maximum(VisualScriptBreakpointStore::MaxHitCount)),
											 "Breakpoints to add or change")
				                             .MaxItems(VisualScriptBreakpointStore::MaxBreakpointsPerGraph))
						.Property("breakOnErrors", SchemaBuilder::Boolean("Stop at aborting errors of targeted executions"))
						.Build(),
					false,
					[self = shared_from_this()](Json const& params) -> CommandResult
					{
						return self->Breakpoints(params);
					});
			}

			CommandResult Breakpoints(Json const& params)
			{
				auto const graphParameter = params.find("graph");
				for (char const* const member : {"remove", "add"})
				{
					if (params.contains(member) && graphParameter == params.end())
					{
						return MakeParameterError(member, "breakpoints are set on a graph: pass graph");
					}
				}
				std::optional<AssetHandle> graph;
				if (graphParameter != params.end())
				{
					CommandValue<AssetHandle> parsed = ParseGraph(*graphParameter, "graph");
					if (!parsed)
					{
						return parsed.TakeError();
					}
					graph = parsed.GetValue();
				}
				m_Session.Refresh();

				// Everything is checked before anything changes.
				std::vector<BreakpointAddition> additions;
				if (auto const add = params.find("add"); add != params.end() && !add->empty())
				{
					CommandValue<Ref<VisualScriptDocument const>> document = LoadStoredDocument(*graph);
					if (!document)
					{
						return document.TakeError();
					}
					Ref<VisualScriptAnalysis const> const analysis = AnalyzeStored(*document.GetValue(), *graph);
					for (size_t index = 0; index < add->size(); index++)
					{
						CommandValue<BreakpointAddition> addition =
							ParseAddition((*add)[index], index, *graph, *document.GetValue(), analysis.get());
						if (!addition)
						{
							return addition.TakeError();
						}
						additions.push_back(addition.TakeValue());
					}
				}
				std::vector<uint32_t> removed;
				if (auto const remove = params.find("remove"); remove != params.end())
				{
					for (Json const& node : *remove)
					{
						removed.push_back(node.get<uint32_t>());
					}
				}
				bool const clear = params.value("clear", false);
				if (graph && !additions.empty())
				{
					// The graph's breakpoints afterwards stay within the store's limit.
					std::vector<uint32_t> nodes;
					if (!clear)
					{
						for (VisualScriptBreakpoint const& breakpoint : m_Session.GetBreakpoints().Get(*graph))
						{
							if (std::find(removed.begin(), removed.end(), breakpoint.Node) == removed.end())
							{
								nodes.push_back(breakpoint.Node);
							}
						}
					}
					for (BreakpointAddition const& addition : additions)
					{
						if (std::find(nodes.begin(), nodes.end(), addition.Node) == nodes.end())
						{
							nodes.push_back(addition.Node);
						}
					}
					if (nodes.size() > VisualScriptBreakpointStore::MaxBreakpointsPerGraph)
					{
						return MakeParameterError("add", fmt::format("a graph has at most {} breakpoints; {} would have {}",
						                                             VisualScriptBreakpointStore::MaxBreakpointsPerGraph, NameGraph(*graph),
						                                             nodes.size()));
					}
				}

				if (clear)
				{
					m_Session.ClearBreakpoints(graph);
				}
				if (!removed.empty())
				{
					m_Session.RemoveBreakpoints(*graph, removed);
				}
				for (size_t index = 0; index < additions.size(); index++)
				{
					BreakpointAddition const& addition = additions[index];
					VisualScriptBreakpoint const* existing = m_Session.GetBreakpoints().Find(*graph, addition.Node);
					VisualScriptBreakpoint breakpoint = existing != nullptr ? *existing : VisualScriptBreakpoint();
					breakpoint.Node = addition.Node;
					breakpoint.Enabled = addition.Enabled.value_or(breakpoint.Enabled);
					breakpoint.Condition = addition.Condition.value_or(breakpoint.Condition);
					breakpoint.HitCount = addition.HitCount.value_or(breakpoint.HitCount);
					if (Result<void> set = m_Session.SetBreakpoint(*graph, std::move(breakpoint)); !set)
					{
						return MakeParameterError(fmt::format("add[{}]", index), set.GetError());
					}
				}
				if (auto const breakOnErrors = params.find("breakOnErrors"); breakOnErrors != params.end())
				{
					m_Session.SetBreakOnErrors(breakOnErrors->get<bool>());
				}
				Json breakpoints = Json::array();
				for (Json& breakpoint : DescribeBreakpoints(graph))
				{
					breakpoints.push_back(std::move(breakpoint));
				}
				return Json::object({{"breakpoints", std::move(breakpoints)}, {"breakOnErrors", m_Session.IsBreakOnErrors()}});
			}

			CommandValue<BreakpointAddition> ParseAddition(Json const& item, size_t index, AssetHandle graph,
			                                               VisualScriptDocument const& document, VisualScriptAnalysis const* analysis) const
			{
				std::string const at = fmt::format("add[{}]", index);
				BreakpointAddition addition;
				addition.Node = item.at("node").get<uint32_t>();
				VisualScriptNode const* node = FindDocumentNode(document, addition.Node).first;
				if (node == nullptr)
				{
					return MakeParameterError(at + ".node", fmt::format("{} has no node {}", NameGraph(graph), addition.Node));
				}
				if (auto const enabled = item.find("enabled"); enabled != item.end())
				{
					addition.Enabled = enabled->get<bool>();
				}
				if (auto const hitCount = item.find("hitCount"); hitCount != item.end())
				{
					addition.HitCount = hitCount->get<uint32_t>();
				}
				if (auto const condition = item.find("condition"); condition != item.end())
				{
					addition.Condition = condition->get<std::string>();
				}
				bool const counts = (addition.Condition && !addition.Condition->empty()) || (addition.HitCount && *addition.HitCount != 0);
				if (counts && EntersFlow(analysis, addition.Node) == false)
				{
					return MakeParameterError(addition.Condition && !addition.Condition->empty() ? at + ".condition" : at + ".hitCount",
					                          fmt::format("no flow enters {} (node {}): a breakpoint on a pure node stops where its value "
					                                      "is read, so it takes no condition and no hit count",
					                                      GetNodeTitle(analysis, *node), addition.Node));
				}
				if (addition.Condition && addition.Condition->size() > VisualScriptBreakpointStore::MaxConditionBytes)
				{
					return MakeParameterError(
						at + ".condition", fmt::format("a condition has at most {} bytes", VisualScriptBreakpointStore::MaxConditionBytes));
				}
				// What else the store refuses (a condition that is not UTF-8), so that no addition fails once others were made.
				VisualScriptBreakpoint checked;
				checked.Node = addition.Node;
				checked.Type = node->Type;
				checked.Condition = addition.Condition.value_or(std::string());
				checked.HitCount = addition.HitCount.value_or(0);
				if (Result<void> const valid = VisualScriptBreakpointStore::Check(checked); !valid)
				{
					return MakeParameterError(at, valid.GetError());
				}
				if (addition.Condition && !addition.Condition->empty() && ScriptEngine::IsInitialized())
				{
					Result<VisualScriptConditionCheck> check =
						VisualScriptDebugger::CheckCondition(graph, addition.Node, *addition.Condition);
					if (!check)
					{
						return MakeParameterError(at + ".condition", check.GetError());
					}
					if (check.GetValue().Checked && check.GetValue().Error)
					{
						return MakeParameterError(at + ".condition", *check.GetValue().Error);
					}
				}
				return addition;
			}

			// --- graph.target ---

			Result<void> RegisterTarget()
			{
				return Add(m_Registry, "graph.target",
				           "Reads or sets which instance of a script graph its breakpoints stop: one entity that runs it, or any "
				           "instance (the default; graph windows show the same choice as their instance). With neither entity nor "
				           "any it only reads. Returns { asset, path, entity (null: any instance), instances: [{ entity, name }] (the "
				           "scene's entities that run it) }.",
				           SchemaBuilder::Object()
				               .Property("graph", GraphSchema("Graph handle or \"asset://<path>.sgraph\""), true)
				               .Property("entity", EntitySchema("An entity that runs the graph"))
				               .Property("any", SchemaBuilder::Boolean("Stop any instance"))
				               .Build(),
				           false,
				           [self = shared_from_this()](Json const& params) -> CommandResult
				           {
							   return self->Target(params);
						   });
			}

			CommandResult Target(Json const& params)
			{
				CommandValue<AssetHandle> graph = ParseGraph(params.at("graph"), "graph");
				if (!graph)
				{
					return graph.TakeError();
				}
				auto const entity = params.find("entity");
				bool const any = params.value("any", false);
				if (entity != params.end() && any)
				{
					return MakeParameterError("any", "pass one of entity and any");
				}
				if (entity != params.end() || any)
				{
					CommandValue<Ref<VisualScriptDocument const>> document = LoadStoredDocument(graph.GetValue());
					if (!document)
					{
						return document.TakeError();
					}
					if (document.GetValue()->Kind == VisualScriptDocumentKind::Library)
					{
						return MakeParameterError("graph", "a function library runs on no entity: its callers' targets choose");
					}
				}
				if (entity != params.end())
				{
					CommandValue<UUID> runner = ParseRunner(*entity, "entity", graph.GetValue());
					if (!runner)
					{
						return runner.TakeError();
					}
					m_Session.SetTarget(graph.GetValue(), runner.GetValue());
				}
				else if (any)
				{
					m_Session.SetTarget(graph.GetValue(), UUID::Invalid());
				}
				Json instances = Json::array();
				for (auto const& [id, name] : FindInstances(graph.GetValue()))
				{
					instances.push_back(Json::object({{"entity", id.ToString()}, {"name", name}}));
				}
				UUID const target = m_Session.GetTarget(graph.GetValue());
				return Json::object({{"asset", graph.GetValue().ToString()},
				                     {"path", GetAssetPath(graph.GetValue())},
				                     {"entity", target.IsValid() ? Json(target.ToString()) : Json()},
				                     {"instances", std::move(instances)}});
			}

			// --- graph.debug ---

			Result<void> RegisterDebug()
			{
				return Add(
					m_Registry, "graph.debug",
					"The visual-script debugger's state: { attached, state (Detached, Running, Stopped, Stepping, StepEnded), "
					"pauseHeld (the debugger paused the scene and has not resumed it), current (the current stop's ID or null), "
					"stops (each { id, reason (Breakpoint, Step, Break, RunTo, Error, Passed), resumable, entity, name, graph, path, "
					"place, message, migrated (an edit reached it), changed, changedMore, breakpoint, frames (the call stack, "
					"innermost first: { kind, entity, name, graph, document, path, function, functionName, node, title, pin, note "
					"}), start }), passed (hits beyond the stops kept), stepping ({ stop, action, waitsIn, needsFrames } or null), "
					"breakOnErrors, targets, breakpoints (those inactive or with an error) }.",
					Json(), true,
					[self = shared_from_this()](Json const&) -> CommandResult
					{
						return self->Debug();
					});
			}

			CommandResult Debug()
			{
				m_Session.Refresh();
				return DescribeState();
			}

			// --- graph.continue ---

			Result<void> RegisterContinue()
			{
				return Add(
					m_Registry, "graph.continue",
					"Continues stopped visual-script executions between frames (no frame runs): every stop, or one (stop), or with to "
					"until a node (with nothing stopped it only arms: the next execution that enters the node stops). Play resumes "
					"only when the debugger paused it; with nothing stopped and the pause held it resumes play (Resumed). Returns "
					"{ outcome (Stopped, Ended, Waiting, Dismissed, Armed, Resumed), failed (an error ended an execution), stop (the "
					"stop reached, or null), paused, state (as graph.debug) }.",
					SchemaBuilder::Object()
						.Property("stop", StopSchema("The stop to continue (default every stop)"))
						.Property("to", SchemaBuilder::Object("Run to a node")
				                            .Property("graph", GraphSchema("The graph or library holding the node"), true)
				                            .Property("node", NodeSchema("Node ID"), true))
						.Build(),
					false,
					[self = shared_from_this()](Json const& params) -> CommandResult
					{
						return self->Continue(params);
					});
			}

			CommandResult Continue(Json const& params)
			{
				auto const stopParameter = params.find("stop");
				auto const to = params.find("to");
				std::optional<std::pair<AssetHandle, uint32_t>> target;
				if (to != params.end())
				{
					if (stopParameter != params.end())
					{
						return MakeParameterError("to", "to continues every stop: pass stop or to");
					}
					CommandValue<AssetHandle> graph = ParseGraph(to->at("graph"), "to.graph");
					if (!graph)
					{
						return graph.TakeError();
					}
					CommandValue<Ref<VisualScriptDocument const>> document = LoadStoredDocument(graph.GetValue());
					if (!document)
					{
						return document.TakeError();
					}
					uint32_t const node = to->at("node").get<uint32_t>();
					if (FindDocumentNode(*document.GetValue(), node).first == nullptr)
					{
						return MakeParameterError("to.node", fmt::format("{} has no node {}", NameGraph(graph.GetValue()), node));
					}
					target.emplace(graph.GetValue(), node);
				}
				if (std::optional<CommandError> refused = RequirePlaying(true))
				{
					return std::move(*refused);
				}
				m_Session.Refresh();
				std::optional<uint64_t> stop;
				if (stopParameter != params.end())
				{
					CommandValue<VisualScriptStop const*> found = FindStop(*stopParameter, "stop");
					if (!found)
					{
						return found.TakeError();
					}
					stop = found.GetValue()->ID;
				}
				if (!target && m_Session.GetState().Stops.empty() && !m_Session.HasPausedScene() &&
				    m_Session.GetSessionState() != VisualScriptDebugSessionState::StepEnded)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation,
					                        "nothing is stopped: no execution to continue (to arms a run to a node)");
				}
				Result<VisualScriptResumeOutcome> const resumed =
					target ? m_Session.RunTo(target->first, target->second)
						   : m_Session.Resume(stop, VisualScriptDebugAction::Continue, !stop.has_value());
				if (!resumed)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", resumed.GetError());
				}
				return DescribeResume(resumed.GetValue());
			}

			// --- graph.step ---

			Result<void> RegisterStep()
			{
				return Add(
					m_Registry, "graph.step",
					"Steps a stopped visual-script execution between frames: Into (into calls and receivers), Over (to the next "
					"node at the same depth) or Out (out of the function or receiver). A step that reaches a node answers Stopped "
					"with the new stop; one that waits in a latent node (a Delay) answers Waiting and stops after it once frames "
					"ran (play resumes if the debugger paused it); after a step ended its execution (StepEnded), Into and Over arm "
					"a break at that graph's next node (Armed). needsFrames: the scene stays paused (play.start paused, play.pause), "
					"so play.advance must run frames. Returns graph.continue's result plus needsFrames and hint.",
					SchemaBuilder::Object()
						.Property("kind", SchemaBuilder::String("How far to step").Enum({"Into", "Over", "Out"}), true)
						.Property("stop", StopSchema("The stop to step (default the current one)"))
						.Build(),
					false,
					[self = shared_from_this()](Json const& params) -> CommandResult
					{
						return self->Step(params);
					});
			}

			CommandResult Step(Json const& params)
			{
				std::string const& kind = params.at("kind").get_ref<std::string const&>();
				VisualScriptDebugAction const action = kind == "Into"   ? VisualScriptDebugAction::StepInto
				                                       : kind == "Over" ? VisualScriptDebugAction::StepOver
				                                                        : VisualScriptDebugAction::StepOut;
				if (std::optional<CommandError> refused = RequirePlaying(true))
				{
					return std::move(*refused);
				}
				m_Session.Refresh();
				VisualScriptDebugState const& state = m_Session.GetState();
				std::optional<uint64_t> stop;
				if (auto const stopParameter = params.find("stop"); stopParameter != params.end())
				{
					CommandValue<VisualScriptStop const*> found = FindStop(*stopParameter, "stop");
					if (!found)
					{
						return found.TakeError();
					}
					if (!found.GetValue()->Resumable)
					{
						return MakeParameterError("stop", fmt::format("stop {} is read-only (a passed breakpoint or an error): "
						                                              "graph.continue dismisses it",
						                                              found.GetValue()->ID));
					}
					stop = found.GetValue()->ID;
				}
				else
				{
					// The current stop, or the first that can be stepped when the current one is read-only.
					VisualScriptStop const* current = state.Current ? m_Session.FindStop(*state.Current) : nullptr;
					if (current == nullptr || !current->Resumable)
					{
						auto const resumable = std::find_if(state.Stops.begin(), state.Stops.end(),
						                                    [](VisualScriptStop const& candidate)
						                                    {
																return candidate.Resumable;
															});
						current = resumable != state.Stops.end() ? &*resumable : nullptr;
					}
					if (current != nullptr)
					{
						stop = current->ID;
					}
					else if (m_Session.GetSessionState() != VisualScriptDebugSessionState::StepEnded)
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation, "nothing is stopped: no execution to step");
					}
					else if (action == VisualScriptDebugAction::StepOut)
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation, "the execution ended: nothing to step out of");
					}
				}
				Result<VisualScriptResumeOutcome> resumed = m_Session.Resume(stop, action);
				if (!resumed)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", resumed.GetError());
				}
				VisualScriptResumeResult const result = resumed.GetValue().Result;
				bool const needsFrames =
					(result == VisualScriptResumeResult::Waiting || result == VisualScriptResumeResult::Armed) && m_PlayMode.IsPaused();
				Json described = DescribeResume(resumed.GetValue());
				described["needsFrames"] = needsFrames;
				described["hint"] = needsFrames ? Json(std::string(PausedHint)) : Json();
				return described;
			}

			// --- graph.break ---

			Result<void> RegisterBreak()
			{
				return Add(m_Registry, "graph.break",
				           "Arms a break: the next node a targeted visual-script execution reaches in a frame that runs stops it (it "
				           "unpauses nothing). graph: only executions of that script graph, or nodes of that library. cancel: cancels "
				           "the armed break and a step that waits. Returns the state, as graph.debug.",
				           SchemaBuilder::Object()
				               .Property("cancel", SchemaBuilder::Boolean("Cancel the armed break and a waiting step").Default(false))
				               .Property("graph", GraphSchema("Only this graph's executions, or this library's nodes"))
				               .Build(),
				           false,
				           [self = shared_from_this()](Json const& params) -> CommandResult
				           {
							   return self->Break(params);
						   });
			}

			CommandResult Break(Json const& params)
			{
				std::optional<AssetHandle> graph;
				if (auto const parameter = params.find("graph"); parameter != params.end())
				{
					CommandValue<AssetHandle> parsed = ParseGraph(*parameter, "graph");
					if (!parsed)
					{
						return parsed.TakeError();
					}
					graph = parsed.GetValue();
				}
				if (std::optional<CommandError> refused = RequirePlaying(true))
				{
					return std::move(*refused);
				}
				m_Session.Refresh();
				if (Result<void> armed = m_Session.Break(params.value("cancel", false), graph); !armed)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", armed.GetError());
				}
				return DescribeState();
			}

			// --- graph.wait ---

			Result<void> RegisterWait()
			{
				return AddAsync(
					m_Registry, "graph.wait",
					"Waits in real time until a visual-script execution stops, at most timeout seconds; asynchronous. Answers at once "
					"when a stop exists, nothing plays, or no frame can run (the scene is paused and nothing is stopped: Paused, whose "
					"hint says what runs frames). Returns { stopped, stop (as graph.debug's stops, or null), reason (Stopped, Timeout, "
					"NotPlaying, PlayStopped, Paused), hint }.",
					SchemaBuilder::Object()
						.Property("timeout", SchemaBuilder::Number("Real seconds")
				                                 .Minimum(MinWaitSeconds)
				                                 .Maximum(MaxWaitSeconds)
				                                 .Default(DefaultWaitSeconds))
						.Build(),
					[self = shared_from_this()](Json const& params, CommandCompletion completion)
					{
						self->Wait(params, std::move(completion));
					});
			}

			void Wait(Json const& params, CommandCompletion completion)
			{
				m_Session.Refresh();
				auto const timeout = params.find("timeout");
				// The session answers the stop's ID: the result names the stop as graph.debug does. An abandoned wait answers
				// Cancelled when the session goes, so only a result reads the session.
				CommandCompletion described(
					[self = shared_from_this(), completion](CommandResult result)
					{
						if (result.IsOk())
						{
							Json& value = result.GetValue();
							if (value["stop"].is_number_unsigned())
							{
								VisualScriptStop const* stop = self->m_Session.FindStop(value["stop"].get<uint64_t>());
								value["stop"] = stop != nullptr ? DebugJson(self->m_Context.GetScene()).Stop(*stop) : Json();
							}
						}
						completion.Complete(std::move(result));
					});
				m_Session.WaitForStop(timeout != params.end() ? timeout->get<double>() : DefaultWaitSeconds, std::move(described));
			}

			// --- graph.trace ---

			Result<void> RegisterTrace()
			{
				return Add(
					m_Registry, "graph.trace",
					"Runs frames at once, as play.advance does, recording which visual-script nodes ran and with which values; "
					"refused while an execution is stopped. entity: only that instance (default each graph's target); graphs: only "
					"steps in these documents; nodes: only these node IDs of graphs; values: the values of impure nodes' inputs and "
					"of outputs. summary (the default): per graph { asset, path, function, steps: [{ node, title, exit, count, "
					"firstFrame, lastFrame }], values: [{ node, pin, direction, type, count, first, last, min, max }] } (min and max "
					"for numbers); summary false: every step in order ({ i, frame, execution, entity, asset, path, function, node, "
					"title, exit }) and values ({ step, node, pin, direction, type, value }). Frames count from 0, the first frame "
					"run. A breakpoint that stops ends the trace after that frame. Returns { framesRun, stopped, stop, summary or "
					"steps and values, truncated: { steps, values, hint } }.",
					SchemaBuilder::Object()
						.Property("frames", SchemaBuilder::Integer("Frames to run").Minimum(1).Maximum(MaxTraceFrames), true)
						.Property("timestep",
				                  SchemaBuilder::Number("Seconds per frame").Minimum(0.0001).Maximum(1.0).Default(DefaultTimestep))
						.Property("entity", EntitySchema("Only this instance"))
						.Property("graphs",
				                  SchemaBuilder::Array(GraphSchema("A graph or library"), "Only steps in these documents").MaxItems(64))
						.Property("nodes", SchemaBuilder::Array(NodeSchema("Node ID"), "Only these nodes of graphs").MaxItems(4096))
						.Property("values", SchemaBuilder::Boolean("Record values").Default(true))
						.Property("summary", SchemaBuilder::Boolean("Summarize per node and pin").Default(true))
						.Property("maxSteps",
				                  SchemaBuilder::Integer("Steps listed at most").Minimum(1).Maximum(MaxRecorded).Default(DefaultRecorded))
						.Property("maxValues",
				                  SchemaBuilder::Integer("Values listed at most").Minimum(1).Maximum(MaxRecorded).Default(DefaultRecorded))
						.Build(),
					false,
					[self = shared_from_this()](Json const& params) -> CommandResult
					{
						return self->Trace(params);
					});
			}

			CommandResult Trace(Json const& params)
			{
				VisualScriptRecordingOptions options;
				if (auto const graphs = params.find("graphs"); graphs != params.end())
				{
					for (size_t index = 0; index < graphs->size(); index++)
					{
						CommandValue<AssetHandle> graph = ParseGraph((*graphs)[index], fmt::format("graphs[{}]", index));
						if (!graph)
						{
							return graph.TakeError();
						}
						options.Documents.push_back(graph.GetValue());
					}
				}
				if (auto const nodes = params.find("nodes"); nodes != params.end())
				{
					if (options.Documents.empty())
					{
						return MakeParameterError("nodes", "node IDs repeat across graphs: pass graphs");
					}
					for (Json const& node : *nodes)
					{
						options.Nodes.push_back(node.get<uint32_t>());
					}
				}
				if (std::optional<CommandError> refused = RequirePlaying(true))
				{
					return std::move(*refused);
				}
				m_Session.Refresh();
				if (std::optional<std::string> const refusal = m_PlayMode.GetFrameStepRefusal())
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", *refusal);
				}
				if (auto const entity = params.find("entity"); entity != params.end())
				{
					CommandValue<UUID> id = ParseEntity(*entity, "entity");
					if (!id)
					{
						return id.TakeError();
					}
					if (!m_Context.GetScene().HasEntity(id.GetValue()))
					{
						return MakeCommandError(AutomationErrorCode::EntityNotFound, "entity {} does not exist", id.GetValue());
					}
					options.Entity = id.GetValue();
				}
				options.Values = params.value("values", true);
				options.Summary = params.value("summary", true);
				options.MaxSteps = params.value("maxSteps", DefaultRecorded);
				options.MaxValues = params.value("maxValues", DefaultRecorded);
				auto const timestep = params.find("timestep");
				Timestep const step(static_cast<float>(timestep != params.end() ? timestep->get<double>() : DefaultTimestep));
				uint32_t const frames = params.at("frames").get<uint32_t>();

				if (Result<void> started = VisualScriptDebugger::StartRecording(options); !started)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError());
				}
				uint64_t const lastStop = m_PlayMode.GetDebugSummary().LastStop;
				uint32_t framesRun = 0;
				bool stopped = false;
				while (framesRun < frames && m_PlayMode.IsPlaying() && m_Session.IsAttached())
				{
					if (Result<void> marked = VisualScriptDebugger::MarkRecordingFrame(framesRun); !marked)
					{
						ST_WARN("graph.trace cannot mark frame {}: {}", framesRun, marked.GetError());
						break;
					}
					if (m_PlayMode.Advance(1, step) == 0)
					{
						break;
					}
					framesRun++;
					if (m_PlayMode.GetDebugSummary().LastStop > lastStop)
					{
						stopped = true;
						break;
					}
				}

				Json described = Json::object({{"framesRun", framesRun}, {"stopped", stopped}});
				PlayDebugSummary const summary = m_PlayMode.GetDebugSummary();
				VisualScriptStop const* stop = stopped && summary.Current ? m_Session.FindStop(*summary.Current) : nullptr;
				described["stop"] = stop != nullptr ? DebugJson(m_Context.GetScene()).Stop(*stop) : Json();
				// A quit during the trace stopped playing and detached the debugger with what it recorded.
				VisualScriptRecording recording;
				if (m_Session.IsAttached())
				{
					Result<VisualScriptRecording> taken = VisualScriptDebugger::StopRecording();
					if (!taken)
					{
						return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", taken.GetError());
					}
					recording = taken.TakeValue();
				}
				else if (options.Summary)
				{
					recording.Summary.emplace();
				}
				AddRecording(described, recording);
				return described;
			}

			void AddRecording(Json& described, VisualScriptRecording const& recording) const
			{
				DebugJson const json(m_Context.GetScene());
				if (recording.Summary)
				{
					Json graphs = Json::array();
					for (VisualScriptGraphSummary const& graph : *recording.Summary)
					{
						Json steps = Json::array();
						for (VisualScriptStepSummary const& step : graph.Steps)
						{
							steps.push_back(Json::object({{"node", step.Node},
							                              {"title", step.Title},
							                              {"exit", step.Exit.empty() ? Json() : Json(step.Exit)},
							                              {"count", step.Count},
							                              {"firstFrame", step.FirstFrame},
							                              {"lastFrame", step.LastFrame}}));
						}
						Json values = Json::array();
						for (VisualScriptValueSummary const& value : graph.Values)
						{
							Json entry = Json::object({{"node", value.Node},
							                           {"pin", value.Pin},
							                           {"direction", value.Input ? "In" : "Out"},
							                           {"type", value.Type},
							                           {"count", value.Count},
							                           {"first", json.BareValue(value.First)},
							                           {"last", json.BareValue(value.Last)}});
							if (value.Min && value.Max)
							{
								entry["min"] = json.BareValue(*value.Min);
								entry["max"] = json.BareValue(*value.Max);
							}
							values.push_back(std::move(entry));
						}
						graphs.push_back(Json::object({{"asset", DescribeHandle(graph.Document)},
						                               {"path", GetAssetPath(graph.Document)},
						                               {"function", graph.Function},
						                               {"steps", std::move(steps)},
						                               {"values", std::move(values)}}));
					}
					described["summary"] = Json::object({{"graphs", std::move(graphs)}});
				}
				else
				{
					Json steps = Json::array();
					for (VisualScriptRecordedStep const& step : recording.Steps)
					{
						steps.push_back(Json::object({{"i", step.Index},
						                              {"frame", step.Frame},
						                              {"execution", step.Execution},
						                              {"entity", json.DescribeEntity(step.Entity)},
						                              {"asset", DescribeHandle(step.Document)},
						                              {"path", GetAssetPath(step.Document)},
						                              {"function", step.Function},
						                              {"node", step.Node},
						                              {"title", step.Title},
						                              {"exit", step.Exit.empty() ? Json() : Json(step.Exit)}}));
					}
					Json values = Json::array();
					for (VisualScriptRecordedValue const& value : recording.Values)
					{
						Json entry = json.Value(value.Value, false);
						entry["step"] = value.Step;
						entry["node"] = value.Node;
						entry["pin"] = value.Pin;
						entry["direction"] = value.Input ? "In" : "Out";
						values.push_back(std::move(entry));
					}
					described["steps"] = std::move(steps);
					described["values"] = std::move(values);
				}
				bool const truncated = recording.TruncatedSteps > 0 || recording.TruncatedValues > 0;
				described["truncated"] = Json::object({{"steps", recording.TruncatedSteps},
				                                       {"values", recording.TruncatedValues},
				                                       {"hint", truncated ? Json(std::string(NarrowHint)) : Json()}});
			}

			// --- graph.values ---

			Result<void> RegisterValues()
			{
				return Add(
					m_Registry, "graph.values",
					"Reads the values of a visual script while the game plays: of an instance's event graph or a watched "
					"function's last call (entity: default the graph's target, else its only instance), or of a stop's frame "
					"(stop and frame, numbered as graph.debug's frames, 0 innermost) with formulas evaluated against it (evaluate). "
					"nodes: the outputs of these nodes (default: with a stop, the frame's node with its inputs and the nodes its "
					"execution computed; else none). Returns { asset, path, entity, name, function, source (Stop, Frame, LastCall, "
					"None), nodes: [{ node, title, inputs?, outputs }], variables, parameters, locals, loops, event, evaluated: [{ "
					"formula, value or error }], timelines: [{ node, time, length, playing, reverse }] (live), truncated }. A stop's "
					"values carry currency (Current: this execution computed it; Earlier; NotComputed; Unknown), changed (changed "
					"elsewhere while stopped) and, for a literal a live edit changed, edited and before.",
					SchemaBuilder::Object()
						.Property("graph", GraphSchema("Graph handle or \"asset://<path>.sgraph\""), true)
						.Property("entity", EntitySchema("The instance (an entity that runs the graph)"))
						.Property("stop", StopSchema("Read this stop's snapshot instead of the live instance"))
						.Property("frame", SchemaBuilder::Integer("The stop's frame (0: innermost)").Minimum(0).Maximum(MaxNodeID))
						.Property("function", SchemaBuilder::Integer("The function's ID; 0: the event graph").Minimum(0).Maximum(MaxNodeID))
						.Property("nodes", SchemaBuilder::Array(NodeSchema("Node ID"), "Nodes whose outputs to read").MaxItems(4096))
						.Property("variables", SchemaBuilder::Boolean("Read the variables").Default(true))
						.Property("timelines", SchemaBuilder::Boolean("Read the timelines").Default(true))
						.Property(
							"evaluate",
							SchemaBuilder::Array(
								SchemaBuilder::String("A formula").MinLength(1).MaxLength(VisualScriptBreakpointStore::MaxConditionBytes),
								"Formulas evaluated against the stop's frame")
								.MaxItems(MaxEvaluated))
						.Build(),
					true,
					[self = shared_from_this()](Json const& params) -> CommandResult
					{
						return self->Values(params);
					});
			}

			CommandResult Values(Json const& params)
			{
				CommandValue<AssetHandle> graph = ParseGraph(params.at("graph"), "graph");
				if (!graph)
				{
					return graph.TakeError();
				}
				auto const stopParameter = params.find("stop");
				if (stopParameter == params.end())
				{
					for (char const* const member : {"evaluate", "frame"})
					{
						if (params.contains(member))
						{
							return MakeParameterError(member, "a stop's frame is read with stop (graph.debug lists the stops)");
						}
					}
				}
				CommandValue<Ref<VisualScriptDocument const>> document = LoadStoredDocument(graph.GetValue());
				if (!document)
				{
					return document.TakeError();
				}
				if (std::optional<CommandError> refused = RequirePlaying(stopParameter != params.end()))
				{
					return std::move(*refused);
				}
				m_Session.Refresh();
				std::vector<uint32_t> nodes;
				if (auto const listed = params.find("nodes"); listed != params.end())
				{
					for (Json const& node : *listed)
					{
						nodes.push_back(node.get<uint32_t>());
					}
				}
				bool const variables = params.value("variables", true);
				bool const timelines = params.value("timelines", true);
				if (stopParameter != params.end())
				{
					return StopValues(params, graph.GetValue(), *stopParameter, nodes, variables, timelines);
				}
				return LiveValues(params, graph.GetValue(), *document.GetValue(), nodes, variables, timelines);
			}

			// The values of an instance as it is now.
			CommandResult LiveValues(Json const& params, AssetHandle graph, VisualScriptDocument const& document,
			                         std::vector<uint32_t> const& nodes, bool variables, bool timelines)
			{
				if (document.Kind == VisualScriptDocumentKind::Library)
				{
					return MakeParameterError("graph", "a function library runs on no entity: read a stop's frame (stop)");
				}
				UUID entity = UUID::Invalid();
				if (auto const parameter = params.find("entity"); parameter != params.end())
				{
					CommandValue<UUID> runner = ParseRunner(*parameter, "entity", graph);
					if (!runner)
					{
						return runner.TakeError();
					}
					entity = runner.GetValue();
				}
				else if (UUID const target = m_Session.GetTarget(graph); target.IsValid())
				{
					// A target that is gone stands for no other instance, as it stops none.
					if (!RunsGraph(target, graph))
					{
						return MakeParameterError("entity", fmt::format("the graph's target {} does not run it now: pass entity, or set "
						                                                "another target (graph.target)",
						                                                target));
					}
					entity = target;
				}
				else
				{
					std::vector<std::pair<UUID, std::string>> const instances = FindInstances(graph);
					if (instances.size() != 1)
					{
						return MakeParameterError("entity", instances.empty()
						                                        ? fmt::format("no entity runs {}", NameGraph(graph))
						                                        : fmt::format("{} entities run {}: pass entity, or set the graph's target "
						                                                      "(graph.target)",
						                                                      instances.size(), NameGraph(graph)));
					}
					entity = instances.front().first;
				}
				uint32_t const function = params.value("function", 0u);
				VisualScriptGraph const* subGraph = FindSubGraph(document, function);
				if (subGraph == nullptr)
				{
					return MakeParameterError("function", fmt::format("{} has no function {}", NameGraph(graph), function));
				}
				for (size_t index = 0; index < nodes.size(); index++)
				{
					if (FindNode(*subGraph, nodes[index]) == nullptr)
					{
						return MakeParameterError(fmt::format("nodes[{}]", index),
						                          fmt::format("the sub-graph has no node {}", nodes[index]));
					}
				}
				VisualScriptValueRequest request;
				request.Entity = entity;
				request.Function = function;
				request.Nodes = nodes;
				request.Variables = variables;
				request.Timelines = timelines;
				Result<VisualScriptLiveValues> read = VisualScriptDebugger::ReadValues(request);
				if (!read)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "entity {} runs no instance of {}: {}", entity,
					                        NameGraph(graph), read.GetError());
				}
				VisualScriptLiveValues const& values = read.GetValue();
				DebugJson const json(m_Context.GetScene());
				Ref<VisualScriptAnalysis const> const analysis = AnalyzeStored(document, graph);
				Json described = DescribeValuesHeader(graph, values.Entity, function, GetSourceName(values.Source));
				Json nodeValues = Json::array();
				for (VisualScriptNodeValues const& node : values.Nodes)
				{
					VisualScriptNode const* stored = FindNode(*subGraph, node.Node);
					nodeValues.push_back(Json::object({{"node", node.Node},
					                                   {"title", stored != nullptr ? GetNodeTitle(analysis.get(), *stored) : std::string()},
					                                   {"outputs", json.Values(node.Outputs, false)}}));
				}
				described["nodes"] = std::move(nodeValues);
				described["variables"] = json.Values(values.Variables, false);
				described["parameters"] = Json::array();
				described["locals"] = Json::array();
				described["loops"] = Json::array();
				described["event"] = Json::array();
				described["evaluated"] = Json::array();
				described["timelines"] = DescribeTimelines(values.Timelines);
				described["truncated"] = values.Truncated;
				return described;
			}

			// The values of a stop's frame, as the snapshot holds them.
			CommandResult StopValues(Json const& params, AssetHandle graph, Json const& stopParameter, std::vector<uint32_t> const& nodes,
			                         bool variables, bool timelines)
			{
				CommandValue<VisualScriptStop const*> found = FindStop(stopParameter, "stop");
				if (!found)
				{
					return found.TakeError();
				}
				VisualScriptStop const& stop = *found.GetValue();
				uint32_t const frameIndex = params.value("frame", 0u);
				if (frameIndex >= stop.Frames.size())
				{
					return MakeParameterError("frame", fmt::format("stop {} has {} frames", stop.ID, stop.Frames.size()));
				}
				VisualScriptStopFrame const& frame = stop.Frames[frameIndex];
				if (frame.Kind == VisualScriptStopFrameKind::Native)
				{
					return MakeParameterError("frame",
					                          fmt::format("frame {} is engine code ({}): it has no values", frameIndex, frame.Title));
				}
				if (frame.Graph != graph && frame.Document != graph)
				{
					return MakeParameterError("graph", fmt::format("frame {} of stop {} is of {}, not of {}", frameIndex, stop.ID,
					                                               NameGraph(frame.Document), NameGraph(graph)));
				}
				if (auto const entity = params.find("entity"); entity != params.end())
				{
					CommandValue<UUID> id = ParseEntity(*entity, "entity");
					if (!id)
					{
						return id.TakeError();
					}
					if (id.GetValue() != frame.Entity)
					{
						return MakeParameterError("entity",
						                          fmt::format("frame {} of stop {} runs on entity {}", frameIndex, stop.ID, frame.Entity));
					}
				}
				if (auto const function = params.find("function"); function != params.end() && function->get<uint32_t>() != frame.Function)
				{
					return MakeParameterError("function",
					                          fmt::format("frame {} of stop {} is of function {}", frameIndex, stop.ID, frame.Function));
				}
				CommandValue<Ref<VisualScriptDocument const>> document = LoadStoredDocument(frame.Document);
				if (!document)
				{
					return document.TakeError();
				}
				VisualScriptGraph const* subGraph = FindSubGraph(*document.GetValue(), frame.Function);
				for (size_t index = 0; index < nodes.size(); index++)
				{
					if (subGraph == nullptr || FindNode(*subGraph, nodes[index]) == nullptr)
					{
						return MakeParameterError(fmt::format("nodes[{}]", index),
						                          fmt::format("the frame's sub-graph has no node {}", nodes[index]));
					}
				}
				std::vector<std::string> formulas;
				if (auto const evaluate = params.find("evaluate"); evaluate != params.end())
				{
					for (Json const& formula : *evaluate)
					{
						formulas.push_back(formula.get<std::string>());
					}
				}
				Result<VisualScriptStopValues> described = VisualScriptDebugger::DescribeStop(stop.ID, frameIndex, formulas);
				if (!described)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", described.GetError());
				}
				VisualScriptStopValues const& values = described.GetValue();
				DebugJson const json(m_Context.GetScene());
				Ref<VisualScriptAnalysis const> const analysis = AnalyzeStored(*document.GetValue(), frame.Document);
				auto const title = [&analysis, subGraph](uint32_t id)
				{
					VisualScriptNode const* stored = subGraph != nullptr ? FindNode(*subGraph, id) : nullptr;
					return stored != nullptr ? GetNodeTitle(analysis.get(), *stored) : std::string();
				};
				auto const describeNode = [&json, &title](VisualScriptNodeValues const& node, bool withInputs)
				{
					Json entry = Json::object({{"node", node.Node}, {"title", title(node.Node)}});
					if (withInputs)
					{
						entry["inputs"] = json.Values(node.Inputs, true);
					}
					entry["outputs"] = json.Values(node.Outputs, true);
					return entry;
				};

				Json nodeValues = Json::array();
				if (nodes.empty() && !params.contains("nodes"))
				{
					if (values.Node.Node != 0)
					{
						nodeValues.push_back(describeNode(values.Node, true));
					}
					for (VisualScriptNodeValues const& node : values.Computed)
					{
						nodeValues.push_back(describeNode(node, false));
					}
				}
				for (uint32_t const id : nodes)
				{
					if (id == values.Node.Node)
					{
						nodeValues.push_back(describeNode(values.Node, true));
						continue;
					}
					auto const computed = std::find_if(values.Computed.begin(), values.Computed.end(),
					                                   [id](VisualScriptNodeValues const& node)
					                                   {
														   return node.Node == id;
													   });
					if (computed != values.Computed.end())
					{
						nodeValues.push_back(describeNode(*computed, false));
						continue;
					}
					// A node this execution did not compute shows what the frame's node reads of it: the values its links bring.
					VisualScriptNodeValues linked;
					linked.Node = id;
					if (subGraph != nullptr)
					{
						for (VisualScriptLink const& link : subGraph->Links)
						{
							if (link.From.Node != id || link.To.Node != values.Node.Node)
							{
								continue;
							}
							auto const input = std::find_if(values.Node.Inputs.begin(), values.Node.Inputs.end(),
							                                [&link](VisualScriptDebugValue const& candidate)
							                                {
																return candidate.Name == link.To.Pin;
															});
							bool const listed = std::any_of(linked.Outputs.begin(), linked.Outputs.end(),
							                                [&link](VisualScriptDebugValue const& output)
							                                {
																return output.Name == link.From.Pin;
															});
							if (input != values.Node.Inputs.end() && !listed)
							{
								VisualScriptDebugValue output = *input;
								output.Name = link.From.Pin;
								output.Edited = false;
								output.Before = Json();
								linked.Outputs.push_back(std::move(output));
							}
						}
					}
					nodeValues.push_back(describeNode(linked, false));
				}

				Json evaluated = Json::array();
				for (VisualScriptEvaluated const& result : values.Evaluated)
				{
					Json entry = Json::object({{"formula", result.Formula}});
					if (result.Value)
					{
						entry["value"] = json.Value(*result.Value, false);
					}
					else
					{
						entry["error"] = result.Error;
					}
					evaluated.push_back(std::move(entry));
				}
				Json loops = Json::array();
				for (VisualScriptLoopState const& loop : values.Loops)
				{
					Json entry = Json::object({{"node", loop.Node}, {"kind", loop.Kind}});
					if (loop.Kind == "ForLoop")
					{
						entry["index"] = loop.Index;
						entry["first"] = loop.First;
						entry["last"] = loop.Last;
					}
					else if (loop.Kind == "ForEach")
					{
						entry["index"] = loop.Index;
						entry["length"] = loop.Length;
						entry["element"] = loop.Element ? json.Value(*loop.Element, true) : Json();
					}
					else if (loop.Kind == "Sequence")
					{
						entry["next"] = loop.Next;
					}
					loops.push_back(std::move(entry));
				}

				Json result = DescribeValuesHeader(graph, frame.Entity, frame.Function, "Stop");
				result["stop"] = stop.ID;
				result["frame"] = frameIndex;
				result["nodes"] = std::move(nodeValues);
				result["variables"] = variables ? json.Values(values.Variables, true) : Json::array();
				result["parameters"] = json.Values(values.Parameters, true);
				result["locals"] = json.Values(values.Locals, true);
				result["loops"] = std::move(loops);
				result["event"] = json.Values(values.Event, true);
				result["evaluated"] = std::move(evaluated);
				Json timelineValues = Json::array();
				if (timelines && frame.Entity.IsValid() && m_Context.GetScene().HasEntity(frame.Entity))
				{
					VisualScriptValueRequest request;
					request.Entity = frame.Entity;
					request.Timelines = true;
					if (Result<VisualScriptLiveValues> live = VisualScriptDebugger::ReadValues(request); live)
					{
						timelineValues = DescribeTimelines(live.GetValue().Timelines);
					}
				}
				result["timelines"] = std::move(timelineValues);
				result["truncated"] = values.Truncated;
				return result;
			}

			Json DescribeValuesHeader(AssetHandle graph, UUID entity, uint32_t function, std::string_view source) const
			{
				DebugJson const json(m_Context.GetScene());
				return Json::object({{"asset", graph.ToString()},
				                     {"path", GetAssetPath(graph)},
				                     {"entity", json.DescribeEntity(entity)},
				                     {"name", json.NameOf(entity)},
				                     {"function", function},
				                     {"source", source}});
			}

			static Json DescribeTimelines(std::span<VisualScriptTimelineState const> timelines)
			{
				Json described = Json::array();
				for (VisualScriptTimelineState const& timeline : timelines)
				{
					described.push_back(Json::object({{"node", timeline.Node},
					                                  {"time", timeline.Time},
					                                  {"length", timeline.Length},
					                                  {"playing", timeline.Playing},
					                                  {"reverse", timeline.Reverse}}));
				}
				return described;
			}

			// --- graph.instances ---

			Result<void> RegisterInstances()
			{
				return Add(m_Registry, "graph.instances",
				           "Lists the instances of visual scripts while the game plays, in the order they were created: of one graph, "
				           "or of one entity (both optional). Returns { instances: [{ entity, name, asset, path, state (Idle, Waiting, "
				           "Stopped), waits: [{ asset, path, function, node, title, kind, remaining?, frames?, index? }] (the latent "
				           "nodes its executions wait in), stops: [stop IDs] }], total }.",
				           SchemaBuilder::Object()
				               .Property("graph", GraphSchema("Only instances of this graph"))
				               .Property("entity", EntitySchema("Only this entity's instance"))
				               .Property("limit", SchemaBuilder::Integer("Instances listed at most")
				                                      .Minimum(1)
				                                      .Maximum(MaxInstancesListed)
				                                      .Default(DefaultInstancesListed))
				               .Property("offset", SchemaBuilder::Integer("Instances skipped").Minimum(0).Default(0))
				               .Build(),
				           true,
				           [self = shared_from_this()](Json const& params) -> CommandResult
				           {
							   return self->Instances(params);
						   });
			}

			CommandResult Instances(Json const& params)
			{
				VisualScriptInstanceQuery query;
				if (auto const graph = params.find("graph"); graph != params.end())
				{
					CommandValue<AssetHandle> parsed = ParseGraph(*graph, "graph");
					if (!parsed)
					{
						return parsed.TakeError();
					}
					query.Graph = parsed.GetValue();
				}
				if (std::optional<CommandError> refused = RequirePlaying(false))
				{
					return std::move(*refused);
				}
				if (auto const entity = params.find("entity"); entity != params.end())
				{
					CommandValue<UUID> id = ParseEntity(*entity, "entity");
					if (!id)
					{
						return id.TakeError();
					}
					if (!m_Context.GetScene().HasEntity(id.GetValue()))
					{
						return MakeCommandError(AutomationErrorCode::EntityNotFound, "entity {} does not exist", id.GetValue());
					}
					query.Entity = id.GetValue();
				}
				m_Session.Refresh();
				query.Limit = params.value("limit", DefaultInstancesListed);
				uint64_t const offset = params.value("offset", uint64_t(0));
				query.Offset = static_cast<uint32_t>(std::min<uint64_t>(offset, std::numeric_limits<uint32_t>::max()));
				Result<VisualScriptInstanceList> listed = VisualScriptDebugger::DescribeInstances(query);
				if (!listed)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", listed.GetError());
				}
				DebugJson const json(m_Context.GetScene());
				Json instances = Json::array();
				for (VisualScriptInstanceInfo const& instance : listed.GetValue().Instances)
				{
					Json waits = Json::array();
					for (VisualScriptInstanceWait const& wait : instance.Waits)
					{
						Json entry = Json::object({{"asset", DescribeHandle(wait.Document)},
						                           {"path", GetAssetPath(wait.Document)},
						                           {"function", wait.Function},
						                           {"node", wait.Node},
						                           {"title", wait.Title},
						                           {"kind", wait.Kind}});
						if (wait.Remaining)
						{
							entry["remaining"] = *wait.Remaining;
						}
						if (wait.Frames)
						{
							entry["frames"] = *wait.Frames;
						}
						if (wait.Index)
						{
							entry["index"] = *wait.Index;
						}
						waits.push_back(std::move(entry));
					}
					instances.push_back(Json::object({{"entity", json.DescribeEntity(instance.Entity)},
					                                  {"name", json.NameOf(instance.Entity)},
					                                  {"asset", DescribeHandle(instance.Graph)},
					                                  {"path", GetAssetPath(instance.Graph)},
					                                  {"state", GetInstanceStateName(instance.State)},
					                                  {"waits", std::move(waits)},
					                                  {"stops", instance.Stops}}));
				}
				return Json::object({{"instances", std::move(instances)}, {"total", listed.GetValue().Total}});
			}

			CommandRegistry& m_Registry;
			VisualScriptDebugSession& m_Session;
			PlayMode& m_PlayMode;
			EditorContext& m_Context;
		};
	}

	Json DescribeVisualScriptStop(VisualScriptStop const& stop, Scene& scene)
	{
		return DebugJson(scene).Stop(stop);
	}

	Result<void> RegisterGraphDebugCommands(CommandRegistry& registry, VisualScriptDebugSession& session, PlayMode& playMode,
	                                        EditorContext& context)
	{
		// The handlers share the commands object: it lives as long as the registry keeps them.
		Ref<GraphDebugCommands> const commands = CreateRef<GraphDebugCommands>(registry, session, playMode, context);
		return commands->Register();
	}
}
