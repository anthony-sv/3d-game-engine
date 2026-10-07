#include "Strada/Core/Application.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Input.h"

#include <doctest/doctest.h>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Strada;

namespace
{
	struct LayerLog
	{
		std::vector<std::string> Calls;
		uint64_t Updates = 0;
	};

	class RecordingLayer : public Layer
	{
	public:
		RecordingLayer(std::string name, LayerLog& log, bool consumeEvents)
			: Layer(std::move(name)),
			  m_Log(log),
			  m_ConsumeEvents(consumeEvents)
		{
		}

		void OnAttach() override { m_Log.Calls.push_back(GetName() + ":Attach"); }
		void OnDetach() override { m_Log.Calls.push_back(GetName() + ":Detach"); }
		void OnUpdate(Timestep timestep) override
		{
			CHECK(timestep.GetSeconds() >= 0.0f);
			m_Log.Updates++;
		}
		void OnEvent(Event& event) override
		{
			m_Log.Calls.push_back(GetName() + ":Event");
			event.Handled = m_ConsumeEvents;
		}

	private:
		LayerLog& m_Log;
		bool m_ConsumeEvents;
	};

	class TestApplication : public Application
	{
	public:
		TestApplication(ApplicationSpecification const& specification, LayerLog& log, bool failInit = false)
			: Application(specification),
			  m_Log(log),
			  m_FailInit(failInit)
		{
		}

		bool ShutdownCalled = false;

	protected:
		Result<void> OnInit() override
		{
			PushLayer<RecordingLayer>("Game", m_Log, false);
			PushOverlay<RecordingLayer>("Overlay", m_Log, false);
			if (m_FailInit)
			{
				return Error{"intentional failure"};
			}
			return {};
		}

		void OnShutdown() override { ShutdownCalled = true; }

	private:
		LayerLog& m_Log;
		bool m_FailInit;
	};

	ApplicationSpecification HeadlessSpecification(uint64_t frames)
	{
		ApplicationSpecification specification;
		specification.Name = "Test Application";
		specification.Headless = true;
		specification.MaxFrames = frames;
		return specification;
	}
}

TEST_CASE("Application: headless run updates layers for the requested number of frames")
{
	LayerLog log;
	TestApplication application(HeadlessSpecification(5), log);
	CHECK(Application::HasInstance());
	CHECK(&Application::Get() == &application);

	int const exitCode = application.Run();
	CHECK(exitCode == 0);
	CHECK(application.GetFrameCount() == 5);
	CHECK(log.Updates == 10);
	CHECK(application.ShutdownCalled);
	CHECK(application.GetWindow() == nullptr);

	// Layers attach in push order and detach overlays-first.
	REQUIRE(log.Calls.size() == 4);
	CHECK(log.Calls[0] == "Game:Attach");
	CHECK(log.Calls[1] == "Overlay:Attach");
	CHECK(log.Calls[2] == "Overlay:Detach");
	CHECK(log.Calls[3] == "Game:Detach");
}

TEST_CASE("Application: failing OnInit exits with an error and detaches layers")
{
	LayerLog log;
	TestApplication application(HeadlessSpecification(5), log, true);
	CHECK(application.Run() == 1);
	CHECK(log.Updates == 0);
	CHECK_FALSE(application.ShutdownCalled);
	REQUIRE(log.Calls.size() == 4);
	CHECK(log.Calls[3] == "Game:Detach");
}

TEST_CASE("Application: only one instance may exist and it is released on destruction")
{
	{
		LayerLog log;
		TestApplication application(HeadlessSpecification(1), log);
		CHECK(Application::HasInstance());
		CHECK(application.Run() == 0);
	}
	CHECK_FALSE(Application::HasInstance());
}

namespace
{
	class QueueApplication : public Application
	{
	public:
		explicit QueueApplication(ApplicationSpecification const& specification)
			: Application(specification)
		{
		}

		std::atomic<int> Executed = 0;
		std::thread::id ExecutionThread;

	protected:
		Result<void> OnInit() override
		{
			// Work submitted from another thread before the loop starts runs on the main thread.
			std::thread worker(
				[this]()
				{
					for (int i = 0; i < 10; i++)
					{
						SubmitToMainThread(
							[this]()
							{
								Executed++;
								ExecutionThread = std::this_thread::get_id();
							});
					}
				});
			worker.join();
			return {};
		}
	};
}

TEST_CASE("Application: work submitted from other threads runs on the main thread")
{
	QueueApplication application(HeadlessSpecification(2));
	CHECK(application.Run() == 0);
	CHECK(application.Executed == 10);
	CHECK(application.ExecutionThread == std::this_thread::get_id());
}

