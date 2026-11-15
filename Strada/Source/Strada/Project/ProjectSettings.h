#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Result.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Serialization/StructSerialization.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace Strada
{
	struct SceneRuntimeSettings;

	// The window of the exported game (the editor keeps its own window).
	struct ProjectWindowSettings
	{
		// Empty uses the project name.
		std::string Title;
		uint32_t Width = 1280;
		uint32_t Height = 720;
		bool Fullscreen = false;
		bool VSync = true;
		bool Resizable = true;
		// The window's icon (a texture of the project); none keeps the Strada icon.
		AssetHandle Icon;

		bool operator==(ProjectWindowSettings const& other) const = default;
	};

	template<>
	struct StructTraits<ProjectWindowSettings>
	{
		static constexpr std::string_view Name = "Window";
		static constexpr auto Fields = std::make_tuple(
			Field("Title", &ProjectWindowSettings::Title).Doc("Window title of the exported game; empty uses the project name."),
			Field("Width", &ProjectWindowSettings::Width).Range(64.0, 16384.0).Doc("Initial window width in pixels."),
			Field("Height", &ProjectWindowSettings::Height).Range(64.0, 16384.0).Doc("Initial window height in pixels."),
			Field("Fullscreen", &ProjectWindowSettings::Fullscreen).Doc("Starts in fullscreen on the primary monitor."),
			Field("VSync", &ProjectWindowSettings::VSync).Doc("Waits for vertical sync when presenting."),
			Field("Resizable", &ProjectWindowSettings::Resizable).Doc("Lets players resize the window."),
			Field("Icon", &ProjectWindowSettings::Icon)
				.References("Texture")
				.Doc("The game window's icon (a square image such as 256x256); none keeps the Strada icon."));
	};

	// Two physics layers whose bodies do not collide.
	struct PhysicsLayerPair
	{
		uint32_t First = 0;
		uint32_t Second = 0;

		bool operator==(PhysicsLayerPair const& other) const = default;
	};

	template<>
	struct StructTraits<PhysicsLayerPair>
	{
		static constexpr std::string_view Name = "LayerPair";
		static constexpr auto Fields = std::make_tuple(Field("First", &PhysicsLayerPair::First).Range(0.0, 15.0),
		                                               Field("Second", &PhysicsLayerPair::Second).Range(0.0, 15.0));
	};

	struct ProjectPhysicsSettings
	{
		// Seconds per physics step.
		float FixedTimestep = 1.0f / 60.0f;
		// Physics steps a frame runs at most: slower frames slow the game down instead of falling further behind.
		uint32_t MaxStepsPerFrame = 8;
		// Collision steps each physics step is divided into (1 to MaxPhysicsCollisionSteps).
		uint32_t CollisionSteps = 1;
		// Physics layer names; RigidBodyComponent::Layer indexes them. At most MaxPhysicsLayers.
		std::vector<std::string> Layers = {"Default"};
		// Layer pairs whose bodies do not collide (every other pair collides).
		std::vector<PhysicsLayerPair> IgnoredCollisions;

		bool operator==(ProjectPhysicsSettings const& other) const = default;
	};

	template<>
	struct StructTraits<ProjectPhysicsSettings>
	{
		static constexpr std::string_view Name = "Physics";
		static constexpr auto Fields = std::make_tuple(
			Field("FixedTimestep", &ProjectPhysicsSettings::FixedTimestep)
				.Range(0.0005, 0.1)
				.Doc("Seconds per physics step; scripts' OnFixedUpdate runs once per step."),
			Field("MaxStepsPerFrame", &ProjectPhysicsSettings::MaxStepsPerFrame)
				.Range(1.0, 64.0)
				.Doc("Physics steps a frame runs at most; slower frames slow the game down instead of falling further behind."),
			Field("CollisionSteps", &ProjectPhysicsSettings::CollisionSteps)
				.Range(1.0, 4.0)
				.Doc("Collision steps each physics step is divided into; more resolve fast or stiff bodies better, at their cost."),
			Field("Layers", &ProjectPhysicsSettings::Layers).Doc("Names of the physics layers (at most 16); RigidBody.Layer indexes them."),
			Field("IgnoredCollisions", &ProjectPhysicsSettings::IgnoredCollisions)
				.Doc("Pairs of layer indices whose bodies do not collide; every other pair collides."));
	};

	// Who makes the game and what it is called: they name the folder its data goes in on each player's machine (GameData)
	// and the macOS bundle identifier. Both are optional, and a project that sets neither keeps what it had before them.
	struct ProjectIdentity
	{
		// Empty, or without an ASCII letter or digit, means no company level.
		std::string Company;
		// Empty, or without an ASCII letter or digit, uses the project's name (GameIdentity::Resolve).
		std::string Product;

		bool operator==(ProjectIdentity const& other) const = default;
	};

	template<>
	struct StructTraits<ProjectIdentity>
	{
		static constexpr std::string_view Name = "Identity";
		static constexpr auto Fields = std::make_tuple(
			Field("Company", &ProjectIdentity::Company)
				.Doc(
					"Who makes the game; names the folder that holds the game's data folder. Empty, or without an ASCII letter or digit, for none."),
			Field("Product", &ProjectIdentity::Product)
				.Doc(
					"The game's name in its data folder and bundle identifier; empty, or without an ASCII letter or digit, uses the project's name."));
	};

	// Settings of a project, stored in its project file (.sproj) under "Project".
	struct ProjectSettings
	{
		std::string Name = "Untitled";
		ProjectIdentity Identity;
		// Relative to the project directory.
		std::string AssetDirectory = "Assets";
		// The compiled C# game assembly, relative to the project directory.
		std::string ScriptModule = "Scripts/Binaries/Game.dll";
		// The scene exported games start with.
		AssetHandle StartScene;
		ProjectWindowSettings Window;
		ProjectPhysicsSettings Physics;

		bool operator==(ProjectSettings const& other) const = default;
	};

	template<>
	struct StructTraits<ProjectSettings>
	{
		static constexpr std::string_view Name = "Project";
		static constexpr auto Fields = std::make_tuple(
			Field("Name", &ProjectSettings::Name).Doc("The game's name."), Field("Identity", &ProjectSettings::Identity),
			Field("AssetDirectory", &ProjectSettings::AssetDirectory).Doc("The asset directory, relative to the project directory."),
			Field("ScriptModule", &ProjectSettings::ScriptModule).Doc("The compiled C# game assembly, relative to the project directory."),
			Field("StartScene", &ProjectSettings::StartScene).References("Scene").Doc("The scene exported games start with."),
			Field("Window", &ProjectSettings::Window), Field("Physics", &ProjectSettings::Physics));
	};

	// Checks what field ranges cannot express: a non-empty name, relative paths inside the project, 1 to 16 unique,
	// non-empty layer names and ignored pairs naming existing layers.
	[[nodiscard]] Result<void> ValidateProjectSettings(ProjectSettings const& settings);

	// What a scene's runtime takes from the project: the physics step, steps per frame and collision steps, layers and
	// ignored layer pairs.
	SceneRuntimeSettings MakeSceneRuntimeSettings(ProjectSettings const& settings);

	// The game's identity: the company as set, the product as set or else the project's name.
	[[nodiscard]] GameIdentity GetGameIdentity(ProjectSettings const& settings);
}
