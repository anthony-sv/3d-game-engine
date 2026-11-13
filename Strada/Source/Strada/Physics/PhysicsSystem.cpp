#include "stpch.h"
#include "Strada/Physics/PhysicsSystem.h"

#include "Strada/Core/Profiler.h"
#include "Strada/Physics/JoltContext.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Profiler.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

// Jolt's own zones (JPH_PROFILE) recorded by the engine's profiler (Docs/Architecture.md §4.4). Jolt defines
// JPH_EXTERNAL_PROFILE in Debug builds only (cmake/StradaDependencies.cmake) and leaves the measurement's constructor and
// destructor to the application. They run on the main thread and on Jolt's workers, so they only begin and end a zone,
// keeping its token in the measurement's 64 bytes of user data; Jolt's zone names are string literals.
#ifdef JPH_EXTERNAL_PROFILE

static_assert(sizeof(Strada::ProfilerZoneToken) <= 64, "the token lives in ExternalProfileMeasurement's user data");

JPH_NAMESPACE_BEGIN

ExternalProfileMeasurement::ExternalProfileMeasurement(char const* inName, uint32 inColor)
{
	static_cast<void>(inColor);
	Strada::ProfilerZoneToken const token = Strada::Profiler::BeginZone(inName);
	std::memcpy(mUserData, &token, sizeof(token));
}

ExternalProfileMeasurement::~ExternalProfileMeasurement()
{
	Strada::ProfilerZoneToken token;
	std::memcpy(&token, mUserData, sizeof(token));
	Strada::Profiler::EndZone(token);
}

JPH_NAMESPACE_END

#endif

namespace Strada
{
	namespace
	{
		// The profiler's name of Jolt's worker threads.
		constexpr char const* JoltWorkerThreadName = "Jolt Worker";

		struct PhysicsSystemData
		{
			Scope<JPH::Factory> Factory;
			Scope<JPH::JobSystemThreadPool> JobSystem;
		};

		Scope<PhysicsSystemData> s_Data;

		ST_PRINTF_FORMAT(1, 2) void TraceToLog(char const* format, ...)
		{
			va_list arguments;
			va_start(arguments, format);
			char message[1024];
			std::vsnprintf(message, sizeof(message), format, arguments);
			va_end(arguments);
			ST_CORE_WARN("Jolt: {}", message);
		}

#ifdef JPH_ENABLE_ASSERTS
		bool AssertFailedToLog(char const* expression, char const* message, char const* file, JPH::uint line)
		{
			ST_CORE_ERROR("Jolt assertion failed: {} ({}) at {}:{}", expression, message != nullptr ? message : "", file, line);
			// Break into the debugger: a failed Jolt assertion is a programming error.
			return true;
		}
#endif
	}

	void PhysicsSystem::Init()
	{
		ST_CORE_ASSERT(!s_Data, "PhysicsSystem is already initialized");
		JPH::RegisterDefaultAllocator();
		JPH::Trace = TraceToLog;
		JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedToLog;)

		s_Data = CreateScope<PhysicsSystemData>();
		s_Data->Factory = CreateScope<JPH::Factory>();
		JPH::Factory::sInstance = s_Data->Factory.get();
		JPH::RegisterTypes();

		// Leave a core for the main thread, which waits for the steps it starts.
		int const workers = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()) - 1);
		s_Data->JobSystem = CreateScope<JPH::JobSystemThreadPool>();
		// The workers share one name in the profiler, which merges their busy time; each ends in it when it exits, freeing
		// its buffer. Set before Init, which starts the threads.
		s_Data->JobSystem->SetThreadInitFunction(
			[](int)
			{
				Profiler::SetThreadName(JoltWorkerThreadName);
			});
		s_Data->JobSystem->SetThreadExitFunction(
			[](int)
			{
				Profiler::EndThread();
			});
		s_Data->JobSystem->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::max(workers, 1));
		ST_CORE_INFO("Physics initialized (Jolt {}.{}.{}, {} worker threads)", JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
		             std::max(workers, 1));
	}

	void PhysicsSystem::Shutdown()
	{
		ST_CORE_ASSERT(s_Data, "PhysicsSystem is not initialized");
		s_Data->JobSystem.reset();
		JPH::UnregisterTypes();
		JPH::Factory::sInstance = nullptr;
		s_Data.reset();
	}

	bool PhysicsSystem::IsInitialized()
	{
		return s_Data != nullptr;
	}

	JPH::JobSystem& Jolt::GetJobSystem()
	{
		ST_CORE_ASSERT(s_Data, "PhysicsSystem is not initialized");
		return *s_Data->JobSystem;
	}
}