namespace
{
	class CloseApplication : public Application
	{
	public:
		explicit CloseApplication(ApplicationSpecification const& specification)
			: Application(specification)
		{
		}

	protected:
		Result<void> OnInit() override
		{
			SubmitToMainThread(
				[this]()
				{
					SetExitCode(7);
					Close();
				});
			return {};
		}
	};
}

TEST_CASE("Application: Close stops the loop and the exit code is returned")
{
	CloseApplication application(HeadlessSpecification(1000));
	CHECK(application.Run() == 7);
	CHECK(application.GetFrameCount() == 1);
}

namespace
{
	// The top overlay consumes every event; the bottom layer passes events through.
	class PropagationApplication : public Application
	{
	public:
		PropagationApplication(ApplicationSpecification const& specification, LayerLog& log, bool topConsumes)
			: Application(specification),
			  m_Log(log),
			  m_TopConsumes(topConsumes)
		{
		}

		bool KeyDownAfterPress = false;
		bool KeyDownAfterRelease = true;

	protected:
		Result<void> OnInit() override
		{
			PushLayer<RecordingLayer>("Bottom", m_Log, false);
			PushOverlay<RecordingLayer>("Top", m_Log, m_TopConsumes);
			SubmitToMainThread(
				[this]()
				{
					KeyPressedEvent press(KeyCode::G, false);
					OnEvent(press);
					KeyDownAfterPress = Input::IsKeyDown(KeyCode::G);

					KeyReleasedEvent release(KeyCode::G);
					OnEvent(release);
					KeyDownAfterRelease = Input::IsKeyDown(KeyCode::G);
				});
			return {};
		}

	private:
		LayerLog& m_Log;
		bool m_TopConsumes;
	};

	int CountCalls(LayerLog const& log, std::string const& call)
	{
		int count = 0;
		for (std::string const& entry : log.Calls)
		{
			count += entry == call ? 1 : 0;
		}
		return count;
	}
}

TEST_CASE("Application: a consumed event stops propagating and never reaches Input")
{
	LayerLog log;
	PropagationApplication application(HeadlessSpecification(2), log, true);
	CHECK(application.Run() == 0);

	CHECK(CountCalls(log, "Top:Event") == 2);
	CHECK(CountCalls(log, "Bottom:Event") == 0);
	CHECK_FALSE(application.KeyDownAfterPress);
	CHECK_FALSE(application.KeyDownAfterRelease);
}

TEST_CASE("Application: unconsumed events reach every layer and update Input")
{
	LayerLog log;
	PropagationApplication application(HeadlessSpecification(2), log, false);
	CHECK(application.Run() == 0);

	CHECK(CountCalls(log, "Top:Event") == 2);
	CHECK(CountCalls(log, "Bottom:Event") == 2);
	CHECK(application.KeyDownAfterPress);
	CHECK_FALSE(application.KeyDownAfterRelease);
}

namespace
{
	class CloseInInitApplication : public Application
	{
	public:
		explicit CloseInInitApplication(ApplicationSpecification const& specification)
			: Application(specification)
		{
		}

		bool ShutdownCalled = false;

	protected:
		Result<void> OnInit() override
		{
			Close();
			return {};
		}

		void OnShutdown() override { ShutdownCalled = true; }
	};

	class ThrowingLayer : public Layer
	{
	public:
		explicit ThrowingLayer(LayerLog& log)
			: Layer("Throwing"),
			  m_Log(log)
		{
		}

		void OnDetach() override { m_Log.Calls.push_back("Throwing:Detach"); }
		void OnUpdate(Timestep) override { throw std::runtime_error("layer failure"); }

	private:
		LayerLog& m_Log;
	};

	class ThrowingApplication : public Application
	{
	public:
		ThrowingApplication(ApplicationSpecification const& specification, LayerLog& log)
			: Application(specification),
			  m_Log(log)
		{
		}

		bool ShutdownCalled = false;

	protected:
		Result<void> OnInit() override
		{
			PushLayer<ThrowingLayer>(m_Log);
			return {};
		}

		void OnShutdown() override { ShutdownCalled = true; }

	private:
		LayerLog& m_Log;
	};
}

TEST_CASE("Application: Close during OnInit exits before the first frame")
{
	CloseInInitApplication application(HeadlessSpecification(100));
	CHECK(application.Run() == 0);
	CHECK(application.GetFrameCount() == 0);
	CHECK(application.ShutdownCalled);
}

TEST_CASE("Application: an exception escaping a layer shuts down in order with exit code 1")
{
	LayerLog log;
	ThrowingApplication application(HeadlessSpecification(100), log);
	CHECK(application.Run() == 1);
	CHECK(application.ShutdownCalled);
	REQUIRE(log.Calls.size() == 1);
	CHECK(log.Calls[0] == "Throwing:Detach");
}
