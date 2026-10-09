#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/StructSerialization.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace Strada
{
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
			Field("Resizable", &ProjectWindowSettings::Resizable).Doc("Lets players resize the window."));
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
				.Range(0.001, 0.1)
				.Doc("Seconds per physics step; scripts' OnFixedUpdate runs once per step."),
			Field("Layers", &ProjectPhysicsSettings::Layers).Doc("Names of the physics layers (at most 16); RigidBody.Layer indexes them."),
			Field("IgnoredCollisions", &ProjectPhysicsSettings::IgnoredCollisions)
				.Doc("Pairs of layer indices whose bodies do not collide; every other pair collides."));
	};

	// Settings of a project, stored in its project file (.sproj) under "Project".
	struct ProjectSettings
	{
		std::string Name = "Untitled";
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
			Field("Name", &ProjectSettings::Name).Doc("The game's name."),
			Field("AssetDirectory", &ProjectSettings::AssetDirectory).Doc("The asset directory, relative to the project directory."),
			Field("ScriptModule", &ProjectSettings::ScriptModule).Doc("The compiled C# game assembly, relative to the project directory."),
			Field("StartScene", &ProjectSettings::StartScene).References("Scene").Doc("The scene exported games start with."),
			Field("Window", &ProjectSettings::Window), Field("Physics", &ProjectSettings::Physics));
	};

	inline constexpr size_t MaxPhysicsLayers = 16;

	// Checks what field ranges cannot express: a non-empty name, relative paths inside the project, 1 to 16 unique,
	// non-empty layer names and ignored pairs naming existing layers.
	[[nodiscard]] Result<void> ValidateProjectSettings(ProjectSettings const& settings);
}
