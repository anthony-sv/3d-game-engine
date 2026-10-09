#include "stpch.h"
#include "Strada/Physics/PhysicsSystem.h"

#include "Strada/Physics/JoltContext.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <thread>

namespace Strada
{
	namespace
	{
		struct PhysicsSystemData
		{
			Scope<JPH::Factory> Factory;
			Scope<JPH::JobSystemThreadPool> JobSystem;
		};

		Scope<PhysicsSystemData> s_Data;

		void TraceToLog(char const* format, ...)
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
		s_Data->JobSystem = CreateScope<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::max(workers, 1));
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
