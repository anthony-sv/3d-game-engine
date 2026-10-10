using System;
using System.Runtime.InteropServices;

namespace Strada.Interop;

// The engine's entry points into the scripting runtime (ScriptEngine.cpp resolves them by name). Exceptions never
// cross into native code: every entry point catches everything, logs it and returns a status.
internal static unsafe class Host
{
	[StructLayout(LayoutKind.Sequential)]
	internal struct NativeBinding
	{
		public byte* Name;
		public int NameLength;
		public void* Function;
	}

	// Statuses; the values are the engine's ScriptStatus.
	internal const int Success = 0;
	internal const int Failure = 1;
	internal const int NotFound = 2;
	internal const int BindingMismatch = 3;

	[UnmanagedCallersOnly]
	internal static int Initialize(NativeBinding* bindings, int bindingCount)
	{
		try
		{
			string? mismatch = InternalCalls.Bind(bindings, bindingCount);
			if (mismatch != null)
			{
				// Without the logging binding the mismatch cannot be reported; the engine names the likely cause.
				if (InternalCalls.Log_Write != null)
				{
					Log.Error(mismatch);
				}
				return BindingMismatch;
			}
			ScriptRegistry.UnloadGameAssembly();
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int Shutdown()
	{
		try
		{
			ScriptRegistry.UnloadGameAssembly();
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int LoadGameAssembly(byte* path, int pathLength)
	{
		try
		{
			ScriptRegistry.LoadGameAssembly(NativeString.FromUtf8(path, pathLength));
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int UnloadGameAssembly()
	{
		try
		{
			ScriptRegistry.UnloadGameAssembly();
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Passes the JSON description of the script classes to receive(context, bytes, length).
	[UnmanagedCallersOnly]
	internal static int DescribeClasses(void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			byte[] description = ScriptRegistry.DescribeClasses();
			fixed (byte* bytes = description)
			{
				receive(context, bytes, description.Length);
			}
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int CreateInstance(byte* className, int classNameLength, ulong entity, byte* fields, int fieldsLength)
	{
		try
		{
			ReadOnlySpan<byte> fieldsJson = fields != null ? new ReadOnlySpan<byte>(fields, fieldsLength) : [];
			return ScriptRegistry.CreateInstance(NativeString.FromUtf8(className, classNameLength), entity, fieldsJson);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int DestroyInstance(ulong entity)
	{
		try
		{
			return ScriptRegistry.DestroyInstance(entity) ? Success : NotFound;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int DestroyAllInstances()
	{
		try
		{
			ScriptRegistry.DestroyAllInstances();
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int Invoke(ulong entity, int scriptEvent, float timeStep, ulong other)
	{
		try
		{
			return ScriptRegistry.Invoke(entity, (ScriptEvent)scriptEvent, timeStep, other);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	private static int Report(Exception exception)
	{
		try
		{
			Log.Error($"Scripting runtime: {exception}");
		}
		catch (Exception)
		{
			// The log itself failed; the status still tells the engine.
		}
		return Failure;
	}
}
