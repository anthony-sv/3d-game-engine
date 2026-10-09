#include "Editor/EditorLayer.h"

#include "Strada/Core/CommandLine.h"
#include "Strada/Core/EntryPoint.h"
#include "Strada/Core/FileSystem.h"

#include <cstdio>

namespace Strada
{
	class EditorApplication : public Application
	{
	public:
		EditorApplication(ApplicationSpecification const& specification, EditorLayerSpecification editorSpecification,
		                  std::filesystem::path screenshotPath)
			: Application(specification),
			  m_EditorSpecification(std::move(editorSpecification)),
			  m_ScreenshotPath(std::move(screenshotPath))
		{
		}

	protected:
		Result<void> OnInit() override
		{
			EditorLayer& editorLayer = PushLayer<EditorLayer>(m_EditorSpecification);
			if (!m_ScreenshotPath.empty())
			{
				// Captured on the last frame when --frames is given, otherwise once the UI has settled.
				uint64_t const maxFrames = GetSpecification().MaxFrames;
				editorLayer.RequestScreenshotAtFrame(m_ScreenshotPath, maxFrames > 0 ? maxFrames - 1 : 60);
			}
			return {};
		}

	private:
		EditorLayerSpecification m_EditorSpecification;
		std::filesystem::path m_ScreenshotPath;
	};

	Scope<Application> CreateApplication(ApplicationCommandLineArgs args, int& exitCode)
	{
		CommandLineParser parser("Strada Editor");
		parser.AddFlag("help", "Show this help and exit")
			.AddOption("frames", "count", "Exit after the given number of frames")
			.AddOption("gpu", "index", "Use the GPU with this adapter index")
			.AddFlag("validation", "Enable Vulkan and NVRHI validation")
			.AddFlag("no-validation", "Disable Vulkan and NVRHI validation")
			.AddFlag("no-vsync", "Present without waiting for vertical sync")
			.AddOption("screenshot", "path", "Save a PNG of the editor window (on the last frame with --frames)")
			.AddFlag("headless", "Run without a window or UI (automation and offscreen use; GPU optional)")
			.AddOption("automation-port", "port", "Port of the automation server on 127.0.0.1 (default: any free port)")
			.AddFlag("no-automation", "Do not start the automation server")
			.AddOption("project", "path", "Open this project file (.sproj) at startup")
			.AddOption("scene", "path", "Open this scene file at startup (after the project)");

		Result<CommandLineArguments> parsed = parser.Parse(args.Count, args.Args);
		if (!parsed)
		{
			std::fprintf(stderr, "%s\n\n%s", parsed.GetError().c_str(), parser.GetHelpText("StradaEditor").c_str());
			exitCode = 2;
			return nullptr;
		}
		CommandLineArguments const& arguments = parsed.GetValue();
		if (arguments.HasFlag("help"))
		{
			std::printf("%s", parser.GetHelpText("StradaEditor").c_str());
			exitCode = 0;
			return nullptr;
		}

		ApplicationSpecification specification;
		specification.Name = "Strada Editor";
		specification.CommandLineArgs = args;
		specification.Window.Title = "Strada Editor";
		specification.Window.Width = 1600;
		specification.Window.Height = 900;
		specification.Window.VSync = !arguments.HasFlag("no-vsync");
		specification.EnableImGui = true;
		specification.ImGuiIniFilePath = FileSystem::GetUserDataDirectory() / "Editor" / "imgui.ini";
		if (arguments.HasFlag("headless"))
		{
			specification.Headless = true;
			specification.EnableImGui = false;
			// Rendering is offscreen when a GPU exists; automation keeps working without one.
			specification.RequireGraphics = false;
			specification.FrameRateLimit = 60;
		}

		EditorLayerSpecification editorSpecification;
		editorSpecification.EnableAutomation = !arguments.HasFlag("no-automation");
		if (std::optional<int64_t> const port = arguments.GetInt("automation-port"))
		{
			if (*port < 0 || *port > 65535)
			{
				std::fprintf(stderr, "--automation-port must be between 0 and 65535\n");
				exitCode = 2;
				return nullptr;
			}
			editorSpecification.AutomationPort = static_cast<uint16_t>(*port);
		}
		if (std::optional<std::string> const project = arguments.GetValue("project"))
		{
			editorSpecification.ProjectPath = FileSystem::PathFromUtf8(*project);
		}
		if (std::optional<std::string> const scene = arguments.GetValue("scene"))
		{
			editorSpecification.ScenePath = FileSystem::PathFromUtf8(*scene);
		}

		if (std::optional<int64_t> const frames = arguments.GetInt("frames"); frames && *frames > 0)
		{
			specification.MaxFrames = static_cast<uint64_t>(*frames);
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

		std::filesystem::path screenshotPath;
		if (std::optional<std::string> const screenshot = arguments.GetValue("screenshot"))
		{
			screenshotPath = FileSystem::PathFromUtf8(*screenshot);
		}
		return CreateScope<EditorApplication>(specification, std::move(editorSpecification), std::move(screenshotPath));
	}
}
