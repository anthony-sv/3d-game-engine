#include "stpch.h"
#include "Strada/Project/ProjectSettings.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"

#include <algorithm>
#include <set>

namespace Strada
{
	namespace
	{
		// A path relative to the project directory that stays inside it.
		Result<void> ValidateProjectPath(std::string_view field, std::string const& value)
		{
			if (value.empty())
			{
				return MakeError("Project.{}: must not be empty", field);
			}
			std::filesystem::path const path = FileSystem::PathFromUtf8(value);
			if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
			{
				return MakeError("Project.{}: must be relative to the project directory, got '{}'", field, value);
			}
			for (std::filesystem::path const& part : path.lexically_normal())
			{
				if (part == "..")
				{
					return MakeError("Project.{}: must stay inside the project directory, got '{}'", field, value);
				}
			}
			return {};
		}
	}

	Result<void> ValidateProjectSettings(ProjectSettings const& settings)
	{
		if (settings.Name.empty())
		{
			return Error{"Project.Name: must not be empty"};
		}
		if (Result<void> result = ValidateProjectPath("AssetDirectory", settings.AssetDirectory); !result)
		{
			return result;
		}
		if (Result<void> result = ValidateProjectPath("ScriptModule", settings.ScriptModule); !result)
		{
			return result;
		}

		std::vector<std::string> const& layers = settings.Physics.Layers;
		if (layers.empty() || layers.size() > MaxPhysicsLayers)
		{
			return MakeError("Project.Physics.Layers: between 1 and {} layers are required, got {}", MaxPhysicsLayers, layers.size());
		}
		std::set<std::string> names;
		for (std::string const& layer : layers)
		{
			if (layer.empty())
			{
				return Error{"Project.Physics.Layers: layer names must not be empty"};
			}
			if (!names.insert(layer).second)
			{
				return MakeError("Project.Physics.Layers: layer '{}' is listed twice", layer);
			}
		}
		for (PhysicsLayerPair const& pair : settings.Physics.IgnoredCollisions)
		{
			if (pair.First >= layers.size() || pair.Second >= layers.size())
			{
				return MakeError("Project.Physics.IgnoredCollisions: layers {} and {} do not both exist ({} layers)", pair.First,
				                 pair.Second, layers.size());
			}
		}
		return {};
	}

	SceneRuntimeSettings MakeSceneRuntimeSettings(ProjectSettings const& settings)
	{
		SceneRuntimeSettings runtime;
		runtime.FixedTimestep = settings.Physics.FixedTimestep;
		runtime.PhysicsLayerCount = static_cast<uint32_t>(std::clamp<size_t>(settings.Physics.Layers.size(), 1, MaxPhysicsLayers));
		for (PhysicsLayerPair const& pair : settings.Physics.IgnoredCollisions)
		{
			runtime.IgnoredCollisions.emplace_back(pair.First, pair.Second);
		}
		return runtime;
	}
}
