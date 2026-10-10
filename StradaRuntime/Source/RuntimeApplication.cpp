#include "Runtime/RuntimeLayer.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/CommandLine.h"
#include "Strada/Core/EntryPoint.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/ScriptProject.h"
#include "Strada/Script/ScriptEngine.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr double DefaultTestTimeout = 60.0;
		constexpr double DefaultTestTimestep = 1.0 / 60.0;

		struct RuntimeSpecification
		{
			// Game.sgame of an exported game, or a project file.
			std::filesystem::path GameFile;
			ProjectFileKind GameKind = ProjectFileKind::Game;
			// A scene file relative to the asset directory to start with; empty for the game's start scene.
			std::string StartScene;
			RuntimeLayerSpecification Layer;
		};

		// The exported game next to the executable, or in the app bundle's Resources directory on macOS.
		std::filesystem::path FindExportedGame()
		{
			std::filesystem::path const directory = FileSystem::GetExecutableDirectory();
			std::filesystem::path const besideExecutable = directory / FileSystem::PathFromUtf8(Project::GameFileName);
#if defined(ST_PLATFORM_MACOS)
			if (!FileSystem::IsRegularFile(besideExecutable))
			{
				return directory.parent_path() / "Resources" / FileSystem::PathFromUtf8(Project::GameFileName);
			}
#endif
			return besideExecutable;
		}
	}

	class RuntimeApplication final : public Application
	{
	public:
		RuntimeApplication(ApplicationSpecification const& specification, RuntimeSpecification runtimeSpecification)
			: Application(specification),
			  m_RuntimeSpecification(std::move(runtimeSpecification))
		{
		}

	protected:
		Result<void> OnInit() override
		{
			std::vector<std::string> warnings;
			Result<Ref<Project>> opened = m_RuntimeSpecification.GameKind == ProjectFileKind::Game
			                                  ? Project::OpenGame(m_RuntimeSpecification.GameFile, &warnings)
			                                  : Project::Open(m_RuntimeSpecification.GameFile, &warnings);
			if (!opened)
			{
				return Error{opened.GetError()};
			}
			for (std::string const& warning : warnings)
			{
				ST_WARN("{}", warning);
			}
			Ref<Project> game = opened.TakeValue();
			if (Result<void> scripts = StartScripts(*game); !scripts)
			{
				return scripts;
			}
			Result<Ref<Scene>> scene = LoadStartScene(*game);
			if (!scene)
			{
				return Error{scene.GetError()};
			}
			PushLayer<RuntimeLayer>(m_RuntimeSpecification.Layer, std::move(game), scene.TakeValue());
			return {};
		}

	private:
		// Games without scripts run without .NET.
		static Result<void> StartScripts(Project const& game)
		{
			std::filesystem::path const assembly = ScriptProject::GetAssemblyPath(game);
			if (!FileSystem::IsRegularFile(assembly))
			{
				if (!game.IsGame() && ScriptProject::Exists(game))
				{
					return MakeError("the project's scripts are not built ({} is missing): build them in the editor first",
					                 FileSystem::PathToUtf8(assembly));
				}
				ST_INFO("The game has no scripts");
				return {};
			}
			if (!game.IsGame() && ScriptProject::IsOutOfDate(game))
			{
				ST_WARN("The project's scripts changed since they were built: running the last build");
			}

			ScriptEngineSettings settings;
			// Exported games carry the scripting API next to their scripts; projects use the engine's.
			if (game.IsGame())
			{
				settings.ScriptCoreDirectory = assembly.parent_path();
			}
			if (Result<void> started = ScriptEngine::Init(settings); !started)
			{
				return MakeError("the game's scripts cannot run: {}", started.GetError());
			}
			if (Result<void> loaded = ScriptEngine::LoadGameAssembly(assembly); !loaded)
			{
				return MakeError("the game's scripts cannot be loaded: {}", loaded.GetError());
			}
			return {};
		}

		Result<Ref<Scene>> LoadStartScene(Project const& game) const
		{
			AssetHandle scene = game.GetSettings().StartScene;
			if (!m_RuntimeSpecification.StartScene.empty())
			{
				scene = AssetManager::FindByPath(m_RuntimeSpecification.StartScene);
				if (AssetManager::GetAssetType(scene) != AssetType::Scene)
				{
					return MakeError("the game has no scene '{}'", m_RuntimeSpecification.StartScene);
				}
			}
			else if (!scene.IsValid())
			{
				return Error{"the game has no start scene: choose one in the project settings"};
			}
			return LoadSceneAsset(scene);
		}

		RuntimeSpecification m_RuntimeSpecification;
	};

	Scope<Application> CreateApplication(ApplicationCommandLineArgs args, int& exitCode)
	{
		CommandLineParser parser("Strada game player: runs the exported game next to it, or a project.\n"
		                         "Test runs (--test) exit with the number of failures (at most 100; a run that does not\n"
		                         "finish counts as one more); a game that cannot start exits with 1.");
		parser.AddFlag("help", "Show this help and exit")
			.AddOption("game", "path", "Run the exported game configured by this Game.sgame (default: the one next to the player)")
			.AddOption("project", "path", "Run a project (.sproj) from its directory, with its last script build")
			.AddOption("scene", "path", "Start with this scene (relative to the asset directory) instead of the start scene")
			.AddFlag("test", "Run headless with a fixed time step until the scripts finish testing, quit or time out")
			.AddOption("timeout", "seconds", "Game time a test run may take (default 60)")
			.AddOption("timestep", "seconds", "Advance the game by this time each frame (default: the frame's real time; 1/60 in tests)")
			.AddFlag("headless", "Run without a window, rendering or sound")
			.AddOption("frames", "count", "Exit after the given number of frames")
			.AddOption("screenshot", "path", "Save a PNG of the window (on the last frame with --frames, otherwise frame 60)")
			.AddOption("gpu", "index", "Use the GPU with this adapter index")
			.AddFlag("validation", "Enable Vulkan and NVRHI validation")
			.AddFlag("no-validation", "Disable Vulkan and NVRHI validation")
			.AddFlag("no-vsync", "Present without waiting for vertical sync");

		Result<CommandLineArguments> parsed = parser.Parse(args.Count, args.Args);
		auto const fail = [&parser, &exitCode](std::string const& message) -> Scope<Application>
		{
			std::fprintf(stderr, "%s\n\n%s", message.c_str(), parser.GetHelpText("StradaRuntime").c_str());
			exitCode = 2;
			return nullptr;
		};
		if (!parsed)
		{
			return fail(parsed.GetError());
		}
		CommandLineArguments const& arguments = parsed.GetValue();
		if (arguments.HasFlag("help"))
		{
			std::printf("%s", parser.GetHelpText("StradaRuntime").c_str());
			exitCode = 0;
			return nullptr;
		}

		std::optional<std::string> const project = arguments.GetValue("project");
		std::optional<std::string> const gameFile = arguments.GetValue("game");
		if (project && gameFile)
		{
			return fail("--game and --project cannot be combined");
		}
		bool const test = arguments.HasFlag("test");
		bool const headless = test || arguments.HasFlag("headless");
		std::optional<double> const timestep = arguments.GetDouble("timestep");
		if (timestep && !(*timestep >= 0.001 && *timestep <= 1.0))
		{
			return fail("--timestep must be between 0.001 and 1 seconds");
		}
		std::optional<double> const timeout = arguments.GetDouble("timeout");
		if (timeout && !(*timeout > 0.0 && *timeout <= 86400.0))
		{
			return fail("--timeout must be between 0 and 86400 seconds");
		}
		if (timeout && !test)
		{
			return fail("--timeout applies to test runs (--test)");
		}
		std::optional<std::string> const screenshot = arguments.GetValue("screenshot");
		if (screenshot && headless)
		{
			return fail("--screenshot needs a window (not --headless or --test)");
		}

		RuntimeSpecification runtime;
		runtime.GameKind = project ? ProjectFileKind::Project : ProjectFileKind::Game;
		runtime.GameFile = project    ? FileSystem::PathFromUtf8(*project)
		                   : gameFile ? FileSystem::PathFromUtf8(*gameFile)
		                              : FindExportedGame();
		if (runtime.GameKind == ProjectFileKind::Game && FileSystem::IsDirectory(runtime.GameFile))
		{
			runtime.GameFile /= FileSystem::PathFromUtf8(Project::GameFileName);
		}
		if (std::optional<std::string> const scene = arguments.GetValue("scene"))
		{
			runtime.StartScene = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(*scene));
		}

		// The window comes from the game, and opens before the engine starts.
		Result<ProjectWindowSettings> window = Project::ReadWindowSettings(runtime.GameFile, runtime.GameKind);
		if (!window)
		{
			bool const located = project.has_value() || gameFile.has_value();
			ST_CRITICAL("Cannot run the game: {}{}", window.GetError(),
			            located ? "" : " (a player without an exported game next to it runs one given by --game or --project)");
			exitCode = 1;
			return nullptr;
		}

		ApplicationSpecification specification;
		specification.Name = window.GetValue().Title;
		specification.CommandLineArgs = args;
		specification.Window.Title = window.GetValue().Title;
		specification.Window.Width = window.GetValue().Width;
		specification.Window.Height = window.GetValue().Height;
		specification.Window.Fullscreen = window.GetValue().Fullscreen;
		specification.Window.Resizable = window.GetValue().Resizable;
		specification.Window.VSync = window.GetValue().VSync && !arguments.HasFlag("no-vsync");
		// Scripting starts once the game's scripts are found (StartScripts).
		specification.EnableScripting = false;
		runtime.Layer.ViewportWidth = window.GetValue().Width;
		runtime.Layer.ViewportHeight = window.GetValue().Height;

		if (headless)
		{
			specification.Headless = true;
			// Nothing is shown, so nothing is rendered: no GPU is needed.
			specification.EnableGraphics = false;
		}
		if (test)
		{
			double const seconds = timestep.value_or(DefaultTestTimestep);
			runtime.Layer.Test = true;
			runtime.Layer.FixedTimestep = static_cast<float>(seconds);
			runtime.Layer.TestFrameLimit = static_cast<uint64_t>(std::ceil(timeout.value_or(DefaultTestTimeout) / seconds));
		}
		else
		{
			runtime.Layer.FixedTimestep = static_cast<float>(timestep.value_or(0.0));
			if (headless)
			{
				// Real-time pacing for headless runs that use the frame time.
				specification.FrameRateLimit = 60;
			}
		}

		if (std::optional<int64_t> const frames = arguments.GetInt("frames"); frames && *frames > 0)
		{
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		}
		if (screenshot)
		{
			// Captured on the last frame when --frames is given, otherwise once the game has run a moment.
			runtime.Layer.ScreenshotPath = FileSystem::PathFromUtf8(*screenshot);
			runtime.Layer.ScreenshotFrame = specification.MaxFrames > 0 ? specification.MaxFrames - 1 : 60;
		}
		if (std::optional<int64_t> const gpu = arguments.GetInt("gpu"); gpu && *gpu >= 0)
		{
			specification.GpuIndex = static_cast<int32_t>(*gpu);
		}
		if (arguments.HasFlag("validation"))
		{
			specification.EnableValidation = true;
		}
		if (arguments.HasFlag("no-validation"))
		{
			specification.EnableValidation = false;
		}
		return CreateScope<RuntimeApplication>(specification, std::move(runtime));
	}
}
