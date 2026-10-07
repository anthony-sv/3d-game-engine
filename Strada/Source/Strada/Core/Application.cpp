#include "stpch.h"
#include "Strada/Core/Application.h"

#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Platform.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace Strada
{
	Application* Application::s_Instance = nullptr;

	namespace
	{
		// Frame deltas are clamped so a stall (breakpoint, window drag, loading) does not produce a huge timestep.
		constexpr double MaxFrameTime = 0.25;
	}

	Application::Application(ApplicationSpecification specification)
		: m_Specification(std::move(specification))
	{
		ST_CORE_ASSERT(s_Instance == nullptr, "Only one Application may exist at a time");
		s_Instance = this;
	}

	Application::~Application()
	{
		ST_CORE_ASSERT(m_LayerStack.IsEmpty(), "Layers must be detached before the Application is destroyed");
		s_Instance = nullptr;
	}

	Application& Application::Get()
	{
		ST_CORE_ASSERT(s_Instance != nullptr, "No Application instance exists");
		return *s_Instance;
	}

	int Application::Run()
	{
		ST_CORE_ASSERT(!m_HasRun, "Application::Run may only be called once");
		m_HasRun = true;

		if (Result<void> result = InitializeEngine(); !result)
		{
			ST_CORE_CRITICAL("Engine initialization failed: {}", result.GetError());
			ShutdownEngine();
			return 1;
		}

		// Engine code does not throw, but the standard library and third-party code can. Exceptions are contained here so
		// the application always shuts down in order and the failure is logged.
		bool initialized = false;
		try
		{
			if (Result<void> result = OnInit(); !result)
			{
				ST_CORE_CRITICAL("Application initialization failed: {}", result.GetError());
				m_ExitCode = 1;
			}
			else
			{
				initialized = true;
				MainLoop();
			}
		}
		catch (std::exception const& exception)
		{
			ST_CORE_CRITICAL("Unhandled exception: {}", exception.what());
			m_ExitCode = 1;
		}

		DetachAllLayers();
		if (initialized)
		{
			OnShutdown();
		}
		ShutdownEngine();
		return m_ExitCode;
	}

	void Application::Close()
	{
		m_CloseRequested = true;
	}

	Result<void> Application::InitializeEngine()
	{
		ST_CORE_INFO("Initializing {}", m_Specification.Name);

		if (!m_Specification.WorkingDirectory.empty())
		{
			std::error_code errorCode;
			std::filesystem::current_path(m_Specification.WorkingDirectory, errorCode);
			if (errorCode)
			{
				return MakeError("Failed to set the working directory to '{}': {}",
				                 FileSystem::PathToUtf8(m_Specification.WorkingDirectory), errorCode.message());
			}
		}

		Input::Reset();

		if (!m_Specification.Headless)
		{
			Result<Scope<Window>> window = Window::Create(m_Specification.Window);
			if (!window)
			{
				return Error{window.GetError()};
			}
			m_Window = window.TakeValue();
			m_Window->SetEventCallback(ST_BIND_EVENT_FN(OnEvent));

			Window* windowPointer = m_Window.get();
			Input::SetCursorModeHandler(
				[windowPointer](CursorMode mode)
				{
					windowPointer->SetCursorMode(mode);
				});
		}

		return {};
	}

	void Application::ShutdownEngine()
	{
		Input::SetCursorModeHandler(nullptr);
		Input::Reset();
		m_Window.reset();
		ST_CORE_INFO("Shut down {}", m_Specification.Name);
	}

	void Application::MainLoop()
	{
		m_Running = true;
		m_StartTime = Platform::GetTime();
		double lastFrameTime = m_StartTime;

		// Close() may already have been requested from OnInit; the loop then exits without running a frame.
		while (!m_CloseRequested)
		{
			double const frameStartTime = Platform::GetTime();
			m_FrameTime = Timestep(static_cast<float>(std::min(frameStartTime - lastFrameTime, MaxFrameTime)));
			lastFrameTime = frameStartTime;

			if (m_Window)
			{
				if (m_Minimized)
				{
					// Sleep while minimized, but wake regularly so queued main-thread work keeps flowing.
					Window::WaitEvents(0.05);
				}
				else
				{
					Window::PollEvents();
				}
			}

			ExecuteMainThreadQueue();

			if (!m_Minimized)
			{
				m_LayerIterationDepth++;
				for (Scope<Layer>& layer : m_LayerStack)
				{
					layer->OnUpdate(m_FrameTime);
				}
				m_LayerIterationDepth--;
			}

			Input::EndFrame();

			m_FrameCount++;
			if (m_Specification.MaxFrames != 0 && m_FrameCount >= m_Specification.MaxFrames)
			{
				m_CloseRequested = true;
			}

			LimitFrameRate(frameStartTime);
		}
		m_Running = false;
	}

	void Application::OnEvent(Event& event)
	{
		// Window state is tracked before layers see the event so it is always correct.
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>(ST_BIND_EVENT_FN(OnWindowResize));
		dispatcher.Dispatch<WindowMinimizeEvent>(ST_BIND_EVENT_FN(OnWindowMinimize));
		dispatcher.Dispatch<WindowLostFocusEvent>(ST_BIND_EVENT_FN(OnWindowLostFocus));

		m_LayerIterationDepth++;
		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
		{
			if (event.Handled)
			{
				break;
			}
			(*it)->OnEvent(event);
		}
		m_LayerIterationDepth--;

		// A layer (e.g. the editor with unsaved changes) may veto closing by handling the event.
		if (!event.Handled)
		{
			dispatcher.Dispatch<WindowCloseEvent>(ST_BIND_EVENT_FN(OnWindowClose));
		}

		// Input sees events no layer consumed (e.g. not typed into a UI text field). Releases and cursor movement are
		// always applied so keys never get stuck and deltas stay continuous.
		EventType const type = event.GetEventType();
		bool const alwaysApply = type == EventType::KeyReleased || type == EventType::MouseButtonReleased || type == EventType::MouseMoved;
		if (!event.Handled || alwaysApply)
		{
			Input::ProcessEvent(event);
		}
	}

	void Application::PopLayer(Layer* layer)
	{
		ST_CORE_ASSERT(m_LayerIterationDepth == 0, "Layers cannot be popped while the layer stack is being iterated");
		Scope<Layer> removed = m_LayerStack.Remove(layer);
		if (removed)
		{
			removed->OnDetach();
		}
	}

	double Application::GetTime() const
	{
		return m_Running ? Platform::GetTime() - m_StartTime : 0.0;
	}

	void Application::SubmitToMainThread(std::function<void()> function)
	{
		std::lock_guard<std::mutex> lock(m_MainThreadQueueMutex);
		m_MainThreadQueue.push_back(std::move(function));
	}

	void Application::ExecuteMainThreadQueue()
	{
		std::vector<std::function<void()>> queue;
		{
			std::lock_guard<std::mutex> lock(m_MainThreadQueueMutex);
			queue.swap(m_MainThreadQueue);
		}

		for (std::function<void()>& function : queue)
		{
			function();
		}
	}

	void Application::DetachAllLayers()
	{
		for (Scope<Layer>& layer : m_LayerStack.RemoveAll())
		{
			layer->OnDetach();
		}
	}

	void Application::LimitFrameRate(double frameStartTime) const
	{
		if (m_Specification.FrameRateLimit == 0 || m_CloseRequested)
		{
			return;
		}

		double const targetFrameTime = 1.0 / static_cast<double>(m_Specification.FrameRateLimit);
		double const remaining = targetFrameTime - (Platform::GetTime() - frameStartTime);
		if (remaining > 0.0)
		{
			std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
		}
	}

	bool Application::OnWindowClose(WindowCloseEvent& event)
	{
		(void)event;
		Close();
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& event)
	{
		m_Minimized = event.GetWidth() == 0 || event.GetHeight() == 0;
		return false;
	}

	bool Application::OnWindowMinimize(WindowMinimizeEvent& event)
	{
		m_Minimized = event.IsMinimized();
		return false;
	}

	bool Application::OnWindowLostFocus(WindowLostFocusEvent& event)
	{
		(void)event;
		// Key-up events are not delivered while unfocused; release everything so nothing stays held.
		Input::ReleaseAll();
		return false;
	}
}
