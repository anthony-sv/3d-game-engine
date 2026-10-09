#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/LayerStack.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/Window.h"

#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace Strada
{
	class Event;
	class ImGuiLayer;
	class Swapchain;
	class WindowCloseEvent;
	class WindowLostFocusEvent;
	class WindowMinimizeEvent;
	class WindowResizeEvent;

	struct ApplicationCommandLineArgs
	{
		int Count = 0;
		char** Args = nullptr;

		char const* operator[](int index) const
		{
			ST_CORE_ASSERT(index >= 0 && index < Count);
			return Args[index];
		}
	};

	struct ApplicationSpecification
	{
		std::string Name = "Strada Application";
		ApplicationCommandLineArgs CommandLineArgs;
		// Empty keeps the process working directory.
		std::filesystem::path WorkingDirectory;
		WindowSpecification Window;
		// No OS window or swapchain; rendering (when a GPU is available) is offscreen only.
		bool Headless = false;
		// Exit after this many frames (0 = run until closed). Used by tests and CI.
		uint64_t MaxFrames = 0;
		// Frame-rate cap applied by sleeping, for loops not paced by vsync such as headless runs (0 = uncapped).
		uint32_t FrameRateLimit = 0;

		// Create the GPU device (and, with a window, its swapchain). Disable for simulation-only runs without a GPU.
		bool EnableGraphics = true;
		// When false, a failure to initialize graphics is logged and the application runs without them (headless tools
		// that render offscreen only when a GPU is available).
		bool RequireGraphics = true;
		// Vulkan validation layers plus NVRHI validation.
#if defined(ST_DEBUG)
		bool EnableValidation = true;
#else
		bool EnableValidation = false;
#endif
		// GPU adapter index (-1 picks the best supported device).
		int32_t GpuIndex = -1;

		// ImGui overlay with docking and multi-viewports (requires a window and graphics).
		bool EnableImGui = false;
		bool EnableImGuiViewports = true;
		// Where the ImGui layout is saved; empty disables persistence.
		std::filesystem::path ImGuiIniFilePath;
	};

	// Owns the window, the layer stack and the main loop. Exactly one instance exists at a time.
	class Application
	{
	public:
		using ScreenshotCallback = std::function<void(Result<Image>)>;

		explicit Application(ApplicationSpecification specification);
		virtual ~Application();

		Application(Application const&) = delete;
		Application& operator=(Application const&) = delete;

		// Initializes the engine, calls OnInit, runs the main loop, then detaches all layers, calls OnShutdown and
		// shuts the engine down. Returns the process exit code. Call once, from the main thread.
		int Run();
		// Requests the main loop to exit after the current frame (or before the first frame when called from OnInit).
		void Close();

		void OnEvent(Event& event);

		// Layers must not be pushed or popped from inside layer callbacks; use SubmitToMainThread instead.
		template<typename T, typename... Args>
		T& PushLayer(Args&&... args)
		{
			static_assert(std::is_base_of_v<Layer, T>, "T must derive from Strada::Layer");
			ST_CORE_ASSERT(m_LayerIterationDepth == 0, "Layers cannot be pushed while the layer stack is being iterated");
			T& layer = static_cast<T&>(m_LayerStack.PushLayer(CreateScope<T>(std::forward<Args>(args)...)));
			layer.OnAttach();
			return layer;
		}

		template<typename T, typename... Args>
		T& PushOverlay(Args&&... args)
		{
			static_assert(std::is_base_of_v<Layer, T>, "T must derive from Strada::Layer");
			ST_CORE_ASSERT(m_LayerIterationDepth == 0, "Layers cannot be pushed while the layer stack is being iterated");
			T& layer = static_cast<T&>(m_LayerStack.PushOverlay(CreateScope<T>(std::forward<Args>(args)...)));
			layer.OnAttach();
			return layer;
		}

		// Detaches and destroys the layer.
		void PopLayer(Layer* layer);

		// Null when headless.
		Window* GetWindow() const { return m_Window.get(); }
		// Null when headless or graphics are disabled.
		Swapchain* GetSwapchain() const { return m_Swapchain.get(); }
		// The main window's framebuffer for the current frame; null when nothing is presented this frame.
		nvrhi::IFramebuffer* GetBackBuffer() const;
		// Null unless ApplicationSpecification::EnableImGui is set.
		ImGuiLayer* GetImGuiLayer() const { return m_ImGuiLayer; }

		void SetVSync(bool enabled);

		// Saves the main window's contents as a PNG right before the next present. Logs the outcome.
		void RequestScreenshot(std::filesystem::path path);
		// Captures the main window's contents (RGBA8) right before the next present and passes them to the callback on
		// the main thread. While the window is minimized the capture waits for the next presented frame. The callback
		// receives an error immediately when there is no window, and when the application closes before capturing.
		void RequestScreenshotImage(ScreenshotCallback callback);
		ApplicationSpecification const& GetSpecification() const { return m_Specification; }
		bool IsHeadless() const { return m_Specification.Headless; }

		uint64_t GetFrameCount() const { return m_FrameCount; }
		// Duration of the previous frame (clamped to avoid huge steps after stalls).
		Timestep GetFrameTime() const { return m_FrameTime; }
		// Seconds since the main loop started.
		double GetTime() const;

		int GetExitCode() const { return m_ExitCode; }
		void SetExitCode(int exitCode) { m_ExitCode = exitCode; }

		// Thread-safe: queues a function to run on the main thread at the start of the next frame.
		void SubmitToMainThread(std::function<void()> function);

		static Application& Get();
		static bool HasInstance() { return s_Instance != nullptr; }

	protected:
		// Called after engine initialization; push layers here. Returning an error exits with code 1.
		virtual Result<void> OnInit() { return {}; }
		// Called after all layers were detached, before engine shutdown (only if OnInit succeeded). Exceptions escaping
		// OnInit or a layer are logged and turn into exit code 1 after an orderly shutdown.
		virtual void OnShutdown() {}

	private:
		Result<void> InitializeEngine();
		void ShutdownEngine();
		void MainLoop();
		void ExecuteMainThreadQueue();
		void DetachAllLayers();
		void LimitFrameRate(double frameStartTime) const;
		Result<void> InitializeGraphics();
		void ShutdownGraphics();
		void ClearBackBuffer();
		void CaptureScreenshot();
		void CancelPendingScreenshots();

		bool OnWindowClose(WindowCloseEvent& event);
		bool OnWindowResize(WindowResizeEvent& event);
		bool OnWindowMinimize(WindowMinimizeEvent& event);
		bool OnWindowLostFocus(WindowLostFocusEvent& event);

		ApplicationSpecification m_Specification;
		Scope<Window> m_Window;
		Scope<Swapchain> m_Swapchain;
		nvrhi::CommandListHandle m_FrameCommandList;
		ImGuiLayer* m_ImGuiLayer = nullptr;
		bool m_BackBufferAvailable = false;
		std::vector<ScreenshotCallback> m_PendingScreenshots;
		LayerStack m_LayerStack;
		uint32_t m_LayerIterationDepth = 0;

		bool m_Running = false;
		bool m_CloseRequested = false;
		bool m_Minimized = false;
		bool m_HasRun = false;
		int m_ExitCode = 0;

		uint64_t m_FrameCount = 0;
		Timestep m_FrameTime;
		double m_StartTime = 0.0;

		std::mutex m_MainThreadQueueMutex;
		std::vector<std::function<void()>> m_MainThreadQueue;

		static Application* s_Instance;
	};

	// Implemented by the client application (editor, runtime) and called by the entry point. Returning null exits
	// immediately with exitCode (e.g. after printing --help or reporting invalid arguments).
	Scope<Application> CreateApplication(ApplicationCommandLineArgs args, int& exitCode);

	// Process entry used by EntryPoint.h: initializes logging, creates and runs the application, returns its exit code.
	int ApplicationMain(int argc, char** argv);
}
