#include "Editor/Automation/PlayCommands.h"

#include "Editor/Automation/JsonSchema.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Scene/SceneSerializer.h"

#include <cmath>
#include <string>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr double DefaultTimestep = 1.0 / 60.0;

		char const* GetStateName(EditorPlayState state)
		{
			switch (state)
			{
				case EditorPlayState::Play:
					return "play";
				case EditorPlayState::Simulate:
					return "simulate";
				case EditorPlayState::Edit:
					break;
			}
			return "edit";
		}

		Json DescribeTestReport(TestRunReport const& report)
		{
			Json results = Json::array();
			for (ScriptTestResult const& result : report.Results)
			{
				results.push_back(Json::object({{"name", result.Name}, {"passed", result.Passed}, {"message", result.Message}}));
			}
			return Json::object({{"finished", report.Finished},
			                     {"passed", report.Results.size() - report.GetFailedCount()},
			                     {"failed", report.GetFailedCount()},
			                     {"exceptions", report.ScriptExceptions},
			                     {"failures", report.GetFailureCount()},
			                     {"results", std::move(results)}});
		}

		Json DescribePlay(PlayMode const& playMode, EditorContext& context)
		{
			Json description = Json::object({{"state", GetStateName(context.GetPlayState())},
			                                 {"paused", playMode.IsPaused()},
			                                 {"startPending", playMode.IsStartPending()},
			                                 {"scene", context.GetScene().GetName()}});
			if (playMode.IsPlaying())
			{
				description["frame"] = context.GetScene().GetRuntimeFrame();
				description["time"] = context.GetScene().GetRuntimeTime();
			}
			TestRunReport const* const report = playMode.GetTestReport();
			description["tests"] = report != nullptr ? DescribeTestReport(*report) : Json();
			return description;
		}

		Json DescribeInput()
		{
			Json keys = Json::array();
			for (uint32_t key = 1; key < KeyCodeCount; key++)
			{
				if (Input::IsKeyDown(static_cast<KeyCode>(key)))
				{
					keys.push_back(KeyCodeToString(static_cast<KeyCode>(key)));
				}
			}
			Json buttons = Json::array();
			for (uint32_t button = 0; button < MouseButtonCount; button++)
			{
				if (Input::IsMouseButtonDown(static_cast<MouseButton>(button)))
				{
					buttons.push_back(MouseButtonToString(static_cast<MouseButton>(button)));
				}
			}
			glm::vec2 const position = Input::GetMousePosition();
			return Json::object(
				{{"keys", std::move(keys)}, {"mouseButtons", std::move(buttons)}, {"mousePosition", {position.x, position.y}}});
		}

		CommandError NotPlaying()
		{
			return MakeCommandError(AutomationErrorCode::InvalidOperation, "the editor is not playing (play.start)");
		}

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

		SchemaBuilder Vector2Schema(std::string_view description)
		{
			return SchemaBuilder::Array(SchemaBuilder::Number(), description).MinItems(2).MaxItems(2);
		}

		SchemaBuilder TimestepSchema()
		{
			return SchemaBuilder::Number("Seconds per frame").Minimum(0.0001).Maximum(1.0).Default(DefaultTimestep);
		}
	}

	Result<void> RegisterPlayCommands(CommandRegistry& registry, PlayMode& playMode, EditorContext& context,
	                                  ViewportSizeProvider viewportSize)
	{
		Result<void> result = AddAsync(
			registry, "play.start",
			"Plays a copy of the edited scene (mode play: scripts, physics and audio; simulate: physics only); panels and "
			"commands then show and edit the copy, and play.stop discards it. Play builds changed scripts first and fails when "
			"they do not build; asynchronous. Returns the play state.",
			SchemaBuilder::Object().Property("mode", SchemaBuilder::String("What runs").Enum({"play", "simulate"}).Default("play")).Build(),
			[&playMode, &context, viewportSize](Json const& params, CommandCompletion completion)
			{
				if (playMode.IsPlaying() || playMode.IsStartPending())
				{
					completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation, "the editor is already playing"));
					return;
				}
				auto const mode = params.find("mode");
				EditorPlayState const state =
					mode != params.end() && *mode == "simulate" ? EditorPlayState::Simulate : EditorPlayState::Play;
				glm::uvec2 const size = viewportSize();
				playMode.RequestStart(state, size.x, size.y, nullptr,
			                          [completion, &playMode, &context](Result<void> const& started)
			                          {
										  if (!started)
										  {
											  completion.Complete(
												  MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError()));
											  return;
										  }
										  completion.Complete(DescribePlay(playMode, context));
									  });
			});

		result =
			result ? Add(registry, "play.stop",
		                 "Stops playing: the running copy and every change made to it go, and the edited scene comes back.", Json(), false,
		                 [&playMode, &context](Json const&) -> CommandResult
		                 {
							 if (!playMode.IsPlaying())
							 {
								 return NotPlaying();
							 }
							 playMode.Stop();
							 return DescribePlay(playMode, context);
						 })
				   : result;

		result =
			result ? Add(registry, "play.pause", "Pauses or resumes the running scene (and its sounds).",
		                 SchemaBuilder::Object().Property("paused", SchemaBuilder::Boolean("Pause (true) or resume").Default(true)).Build(),
		                 false,
		                 [&playMode, &context](Json const& params) -> CommandResult
		                 {
							 if (!playMode.IsPlaying())
							 {
								 return NotPlaying();
							 }
							 auto const paused = params.find("paused");
							 playMode.SetPaused(paused == params.end() || paused->get<bool>());
							 return DescribePlay(playMode, context);
						 })
				   : result;

		result = result
		             ? Add(registry, "play.step",
		                   "Advances the paused scene by frames, one per editor frame with the editor's frame time (play.advance runs "
		                   "frames at once).",
		                   SchemaBuilder::Object()
		                       .Property("frames", SchemaBuilder::Integer("Frames to advance").Minimum(1).Maximum(100000).Default(1))
		                       .Build(),
		                   false,
		                   [&playMode, &context](Json const& params) -> CommandResult
		                   {
							   if (!playMode.IsPlaying())
							   {
								   return NotPlaying();
							   }
							   if (!playMode.IsPaused())
							   {
								   return MakeCommandError(AutomationErrorCode::InvalidOperation, "the scene is not paused (play.pause)");
							   }
							   auto const frames = params.find("frames");
							   playMode.Step(frames != params.end() ? frames->get<uint32_t>() : 1);
							   return DescribePlay(playMode, context);
						   })
		             : result;

		result = result ? Add(registry, "play.advance",
		                      "Runs frames at once with a fixed time step, paused or not; input set before applies to the first frame. "
		                      "Pause first for deterministic runs: the editor keeps playing in real time otherwise. Stops early when "
		                      "the scripts quit (Application.Quit). Returns framesRun and the play state.",
		                      SchemaBuilder::Object()
		                          .Property("frames", SchemaBuilder::Integer("Frames to run").Minimum(1).Maximum(100000), true)
		                          .Property("timestep", TimestepSchema())
		                          .Build(),
		                      false,
		                      [&playMode, &context](Json const& params) -> CommandResult
		                      {
								  if (!playMode.IsPlaying())
								  {
									  return NotPlaying();
								  }
								  auto const timestep = params.find("timestep");
								  float const seconds =
									  static_cast<float>(timestep != params.end() ? timestep->get<double>() : DefaultTimestep);
								  uint32_t const run = playMode.Advance(params.at("frames").get<uint32_t>(), Timestep(seconds));
								  Json description = DescribePlay(playMode, context);
								  description["framesRun"] = run;
								  return description;
							  })
		                : result;

		result = result ? Add(registry, "play.state",
		                      "The play state: { state (edit, play or simulate), paused, startPending, scene, frame and time (while "
		                      "playing), tests (the run's Strada.Testing report: finished, passed, failed, exceptions, failures, "
		                      "results; null before the first run) }.",
		                      Json(), true,
		                      [&playMode, &context](Json const&) -> CommandResult
		                      {
								  return DescribePlay(playMode, context);
							  })
		                : result;

		result =
			result
				? Add(registry, "input.set",
		              "Sets the game's input like devices would: keys and mouse buttons down (true) or up (false) by name "
		              "(KeyCode and MouseButton names), the mouse position in pixels of the game view and scroll for this frame. "
		              "Pressed and released last one frame. Returns the keys and buttons down and the mouse position.",
		              SchemaBuilder::Object()
		                  .Property("keys", SchemaBuilder::Object("Key name -> down").AdditionalProperties(SchemaBuilder::Boolean()))
		                  .Property("mouseButtons",
		                            SchemaBuilder::Object("Button name -> down").AdditionalProperties(SchemaBuilder::Boolean()))
		                  .Property("mousePosition", Vector2Schema("Position in pixels from the game view's top-left corner"))
		                  .Property("scroll", Vector2Schema("Scroll this frame (y is the usual wheel)"))
		                  .Build(),
		              false,
		              [](Json const& params) -> CommandResult
		              {
						  // Every name is checked before anything changes.
						  std::vector<std::pair<KeyCode, bool>> keys;
						  if (auto const entries = params.find("keys"); entries != params.end())
						  {
							  for (auto const& [name, down] : entries->items())
							  {
								  std::optional<KeyCode> const key = KeyCodeFromString(name);
								  if (!key)
								  {
									  return MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a key name", name);
								  }
								  keys.emplace_back(*key, down.get<bool>());
							  }
						  }
						  std::vector<std::pair<MouseButton, bool>> buttons;
						  if (auto const entries = params.find("mouseButtons"); entries != params.end())
						  {
							  for (auto const& [name, down] : entries->items())
							  {
								  std::optional<MouseButton> const button = MouseButtonFromString(name);
								  if (!button)
								  {
									  return MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a mouse button name", name);
								  }
								  buttons.emplace_back(*button, down.get<bool>());
							  }
						  }
						  for (auto const& [key, down] : keys)
						  {
							  Input::SetKeyState(key, down);
						  }
						  for (auto const& [button, down] : buttons)
						  {
							  Input::SetMouseButtonState(button, down);
						  }
						  if (auto const position = params.find("mousePosition"); position != params.end())
						  {
							  Input::SetMousePosition({(*position)[0].get<float>(), (*position)[1].get<float>()});
						  }
						  if (auto const scroll = params.find("scroll"); scroll != params.end())
						  {
							  Input::AddScrollDelta({(*scroll)[0].get<float>(), (*scroll)[1].get<float>()});
						  }
						  return DescribeInput();
					  })
				: result;

		result = result ? Add(registry, "input.release", "Releases every key and mouse button (they report released for one frame).",
		                      Json(), false,
		                      [](Json const&) -> CommandResult
		                      {
								  Input::ReleaseAll();
								  return DescribeInput();
							  })
		                : result;
		if (!result)
		{
			return result;
		}

		return AddAsync(
			registry, "test.run",
			"Plays a scene until its scripts finish testing (Strada.Testing.TestReporter.Finish), they quit, or a timeout in "
			"game seconds passes, with a fixed time step, then stops. The scene is a scene file of the asset directory, or a "
			"copy of the edited scene. Scripts build first when they changed; asynchronous. Returns { finished, timedOut, "
			"frames, passed, failed, exceptions, failures, results: [{ name, passed, message }] }.",
			SchemaBuilder::Object()
				.Property("scene", SchemaBuilder::String("Scene file relative to the asset directory").MinLength(1))
				.Property("timeout",
		                  SchemaBuilder::Number("Game seconds before the run gives up").Minimum(0.01).Maximum(3600.0).Default(60.0))
				.Property("timestep", TimestepSchema())
				.Build(),
			[&playMode, viewportSize](Json const& params, CommandCompletion completion)
			{
				if (playMode.IsPlaying() || playMode.IsStartPending())
				{
					completion.Complete(
						MakeCommandError(AutomationErrorCode::InvalidOperation, "the editor is playing: stop playing first (play.stop)"));
					return;
				}
				Ref<Scene> scene;
				if (auto const path = params.find("scene"); path != params.end())
				{
					AssetHandle const asset =
						AssetManager::IsInitialized() ? AssetManager::FindByPath(path->get<std::string>()) : AssetHandle();
					if (!asset.IsValid() || AssetManager::GetAssetType(asset) != AssetType::Scene)
					{
						completion.Complete(
							MakeCommandError(AutomationErrorCode::AssetNotFound, "there is no scene '{}'", path->get<std::string>()));
						return;
					}
					Result<Ref<Scene>> loaded = SceneSerializer::LoadFromFile(
						AssetManager::GetAbsolutePath(asset), AssetManager::CreateDeserializationContext(UnknownFieldPolicy::Warn));
					if (!loaded)
					{
						completion.Complete(MakeCommandError(AutomationErrorCode::FileError, "{}", loaded.GetError()));
						return;
					}
					scene = loaded.TakeValue();
				}
				auto const timeout = params.find("timeout");
				auto const timestep = params.find("timestep");
				double const seconds = timestep != params.end() ? timestep->get<double>() : DefaultTimestep;
				uint32_t const maxFrames =
					static_cast<uint32_t>(std::ceil((timeout != params.end() ? timeout->get<double>() : 60.0) / seconds));
				glm::uvec2 const size = viewportSize();
				playMode.RequestStart(EditorPlayState::Play, size.x, size.y, std::move(scene),
			                          [completion, &playMode, maxFrames, seconds](Result<void> const& started)
			                          {
										  if (!started)
										  {
											  completion.Complete(
												  MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError()));
											  return;
										  }
										  uint32_t frames = 0;
										  while (playMode.IsPlaying() && frames < maxFrames && !playMode.GetTestReport()->Finished)
										  {
											  frames += playMode.Advance(1, Timestep(static_cast<float>(seconds)));
										  }
										  playMode.Stop();
										  TestRunReport const report = *playMode.GetTestReport();
										  Json description = DescribeTestReport(report);
										  description["timedOut"] = !report.Finished && frames >= maxFrames;
										  description["frames"] = frames;
										  completion.Complete(std::move(description));
									  });
			});
	}
}
