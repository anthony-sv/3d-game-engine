#include "Editor/Automation/PlayCommands.h"

#include "Editor/Automation/GraphDebugCommands.h"
#include "Editor/Automation/JsonSchema.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/SceneRunner.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/UILayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr double DefaultTimestep = 1.0 / 60.0;

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

		// The debugger's part is brought up to date first: an earlier command in the same batch may have ended a stop.
		Json DescribePlay(PlayMode& playMode, EditorContext& context)
		{
			playMode.RefreshObserver();
			Json description = Json::object({{"state", GetEditorPlayStateName(context.GetPlayState())},
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
			PlayDebugSummary const debug = playMode.GetDebugSummary();
			description["debug"] = Json::object({{"state", debug.State},
			                                     {"stops", debug.Stops},
			                                     {"current", debug.Current ? Json(*debug.Current) : Json()},
			                                     {"pauseHeld", debug.PauseHeld}});
			return description;
		}

		// The input as input.set and input.release answer it: keys and mouse buttons down, the mouse position, the action
		// overrides held (by scripts and graphs too) as [x, y] (Button and Axis actions read x), and each injected gamepad slot
		// with its buttons down and every axis.
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
			Json actions = Json::object();
			for (auto const& [name, value] : Input::GetActionOverrides())
			{
				actions[name] = Json::array({value.x, value.y});
			}
			Json gamepads = Json::object();
			for (uint32_t slot = 0; slot < MaxGamepads; slot++)
			{
				std::optional<Input::GamepadState> const injected = Input::GetInjectedGamepadState(slot);
				if (!injected)
				{
					continue;
				}
				Json padButtons = Json::array();
				for (uint32_t button = 0; button < GamepadButtonCount; button++)
				{
					if (injected->Buttons[button])
					{
						padButtons.push_back(GamepadButtonToString(static_cast<GamepadButton>(button)));
					}
				}
				Json axes = Json::object();
				for (uint32_t axis = 0; axis < GamepadAxisCount; axis++)
				{
					axes[GamepadAxisToString(static_cast<GamepadAxis>(axis))] = injected->Axes[axis];
				}
				gamepads[std::to_string(slot)] =
					Json::object({{"connected", injected->Connected}, {"buttons", std::move(padButtons)}, {"axes", std::move(axes)}});
			}
			glm::vec2 const position = Input::GetMousePosition();
			return Json::object({{"keys", std::move(keys)},
			                     {"mouseButtons", std::move(buttons)},
			                     {"mousePosition", {position.x, position.y}},
			                     {"actions", std::move(actions)},
			                     {"gamepads", std::move(gamepads)}});
		}

		// An entry of input.set's actions, checked: the override to put on the action, or none to clear it.
		struct ActionRequest
		{
			std::string Name;
			std::optional<glm::vec2> Value;
		};

		// An entry of input.set's gamepads, checked: the slot's whole injected state after the request, or none to release it.
		struct GamepadRequest
		{
			uint32_t Slot = 0;
			std::optional<Input::GamepadState> State;
		};

		// The open project's actions (the asset ProjectSettings::InputActions names), by name with its letter case: what
		// input.set's actions are checked against. An error that says why there are none.
		Result<std::vector<InputAction>> ReadProjectInputActions(EditorContext const& context)
		{
			Project const* const project = context.GetProject();
			if (project == nullptr)
			{
				return MakeError("no project is open, so there are no input actions");
			}
			AssetHandle const handle = project->GetSettings().InputActions;
			if (!handle.IsValid())
			{
				return MakeError("the project names no input actions (project.settings' InputActions)");
			}
			AssetType const type = TypedDataAsset<InputActionMap>::GetStaticType();
			Ref<TypedDataAsset<InputActionMap>> const asset =
				AssetManager::IsInitialized() && type != AssetType::None && AssetManager::GetAssetType(handle) == type
					? AssetManager::GetAsset<TypedDataAsset<InputActionMap>>(handle)
					: nullptr;
			if (!asset)
			{
				return MakeError("the project's InputActions, {}, does not load as an InputActions asset",
				                 AssetManager::IsInitialized() ? AssetManager::GetReference(handle) : handle.ToString());
			}
			return asset->GetData().Actions;
		}

		// Whether the number is finite and stays finite as a float (an override is a float).
		bool IsFiniteFloat(double value)
		{
			return std::isfinite(value) && std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
		}

		std::string ListActionNames(std::vector<InputAction> const& actions)
		{
			constexpr size_t Listed = 12;
			std::string names;
			for (size_t index = 0; index < actions.size() && index < Listed; index++)
			{
				names += (index == 0 ? "" : ", ") + actions[index].Name;
			}
			if (actions.size() > Listed)
			{
				names += fmt::format(" and {} more", actions.size() - Listed);
			}
			return names.empty() ? std::string("none") : names;
		}

		// input.set's actions, every entry checked before anything changes: a name of the project's actions, and a value of
		// the shape its type takes (a number or a bool for Button and Axis, [x, y] for Vector2), finite as a float, or null.
		std::optional<CommandError> ReadActionRequests(Json const& entries, EditorContext const& context,
		                                               std::vector<ActionRequest>& requests)
		{
			if (entries.empty())
			{
				return std::nullopt;
			}
			Result<std::vector<InputAction>> const actions = ReadProjectInputActions(context);
			if (!actions)
			{
				return MakeCommandError(AutomationErrorCode::InvalidOperation, "input actions cannot be set: {}", actions.GetError());
			}
			size_t newNames = 0;
			for (auto const& [name, value] : entries.items())
			{
				auto const action = std::ranges::find(actions.GetValue(), name, &InputAction::Name);
				if (action == actions.GetValue().end())
				{
					return MakeCommandError(AutomationErrorCode::InvalidParams,
					                        "'{}' is not an input action of the project (its actions, with their letter case: {})", name,
					                        ListActionNames(actions.GetValue()));
				}
				ActionRequest request{name, std::nullopt};
				if (!value.is_null())
				{
					bool const vector = action->Type == InputActionType::Vector2;
					std::array<double, 2> components = {0.0, 0.0};
					if (vector && value.is_array() && value.size() == 2 && value[0].is_number() && value[1].is_number())
					{
						components = {value[0].get<double>(), value[1].get<double>()};
					}
					else if (!vector && value.is_number())
					{
						components[0] = value.get<double>();
					}
					else if (!vector && value.is_boolean())
					{
						components[0] = value.get<bool>() ? 1.0 : 0.0;
					}
					else
					{
						return MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is a{} action: give it {}, or null to clear it",
						                        name, vector ? " Vector2" : (action->Type == InputActionType::Axis ? "n Axis" : " Button"),
						                        vector ? "[x, y]" : "a number or a bool");
					}
					if (!IsFiniteFloat(components[0]) || !IsFiniteFloat(components[1]))
					{
						return MakeCommandError(AutomationErrorCode::InvalidParams, "the value of '{}' is not a finite number", name);
					}
					request.Value = glm::vec2(static_cast<float>(components[0]), static_cast<float>(components[1]));
					newNames += Input::FindActionOverride(name) == nullptr ? 1 : 0;
				}
				requests.push_back(std::move(request));
			}
			size_t records = 0;
			Input::ForEachActionOverride(
				[&records](std::string_view, Input::ActionOverride const&)
				{
					records++;
				});
			if (records + newNames > Input::MaxActionOverrides)
			{
				// A cleared override keeps its record (Input::ActionOverride) while a scene may still read its counts: records
				// not held are dropped when play mode stops and when a project closes or opens.
				return MakeCommandError(AutomationErrorCode::InvalidOperation,
				                        "{} action names have override records already, the most the input keeps; the records of "
				                        "overrides no longer held go when the game stops, and names that have one can still be set",
				                        Input::MaxActionOverrides);
			}
			return std::nullopt;
		}

		// A gamepad slot as input.set names it: "0" to "15", decimal, without a sign, spaces or a leading zero.
		std::optional<uint32_t> ParseGamepadSlot(std::string_view text)
		{
			if (text.empty() || text.size() > 2 || (text.size() == 2 && text[0] == '0') ||
			    !std::ranges::all_of(text,
			                         [](char character)
			                         {
										 return character >= '0' && character <= '9';
									 }))
			{
				return std::nullopt;
			}
			uint32_t const slot = text.size() == 1 ? static_cast<uint32_t>(text[0] - '0')
			                                       : static_cast<uint32_t>(text[0] - '0') * 10 + static_cast<uint32_t>(text[1] - '0');
			return slot < MaxGamepads ? std::optional<uint32_t>(slot) : std::nullopt;
		}

		// input.set's gamepads, every entry checked before anything changes. A slot's first injection starts at rest
		// (Input::GetRestingGamepadState); a later one changes only the buttons and axes it names.
		std::optional<CommandError> ReadGamepadRequests(Json const& entries, std::vector<GamepadRequest>& requests)
		{
			for (auto const& [name, value] : entries.items())
			{
				std::optional<uint32_t> const slot = ParseGamepadSlot(name);
				if (!slot)
				{
					return MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a gamepad slot (\"0\" to \"15\")", name);
				}
				GamepadRequest request{*slot, std::nullopt};
				if (!value.is_null())
				{
					Input::GamepadState state = Input::GetInjectedGamepadState(*slot).value_or(Input::GetRestingGamepadState());
					if (auto const padButtons = value.find("buttons"); padButtons != value.end())
					{
						for (auto const& [buttonName, down] : padButtons->items())
						{
							std::optional<GamepadButton> const button = GamepadButtonFromString(buttonName);
							if (!button)
							{
								return MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a gamepad button name",
								                        buttonName);
							}
							state.Buttons[static_cast<size_t>(*button)] = down.get<bool>();
						}
					}
					if (auto const axes = value.find("axes"); axes != value.end())
					{
						for (auto const& [axisName, position] : axes->items())
						{
							std::optional<GamepadAxis> const axis = GamepadAxisFromString(axisName);
							if (!axis)
							{
								return MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a gamepad axis name", axisName);
							}
							// Not-a-number fails both comparisons: a request built in memory can hold one.
							double const reading = position.get<double>();
							if (!(reading >= -1.0 && reading <= 1.0))
							{
								return MakeCommandError(AutomationErrorCode::InvalidParams, "gamepad {}'s {} must be a number from -1 to 1",
								                        *slot, axisName);
							}
							state.Axes[static_cast<size_t>(*axis)] = static_cast<float>(reading);
						}
					}
					request.State = state;
				}
				requests.push_back(request);
			}
			return std::nullopt;
		}

		CommandError NotPlaying()
		{
			return MakeCommandError(AutomationErrorCode::InvalidOperation, "the editor is not playing (play.start)");
		}

		// Frames refused while an execution is stopped at a breakpoint, with the debugger's reason, read up to date.
		std::optional<CommandError> RefuseFrames(PlayMode& playMode)
		{
			playMode.RefreshObserver();
			if (std::optional<std::string> const refusal = playMode.GetFrameStepRefusal())
			{
				return MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", *refusal);
			}
			return std::nullopt;
		}

		char const* GetButtonStateName(UIButtonState state)
		{
			switch (state)
			{
				case UIButtonState::Normal:
					break;
				case UIButtonState::Hovered:
					return "hovered";
				case UIButtonState::Pressed:
					return "pressed";
				case UIButtonState::Disabled:
					return "disabled";
			}
			return "normal";
		}

		Json ToJson(glm::vec2 const& value)
		{
			return Json::array({value.x, value.y});
		}

		// The scene's UI laid out as the game lays it out to draw it and to find what the mouse is over: at the scene's view
		// size, or the game view's before the scene has one.
		CommandResult DescribeUILayout(EditorContext& context, ViewportSizeProvider const& viewportSize, Json const& params)
		{
			Scene& scene = context.GetScene();
			std::optional<entt::entity> only;
			if (auto const entity = params.find("entity"); entity != params.end())
			{
				std::string const& text = entity->get_ref<std::string const&>();
				std::optional<UUID> const id = UUID::FromString(text);
				if (!id || !id->IsValid())
				{
					CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a valid entity ID", text);
					error.Data = Json::object({{"path", "entity"}});
					return error;
				}
				Entity const found = scene.GetEntityByUUID(*id);
				if (!found)
				{
					return MakeCommandError(AutomationErrorCode::EntityNotFound, "entity {} does not exist", *id);
				}
				only = found.GetHandle();
			}

			glm::uvec2 size(scene.GetViewportWidth(), scene.GetViewportHeight());
			if (size.x == 0 || size.y == 0)
			{
				size = viewportSize();
			}
			Json elements = Json::array();
			for (UIElement const& element : UILayout::LayoutScene(scene, glm::vec2(size)))
			{
				if (only && element.Handle != *only)
				{
					continue;
				}
				Entity const entity(element.Handle, &scene);
				Json description = Json::object({{"id", entity.GetUUID().ToString()},
				                                 {"name", entity.GetName()},
				                                 {"canvas", Entity(element.Canvas, &scene).GetUUID().ToString()},
				                                 {"min", ToJson(element.Rectangle.Min)},
				                                 {"max", ToJson(element.Rectangle.Max)},
				                                 {"center", ToJson(element.Rectangle.GetCenter())},
				                                 {"opacity", element.Opacity},
				                                 {"hitTarget", element.HitTarget}});
				description["button"] = element.Button != entt::null ? Json(Entity(element.Button, &scene).GetUUID().ToString()) : Json();
				if (entity.HasComponent<UIButtonComponent>())
				{
					description["state"] = GetButtonStateName(scene.GetUIButtonState(entity));
				}
				elements.push_back(std::move(description));
			}
			if (only && elements.empty())
			{
				return MakeCommandError(AutomationErrorCode::InvalidParams,
				                        "entity {} is not on the screen: it is outside every canvas, or it or an ancestor is hidden",
				                        Entity(*only, &scene).GetUUID());
			}
			return Json::object(
				{{"width", size.x}, {"height", size.y}, {"pointerOverUI", scene.IsPointerOverUI()}, {"elements", std::move(elements)}});
		}

		// physics.query's kinds of query: Scene's Raycast, RaycastAll, ShapeCast and Overlap.
		enum class PhysicsQueryKind : uint8_t
		{
			Ray = 0,
			RayAll,
			Cast,
			Overlap
		};

		constexpr uint64_t DefaultQueryHits = 100;
		constexpr uint64_t MaxQueryHits = 10000;

		CommandError RefuseQuery(std::string_view path, std::string message)
		{
			CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "{}", message);
			error.Data = Json::object({{"path", path}});
			return error;
		}

		// A vector the schema shaped as numbers; null when one is not finite (no JSON text holds one, a request built in
		// memory can).
		template<glm::length_t N>
		std::optional<glm::vec<N, double>> ReadQueryVector(Json const& value)
		{
			glm::vec<N, double> vector(0.0);
			for (glm::length_t index = 0; index < N; index++)
			{
				vector[index] = value[static_cast<size_t>(index)].get<double>();
				if (!std::isfinite(vector[index]))
				{
					return std::nullopt;
				}
			}
			return vector;
		}

		Json ToJson(glm::vec3 const& value)
		{
			return Json::array({value.x, value.y, value.z});
		}

		Json DescribeQueryEntity(Scene& scene, UUID id)
		{
			Entity const entity = scene.GetEntityByUUID(id);
			return Json::object({{"entity", id.ToString()}, {"name", entity ? Json(entity.GetName()) : Json()}});
		}

		Json DescribeQueryHit(Scene& scene, UUID id, glm::vec3 const& point, glm::vec3 const& normal, float distance, AssetHandle material)
		{
			Json hit = DescribeQueryEntity(scene, id);
			hit["point"] = ToJson(point);
			hit["normal"] = ToJson(normal);
			hit["distance"] = distance;
			hit["material"] = !material.IsValid()             ? Json()
			                  : AssetManager::IsInitialized() ? Json(AssetManager::GetReference(material))
			                                                  : Json(material.ToString());
			return hit;
		}

		// physics.query: the playing scene's physics world as of its last step, with the changes made since applied (as a
		// script's query sees it). Every parameter is checked before the query runs; a parameter the kind has no use for is
		// refused rather than ignored, so a request never means something it does not say.
		CommandResult QueryPhysics(PlayMode& playMode, EditorContext& context, Json const& params)
		{
			playMode.RefreshObserver();
			if (!playMode.IsPlaying())
			{
				return MakeCommandError(AutomationErrorCode::Unavailable,
				                        "nothing is playing: physics.query reads the playing scene's physics world (play.start)");
			}
			Scene& scene = context.GetScene();
			if (scene.GetPhysicsScene() == nullptr)
			{
				return MakeCommandError(AutomationErrorCode::Unavailable,
				                        "the playing scene has no physics world (physics is unavailable)");
			}

			std::string const& kindName = params.at("kind").get_ref<std::string const&>();
			PhysicsQueryKind const kind = kindName == "raycast"      ? PhysicsQueryKind::Ray
			                              : kindName == "raycastAll" ? PhysicsQueryKind::RayAll
			                              : kindName == "shapeCast"  ? PhysicsQueryKind::Cast
			                                                         : PhysicsQueryKind::Overlap;
			bool const isRay = kind == PhysicsQueryKind::Ray || kind == PhysicsQueryKind::RayAll;
			auto const has = [&params](char const* parameter)
			{
				return params.contains(parameter);
			};

			// What the kind uses.
			if (isRay)
			{
				for (char const* const name : {"shape", "radius", "halfExtents", "halfHeight", "rotation", "rotationEuler"})
				{
					if (has(name))
					{
						return RefuseQuery(name, fmt::format("a ray has no shape: {} is for shapeCast and overlap", name));
					}
				}
			}
			else if (!has("shape"))
			{
				return RefuseQuery("shape", fmt::format("{} needs a shape (sphere, box or capsule)", kindName));
			}
			if (kind == PhysicsQueryKind::Overlap)
			{
				for (char const* const name : {"direction", "distance"})
				{
					if (has(name))
					{
						return RefuseQuery(name,
						                   fmt::format("an overlap does not move: {} is for raycast, raycastAll and shapeCast", name));
					}
				}
			}
			else if (!has("direction"))
			{
				return RefuseQuery("direction", fmt::format("{} needs a direction", kindName));
			}
			if ((kind == PhysicsQueryKind::Ray || kind == PhysicsQueryKind::Cast) && has("maxHits"))
			{
				return RefuseQuery("maxHits", fmt::format("{} gives one hit: maxHits is for raycastAll and overlap", kindName));
			}
			bool const triggers = params.value("triggers", false);
			if (kind == PhysicsQueryKind::Ray && triggers)
			{
				return RefuseQuery("triggers", "raycast passes through triggers: raycastAll sees them");
			}
			if (has("rotation") && has("rotationEuler"))
			{
				return RefuseQuery("rotationEuler", "give rotation or rotationEuler, not both");
			}

			PhysicsQueryShape shape;
			if (!isRay)
			{
				std::string const& shapeName = params.at("shape").get_ref<std::string const&>();
				shape.Type = shapeName == "sphere" ? PhysicsQueryShapeType::Sphere
				             : shapeName == "box"  ? PhysicsQueryShapeType::Box
				                                   : PhysicsQueryShapeType::Capsule;
				bool const round = shape.Type != PhysicsQueryShapeType::Box;
				if (has("radius") && !round)
				{
					return RefuseQuery("radius", "a box has no radius: its size is halfExtents");
				}
				if (has("halfExtents") && round)
				{
					return RefuseQuery("halfExtents", fmt::format("a {} has no halfExtents: its size is radius", shapeName));
				}
				if (has("halfHeight") && shape.Type != PhysicsQueryShapeType::Capsule)
				{
					return RefuseQuery("halfHeight", fmt::format("a {} has no halfHeight: only a capsule has one", shapeName));
				}
				for (char const* const name : {"rotation", "rotationEuler"})
				{
					if (has(name) && shape.Type == PhysicsQueryShapeType::Sphere)
					{
						return RefuseQuery(name, fmt::format("a sphere looks the same turned: {} is for a box or a capsule", name));
					}
				}
				if (has("radius"))
				{
					double const radius = params.at("radius").get<double>();
					if (!std::isfinite(radius))
					{
						return RefuseQuery("radius", "radius is not finite");
					}
					shape.Radius = static_cast<float>(radius);
				}
				if (has("halfHeight"))
				{
					double const halfHeight = params.at("halfHeight").get<double>();
					if (!std::isfinite(halfHeight))
					{
						return RefuseQuery("halfHeight", "halfHeight is not finite");
					}
					shape.HalfHeight = static_cast<float>(halfHeight);
				}
				if (has("halfExtents"))
				{
					std::optional<glm::dvec3> const halfExtents = ReadQueryVector<3>(params.at("halfExtents"));
					if (!halfExtents)
					{
						return RefuseQuery("halfExtents", "halfExtents is not finite");
					}
					shape.HalfExtents = glm::vec3(*halfExtents);
				}
			}

			std::optional<glm::dvec3> const origin = ReadQueryVector<3>(params.at("origin"));
			if (!origin)
			{
				return RefuseQuery("origin", "origin is not finite");
			}
			glm::vec3 direction(0.0f);
			if (has("direction"))
			{
				// Normalized in double, so that no finite direction overflows on its way to a unit vector.
				std::optional<glm::dvec3> const value = ReadQueryVector<3>(params.at("direction"));
				double const length = value ? glm::length(*value) : 0.0;
				if (!value || !std::isfinite(length) || length < 1.0e-9)
				{
					return RefuseQuery("direction", "direction must be a finite vector that is not zero");
				}
				direction = glm::vec3(*value / length);
			}
			float distance = MaxPhysicsQueryDistance;
			if (has("distance"))
			{
				double const value = params.at("distance").get<double>();
				if (!std::isfinite(value))
				{
					return RefuseQuery("distance", "distance is not finite");
				}
				distance = static_cast<float>(value);
				// A query of no length finds nothing, even what a shape starts in: refused so the request means what it says.
				if (!(distance > 0.0f))
				{
					return RefuseQuery("distance", "distance must be more than 0");
				}
			}
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			if (has("rotation"))
			{
				std::optional<glm::dvec4> const value = ReadQueryVector<4>(params.at("rotation"));
				double const length = value ? glm::length(*value) : 0.0;
				if (!value || !std::isfinite(length) || length < 1.0e-9)
				{
					return RefuseQuery("rotation", "rotation must be a finite quaternion [x, y, z, w] that is not zero");
				}
				glm::dvec4 const unit = *value / length;
				rotation = glm::quat(static_cast<float>(unit.w), static_cast<float>(unit.x), static_cast<float>(unit.y),
				                     static_cast<float>(unit.z));
			}
			else if (has("rotationEuler"))
			{
				std::optional<glm::dvec3> const degrees = ReadQueryVector<3>(params.at("rotationEuler"));
				if (!degrees)
				{
					return RefuseQuery("rotationEuler", "rotationEuler is not finite");
				}
				rotation = Math::EulerDegreesToQuaternion(glm::vec3(*degrees));
			}
			PhysicsQueryFilter filter;
			filter.LayerMask = params.value("mask", AllPhysicsLayers);
			filter.IncludeTriggers = triggers;
			uint64_t const maxHits = params.value("maxHits", DefaultQueryHits);

			glm::vec3 const position(*origin);
			Json hits = Json::array();
			size_t count = 0;
			switch (kind)
			{
				case PhysicsQueryKind::Ray:
					if (std::optional<RaycastHit> const hit = scene.Raycast(position, direction, distance, filter.LayerMask))
					{
						hits.push_back(DescribeQueryHit(scene, hit->Entity, hit->Point, hit->Normal, hit->Distance, hit->Material));
						count = 1;
					}
					break;
				case PhysicsQueryKind::RayAll:
				{
					std::vector<RaycastHit> found;
					count = scene.RaycastAll(position, direction, distance, found, filter);
					for (size_t index = 0; index < found.size() && index < maxHits; index++)
					{
						RaycastHit const& hit = found[index];
						hits.push_back(DescribeQueryHit(scene, hit.Entity, hit.Point, hit.Normal, hit.Distance, hit.Material));
					}
					break;
				}
				case PhysicsQueryKind::Cast:
					if (std::optional<ShapeCastHit> const hit = scene.ShapeCast(shape, position, rotation, direction, distance, filter))
					{
						hits.push_back(DescribeQueryHit(scene, hit->Entity, hit->Point, hit->Normal, hit->Distance, hit->Material));
						count = 1;
					}
					break;
				case PhysicsQueryKind::Overlap:
				{
					std::vector<UUID> found;
					count = scene.Overlap(shape, position, rotation, found, filter);
					for (size_t index = 0; index < found.size() && index < maxHits; index++)
					{
						hits.push_back(DescribeQueryEntity(scene, found[index]));
					}
					break;
				}
			}
			bool const truncated = hits.size() < count;
			PhysicsScene const* const physics = scene.GetPhysicsScene();
			return Json::object({{"kind", kindName},
			                     {"state", GetEditorPlayStateName(context.GetPlayState())},
			                     {"frame", scene.GetRuntimeFrame()},
			                     {"step", physics != nullptr ? physics->GetStepCount() : uint64_t(0)},
			                     {"count", count},
			                     {"truncated", truncated},
			                     {"hits", std::move(hits)}});
		}

		// A material, or a definition, as an agent names it: its reference (a path, or the handle of an asset without one),
		// null for none.
		Json DescribeAsset(AssetHandle handle)
		{
			if (!handle.IsValid())
			{
				return Json();
			}
			return AssetManager::IsInitialized() ? Json(AssetManager::GetReference(handle)) : Json(handle.ToString());
		}

		Json DescribeVehicleInputs(VehicleInputs const& inputs)
		{
			return Json::object(
				{{"throttle", inputs.Throttle}, {"frontBrake", inputs.FrontBrake}, {"rearBrake", inputs.RearBrake}, {"lean", inputs.Lean}});
		}

		// The name and the surface name of a definition's wheel and of a physics material, read from the assets as they are
		// loaded (empty when they are not).
		std::string GetVehicleWheelName(AssetHandle definition, size_t wheel)
		{
			if (!definition.IsValid() || !AssetManager::IsInitialized())
			{
				return std::string();
			}
			Result<Ref<TypedDataAsset<Vehicle>>> const asset = AssetManager::TryGetAsset<TypedDataAsset<Vehicle>>(definition);
			if (!asset)
			{
				return std::string();
			}
			std::vector<VehicleWheelDefinition> const& wheels = asset.GetValue()->GetData().Wheels;
			return wheel < wheels.size() ? wheels[wheel].Name : std::string();
		}

		std::string GetSurfaceName(AssetHandle material)
		{
			if (!material.IsValid() || !AssetManager::IsInitialized())
			{
				return std::string();
			}
			Result<Ref<TypedDataAsset<PhysicsMaterial>>> const asset = AssetManager::TryGetAsset<TypedDataAsset<PhysicsMaterial>>(material);
			return asset ? asset.GetValue()->GetData().Surface : std::string();
		}

		Json DescribeVehicle(Scene& scene, Entity entity, VehicleState const& state)
		{
			AssetHandle const definition = entity.GetComponent<VehicleComponent>().Definition;
			Json wheels = Json::array();
			for (size_t index = 0; index < state.Wheels.size(); index++)
			{
				VehicleWheelState const& wheel = state.Wheels[index];
				Json described = Json::object({{"index", index}, {"name", GetVehicleWheelName(definition, index)}});
				described["contact"] = wheel.ContactEntity.IsValid() ? DescribeQueryEntity(scene, wheel.ContactEntity) : Json();
				described["surface"] = DescribeAsset(wheel.Surface);
				described["surfaceName"] = GetSurfaceName(wheel.Surface);
				described["tyreGrip"] = wheel.TyreGrip;
				described["rollingResistance"] = wheel.RollingResistance;
				described["suspensionLength"] = wheel.SuspensionLength;
				described["spin"] = wheel.AngularVelocity;
				described["steerAngle"] = glm::degrees(wheel.SteerAngle);
				described["longitudinalSlip"] = wheel.LongitudinalSlip;
				described["lateralSlip"] = glm::degrees(wheel.LateralSlip);
				described["contactPoint"] = ToJson(wheel.ContactPoint);
				described["contactNormal"] = ToJson(wheel.ContactNormal);
				wheels.push_back(std::move(described));
			}
			return Json::object({{"entity", entity.GetUUID().ToString()},
			                     {"name", entity.GetName()},
			                     {"definition", DescribeAsset(definition)},
			                     {"inputs", DescribeVehicleInputs(state.Inputs)},
			                     {"applied", DescribeVehicleInputs(state.AppliedInputs)},
			                     {"engineSpeed", state.EngineSpeed},
			                     {"engineTorque", state.EngineTorque},
			                     {"gear", state.Gear},
			                     {"clutch", state.ClutchFriction},
			                     {"shifting", state.IsShifting},
			                     {"forwardSpeed", state.ForwardSpeed},
			                     {"leanAngle", glm::degrees(state.LeanAngle)},
			                     {"targetLeanAngle", glm::degrees(state.TargetLeanAngle)},
			                     {"wheels", std::move(wheels)}});
		}

		// vehicle.state: the playing scene's vehicles as their last step left them (as a script's reads see them).
		CommandResult ReadVehicleState(PlayMode& playMode, EditorContext& context, Json const& params)
		{
			playMode.RefreshObserver();
			if (!playMode.IsPlaying())
			{
				return MakeCommandError(AutomationErrorCode::InvalidOperation,
				                        "nothing is playing: vehicle.state reads the playing scene's vehicles (play.start)");
			}
			Scene& scene = context.GetScene();
			scene.ApplyPhysicsChanges();
			PhysicsScene const* const physics = scene.GetPhysicsScene();
			if (physics == nullptr)
			{
				return MakeCommandError(AutomationErrorCode::Unavailable,
				                        "the playing scene has no physics world (physics is unavailable)");
			}

			Json vehicles = Json::array();
			if (auto const entityParameter = params.find("entity"); entityParameter != params.end())
			{
				std::string const& text = entityParameter->get_ref<std::string const&>();
				std::optional<UUID> const id = UUID::FromString(text);
				if (!id || !id->IsValid())
				{
					CommandError error = MakeCommandError(AutomationErrorCode::InvalidParams, "'{}' is not a valid entity ID", text);
					error.Data = Json::object({{"path", "entity"}});
					return error;
				}
				Entity const entity = scene.GetEntityByUUID(*id);
				if (!entity)
				{
					return MakeCommandError(AutomationErrorCode::EntityNotFound, "entity {} does not exist", *id);
				}
				if (!entity.HasComponent<VehicleComponent>())
				{
					return MakeCommandError(AutomationErrorCode::ComponentNotFound, "entity {} has no Vehicle component", *id);
				}
				std::optional<VehicleState> const state = physics->GetVehicleState(*id);
				if (!state)
				{
					return MakeCommandError(AutomationErrorCode::InvalidOperation,
					                        "entity {}'s Vehicle makes no vehicle: it needs a dynamic RigidBody, a collider and a "
					                        "definition that loads (the log says which is missing)",
					                        *id);
				}
				vehicles.push_back(DescribeVehicle(scene, entity, *state));
			}
			else
			{
				std::vector<std::pair<UUID, entt::entity>> found;
				for (entt::entity const handle : scene.GetRegistry().view<VehicleComponent>())
				{
					found.emplace_back(Entity(handle, &scene).GetUUID(), handle);
				}
				std::sort(found.begin(), found.end(),
				          [](auto const& first, auto const& second)
				          {
							  return first.first < second.first;
						  });
				for (auto const& [id, handle] : found)
				{
					if (std::optional<VehicleState> const state = physics->GetVehicleState(id))
					{
						vehicles.push_back(DescribeVehicle(scene, Entity(handle, &scene), *state));
					}
				}
			}
			return Json::object({{"state", GetEditorPlayStateName(context.GetPlayState())},
			                     {"frame", scene.GetRuntimeFrame()},
			                     {"step", physics->GetStepCount()},
			                     {"vehicles", std::move(vehicles)}});
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

	char const* GetEditorPlayStateName(EditorPlayState state)
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

	Result<void> RegisterPlayCommands(CommandRegistry& registry, PlayMode& playMode, EditorContext& context,
	                                  ViewportSizeProvider viewportSize)
	{
		Result<void> result =
			AddAsync(registry, "play.start",
		             "Plays a copy of the edited scene (mode play: scripts, physics and audio; simulate: physics only); panels and "
		             "commands then show and edit the copy, and play.stop discards it. paused: the scene starts (its scripts' OnCreate "
		             "run) and is paused before its first frame, for sessions that advance it with fixed steps (play.advance). Play "
		             "builds changed scripts first and fails when they do not build; asynchronous. Returns the play state.",
		             SchemaBuilder::Object()
		                 .Property("mode", SchemaBuilder::String("What runs").Enum({"play", "simulate"}).Default("play"))
		                 .Property("paused", SchemaBuilder::Boolean("Pause the scene before its first frame").Default(false))
		                 .Build(),
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
						 PlayStartOptions options;
						 if (auto const paused = params.find("paused"); paused != params.end())
						 {
							 options.Paused = paused->get<bool>();
						 }
						 glm::uvec2 const size = viewportSize();
						 playMode.RequestStart(
							 state, size.x, size.y, nullptr,
							 [completion, &playMode, &context](Result<void> const& started)
							 {
								 if (!started)
								 {
									 completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError()));
									 return;
								 }
								 completion.Complete(DescribePlay(playMode, context));
							 },
							 options);
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
			result ? Add(registry, "play.pause",
		                 "Pauses or resumes the running scene (and its sounds). Resuming while an execution is stopped at a "
		                 "breakpoint continues every stopped execution first; the scene stays paused when one stops again (the "
		                 "result's debug tells).",
		                 SchemaBuilder::Object().Property("paused", SchemaBuilder::Boolean("Pause (true) or resume").Default(true)).Build(),
		                 false,
		                 [&playMode, &context](Json const& params) -> CommandResult
		                 {
							 if (!playMode.IsPlaying())
							 {
								 return NotPlaying();
							 }
							 auto const paused = params.find("paused");
							 if (paused == params.end() || paused->get<bool>())
							 {
								 playMode.SetPaused(true);
							 }
							 else
							 {
								 // Stopped executions continue first: they may stop again, or quit.
								 playMode.Resume();
							 }
							 return DescribePlay(playMode, context);
						 })
				   : result;

		result = result
		             ? Add(registry, "play.step",
		                   "Advances the paused scene by frames, one per editor frame with the editor's frame time (play.advance runs "
		                   "frames at once). The input set while paused reaches the first frame stepped. Refused while an execution "
		                   "is stopped at a breakpoint.",
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
							   if (std::optional<CommandError> refused = RefuseFrames(playMode))
							   {
								   return std::move(*refused);
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

		result =
			result ? Add(registry, "play.advance",
		                 "Runs frames at once with a fixed time step, paused or not; input set before applies to the first frame, "
		                 "also input set while paused with editor frames between. "
		                 "Pause first for deterministic runs: the editor keeps playing in real time otherwise. Stops early when "
		                 "the scripts quit (Application.Quit) and after a frame in which an execution stopped at a breakpoint; "
		                 "refused while one is stopped. Returns the play state with framesRun, stopped (an execution stopped) and "
		                 "stop (as graph.debug's stops, or null: also when the stop ended in the frame that made it, its entity "
		                 "destroyed or another scene loaded, which keeps the scene paused).",
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
							 if (std::optional<CommandError> refused = RefuseFrames(playMode))
							 {
								 return std::move(*refused);
							 }
							 auto const timestep = params.find("timestep");
							 float const seconds = static_cast<float>(timestep != params.end() ? timestep->get<double>() : DefaultTimestep);
							 uint64_t const stopsMade = playMode.GetDebugSummary().StopsMade;
							 uint32_t const run = playMode.Advance(params.at("frames").get<uint32_t>(), Timestep(seconds));
							 Json description = DescribePlay(playMode, context);
							 description["framesRun"] = run;
							 // A stop made by the frames ended them: the stop is the current one, unless it already ended in
			                 // its frame (a scene load, its entity destroyed).
							 PlayDebugSummary const debug = playMode.GetDebugSummary();
							 bool const stopped = debug.StopsMade > stopsMade;
							 VisualScriptStop const* stop = stopped && debug.Current ? playMode.FindDebugStop(*debug.Current) : nullptr;
							 description["stopped"] = stopped;
							 description["stop"] = stop != nullptr ? DescribeVisualScriptStop(*stop, context.GetScene()) : Json();
							 return description;
						 })
				   : result;

		result = result ? Add(registry, "play.state",
		                      "The play state: { state (edit, play or simulate), paused, startPending, scene, frame and time (while "
		                      "playing), tests (the run's Strada.Testing report: finished, passed, failed, exceptions, failures, "
		                      "results; null before the first run), debug (the visual-script debugger: state (Detached, Running, "
		                      "Stopped, Stepping or StepEnded), stops, current (the current stop's ID or null), pauseHeld (it paused "
		                      "the scene for a stop and has not resumed it)) }.",
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
		              "(KeyCode and MouseButton names), the mouse position in pixels of the game view and scroll for one frame. "
		              "Pressed and released last one frame. While playing that is the game's next frame: a paused game keeps the "
		              "presses, releases, mouse movement and scroll until its next frame (play.step, play.advance), however many "
		              "editor frames come between; graph code that resuming a stopped execution runs reads it too. actions: the "
		              "project's input actions by name -> a value held as one more device of the action (a number or a bool for "
		              "Button and Axis actions, [x, y] for Vector2), or null to clear it; held until input.release, null or "
		              "play.stop, and read by the game's next frame (never by a resumed execution). gamepads: slot \"0\" to "
		              "\"15\" -> { buttons: name -> down, axes: name -> -1 to 1 } injected in place of the device (a first "
		              "injection starts connected and at rest, triggers at -1; a later one changes what it names; window polls "
		              "leave it alone), or null to give the slot back to its device. Everything is checked before anything "
		              "changes. Returns the keys and buttons down, the mouse position, the action overrides held ([x, y]) and the "
		              "injected gamepads.",
		              SchemaBuilder::Object()
		                  .Property("keys", SchemaBuilder::Object("Key name -> down").AdditionalProperties(SchemaBuilder::Boolean()))
		                  .Property("mouseButtons",
		                            SchemaBuilder::Object("Button name -> down").AdditionalProperties(SchemaBuilder::Boolean()))
		                  .Property("mousePosition", Vector2Schema("Position in pixels from the game view's top-left corner"))
		                  .Property("scroll", Vector2Schema("Scroll this frame (y is the usual wheel)"))
		                  .Property("actions", SchemaBuilder::Object("Input action name -> a number, a bool, [x, y] (Vector2 actions) or "
		                                                             "null (clears the override)")
		                                           .AdditionalProperties(SchemaBuilder::Any()))
		                  .Property("gamepads",
		                            SchemaBuilder::Object("Gamepad slot \"0\" to \"15\" -> its buttons and axes, or null (releases it)")
		                                .AdditionalProperties(
		                                    SchemaBuilder::Object()
		                                        .Property("buttons", SchemaBuilder::Object("GamepadButton name -> down")
		                                                                 .AdditionalProperties(SchemaBuilder::Boolean()))
		                                        .Property("axes", SchemaBuilder::Object("GamepadAxis name -> -1 to 1 (triggers rest at -1)")
		                                                              .AdditionalProperties(SchemaBuilder::Number().Minimum(-1.0).Maximum(1.0)))
		                                        .Nullable()))
		                  .Build(),
		              false,
		              [&context](Json const& params) -> CommandResult
		              {
						  // Every name and value is checked before anything changes.
						  std::vector<ActionRequest> actionRequests;
						  if (auto const entries = params.find("actions"); entries != params.end())
						  {
							  if (std::optional<CommandError> refused = ReadActionRequests(*entries, context, actionRequests))
							  {
								  return std::move(*refused);
							  }
						  }
						  std::vector<GamepadRequest> gamepadRequests;
						  if (auto const entries = params.find("gamepads"); entries != params.end())
						  {
							  if (std::optional<CommandError> refused = ReadGamepadRequests(*entries, gamepadRequests))
							  {
								  return std::move(*refused);
							  }
						  }
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
						  for (ActionRequest const& request : actionRequests)
						  {
							  if (request.Value)
							  {
								  // Checked above: the name is the project's, the value finite and the overrides below their limit.
								  static_cast<void>(Input::SetActionOverride(request.Name, *request.Value));
							  }
							  else
							  {
								  Input::ClearActionOverride(request.Name);
							  }
						  }
						  for (GamepadRequest const& request : gamepadRequests)
						  {
							  if (request.State)
							  {
								  Input::InjectGamepadState(request.Slot, *request.State);
							  }
							  else
							  {
								  Input::ReleaseInjectedGamepad(request.Slot);
							  }
						  }
						  return DescribeInput();
					  })
				: result;

		result = result ? Add(registry, "input.release",
		                      "Releases every key and mouse button (they report released for one frame), every injected gamepad and "
		                      "every input action override.",
		                      Json(), false,
		                      [](Json const&) -> CommandResult
		                      {
								  Input::ReleaseKeysAndMouseButtons();
								  Input::ReleaseInjectedInput();
								  return DescribeInput();
							  })
		                : result;

		result =
			result
				? Add(registry, "ui.layout",
		              "Where the screen-space UI is in the game view: every element of the scene's canvases (the running scene "
		              "while playing, else the edited one) as the game lays it out to draw it and to find what the mouse is over, "
		              "at the game view's size. Returns { width, height, pointerOverUI, elements: [{ id, name, canvas, min, max, "
		              "center (game-view pixels from the top-left corner, y down, as input.set's mousePosition), opacity, "
		              "hitTarget (takes clicks), button (the button its clicks go to, or null), state (buttons only: normal, "
		              "hovered, pressed or disabled) }] } in drawing order, topmost last. entity: only that element.",
		              SchemaBuilder::Object().Property("entity", SchemaBuilder::String("Entity ID of one UI element").MinLength(1)).Build(),
		              true,
		              [&context, viewportSize](Json const& params) -> CommandResult
		              {
						  return DescribeUILayout(context, viewportSize, params);
					  })
				: result;

		SchemaBuilder const coordinateSchema = SchemaBuilder::Number().Minimum(-MaxPhysicsCoordinate).Maximum(MaxPhysicsCoordinate);
		SchemaBuilder const radiusSchema = SchemaBuilder::Number().Minimum(0.0).Maximum(MaxPhysicsQueryRadius);
		SchemaBuilder const extentSchema = SchemaBuilder::Number().Minimum(0.0).Maximum(MaxPhysicsQueryExtent);
		SchemaBuilder const componentSchema = SchemaBuilder::Number().Minimum(-1.0e6).Maximum(1.0e6);
		result =
			result
				? Add(registry, "physics.query",
		              "Asks the playing scene's physics world (play or simulate, paused or not) what a ray or a shape meets, as a "
		              "script's query does: the world as of its last step, with the bodies made or moved since put where they "
		              "are. kind: raycast (the first solid hit, triggers passed through), raycastAll (every entity along the ray, "
		              "its nearest hit each, nearest first, then by entity ID), shapeCast (the first hit of a sphere, box or capsule "
		              "swept along direction; a collider the shape starts in is hit at distance 0) or overlap (the entities the shape "
		              "overlaps where it is, by entity ID). origin is the ray's start or the shape's center. Returns { kind, state, "
		              "frame, step (physics steps run), count (hits found), truncated (more than maxHits), hits: [{ entity, name, "
		              "point, normal, distance, material (the physics material's reference or null) }] (overlap: entity and name "
		              "only) }. A parameter the kind has no use for is refused (InvalidParams). Read-only: not an undo step. "
		              "Unavailable while nothing plays or without physics.",
		              SchemaBuilder::Object()
		                  .Property("kind", SchemaBuilder::String("What to ask").Enum({"raycast", "raycastAll", "shapeCast", "overlap"}),
		                            true)
		                  .Property(
							  "origin",
							  SchemaBuilder::Array(coordinateSchema, "World position [x, y, z]: the ray's start or the shape's center")
								  .MinItems(3)
								  .MaxItems(3),
							  true)
		                  .Property("direction",
		                            SchemaBuilder::Array(componentSchema, "Where the ray or shape goes [x, y, z], any length but zero "
		                                                                  "(raycast, raycastAll and shapeCast: required)")
		                                .MinItems(3)
		                                .MaxItems(3))
		                  .Property(
							  "distance",
							  SchemaBuilder::Number(
								  "How far in meters, more than 0 (raycast, raycastAll, shapeCast; default as far as the world reaches)")
								  .Minimum(0.0)
								  .Maximum(MaxPhysicsQueryDistance))
		                  .Property("shape",
		                            SchemaBuilder::String("The shape (shapeCast and overlap: required)").Enum({"sphere", "box", "capsule"}))
		                  .Property("radius", SchemaBuilder(radiusSchema).Description("Sphere and capsule radius in meters").Default(0.5))
		                  .Property("halfExtents",
		                            SchemaBuilder::Array(extentSchema, "Box half extents [x, y, z] in meters (default 0.5 each)")
		                                .MinItems(3)
		                                .MaxItems(3))
		                  .Property("halfHeight", SchemaBuilder(extentSchema)
		                                              .Description("Capsule: half the length of the cylinder between its caps, along "
		                                                           "its local Y axis")
		                                              .Default(0.5))
		                  .Property("rotation",
		                            SchemaBuilder::Array(componentSchema, "The shape's rotation as a quaternion [x, y, z, w] (box and "
		                                                                  "capsule; default none)")
		                                .MinItems(4)
		                                .MaxItems(4))
		                  .Property("rotationEuler",
		                            SchemaBuilder::Array(componentSchema, "The shape's rotation as [pitch, yaw, roll] in degrees, "
		                                                                  "in place of rotation")
		                                .MinItems(3)
		                                .MaxItems(3))
		                  .Property("mask", SchemaBuilder::Integer("Layers seen: bit i sees layer i (default every layer)")
		                                        .Minimum(0)
		                                        .Maximum(AllPhysicsLayers)
		                                        .Default(AllPhysicsLayers))
		                  .Property("triggers", SchemaBuilder::Boolean("Whether trigger colliders count (not for raycast)").Default(false))
		                  .Property("maxHits", SchemaBuilder::Integer("Most hits returned (raycastAll and overlap)")
		                                           .Minimum(1)
		                                           .Maximum(MaxQueryHits)
		                                           .Default(DefaultQueryHits))
		                  .Build(),
		              true,
		              [&playMode, &context](Json const& params) -> CommandResult
		              {
						  return QueryPhysics(playMode, context, params);
					  })
				: result;
		result = result
		             ? Add(registry, "vehicle.state",
		                   "Reads the playing scene's vehicles (play or simulate, paused or not) as their last physics step left them, "
		                   "as a script's reads see them: every vehicle in the order of its entity's ID, or the one entity names. "
		                   "Returns { state, frame, step (physics steps run), vehicles: [{ entity, name, definition (the .svehicle's "
		                   "reference), inputs and applied ({ throttle, frontBrake, rearBrake, lean }: the latest given and those the "
		                   "last step applied), engineSpeed (rpm), engineTorque (N m), gear (0 neutral), clutch (0 open to 1 "
		                   "closed), shifting, forwardSpeed (m/s along the chassis's front), leanAngle and targetLeanAngle (degrees, "
		                   "positive to the right), wheels: [{ index (0 front, 1 rear), name, contact ({ entity, name } or null), "
		                   "surface (the physics material's reference or null), surfaceName, tyreGrip, rollingResistance, "
		                   "suspensionLength (m), spin (rad/s), steerAngle (degrees), longitudinalSlip (the ratio's size, never "
		                   "negative), lateralSlip (degrees, 0 to 90), contactPoint, contactNormal }] }] }. Read-only: not an undo "
		                   "step. InvalidOperation while nothing plays and for an entity whose Vehicle makes no vehicle; "
		                   "EntityNotFound, ComponentNotFound (no Vehicle); Unavailable without physics.",
		                   SchemaBuilder::Object()
		                       .Property("entity", SchemaBuilder::String("Entity ID of one vehicle (default every vehicle)").MinLength(1))
		                       .Build(),
		                   true,
		                   [&playMode, &context](Json const& params) -> CommandResult
		                   {
							   return ReadVehicleState(playMode, context, params);
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
			"copy of the edited scene. Breakpoints are ignored: the run attaches no debugger. Scripts build first when they "
			"changed; asynchronous. Returns { finished, timedOut, frames, passed, failed, exceptions, failures, results: "
			"[{ name, passed, message }] }.",
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
					Result<Ref<Scene>> loaded = LoadSceneAsset(asset);
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
				playMode.RequestStart(
					EditorPlayState::Play, size.x, size.y, std::move(scene),
					[completion, &playMode, maxFrames, seconds](Result<void> const& started)
					{
						if (!started)
						{
							completion.Complete(MakeCommandError(AutomationErrorCode::InvalidOperation, "{}", started.GetError()));
							return;
						}
						uint32_t frames = 0;
						while (playMode.IsPlaying() && frames < maxFrames && !playMode.GetTestReport()->Finished)
						{
							// A run no frame can advance ends (a test run attaches no debugger, which alone refuses frames).
							uint32_t const ran = playMode.Advance(1, Timestep(static_cast<float>(seconds)));
							if (ran == 0)
							{
								break;
							}
							frames += ran;
						}
						playMode.Stop();
						TestRunReport const report = *playMode.GetTestReport();
						Json description = DescribeTestReport(report);
						description["timedOut"] = !report.Finished && frames >= maxFrames;
						description["frames"] = frames;
						completion.Complete(std::move(description));
					},
					PlayStartOptions{.TestRun = true});
			});
	}
}
