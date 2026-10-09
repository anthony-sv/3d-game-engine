#pragma once

namespace Strada
{
	// Jolt's global state shared by every PhysicsScene: the allocator hooks, the type factory and the worker threads
	// that run simulation steps. Initialized by Application after the AssetManager. Main thread only.
	class PhysicsSystem
	{
	public:
		static void Init();
		static void Shutdown();
		static bool IsInitialized();
	};
}
