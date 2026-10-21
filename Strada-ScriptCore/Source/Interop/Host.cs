using System;
using System.Runtime.InteropServices;
using Strada.VisualScripting;

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

	// The process-wide visual-script runtime (spec section 5.1): the graph instances of the running scene and the programs
	// of their graphs. Initialize and Shutdown replace it with an empty one, since the scripting runtime outlives the
	// engine's Shutdown.
	internal static VisualScriptRuntime VisualScripts { get; private set; } = new(new EngineEnvironment());

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
			ResetVisualScripts();
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
			ResetVisualScripts();
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

	// Sets fields of the entity's instance from stored values ({ "Name": { "Type", "Value" } }), as CreateInstance does.
	[UnmanagedCallersOnly]
	internal static int SetInstanceFields(ulong entity, byte* fields, int fieldsLength)
	{
		try
		{
			ReadOnlySpan<byte> fieldsJson = fields != null ? new ReadOnlySpan<byte>(fields, fieldsLength) : [];
			return ScriptRegistry.SetInstanceFields(entity, fieldsJson);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Passes the values the fields of the entity's instance hold, in the stored form ({ "Name": { "Type", "Value" } }), to
	// receive(context, bytes, length).
	[UnmanagedCallersOnly]
	internal static int DescribeInstanceFields(ulong entity, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			byte[]? description = ScriptRegistry.DescribeInstanceFields(entity);
			if (description == null)
			{
				return NotFound;
			}
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

	// Visual scripts (spec section 6): thin adapters over the runtime's operations. Strings and documents are UTF-8, borrowed
	// for the call; receive(context, bytes, length) gets the result exactly once, when the status is Success.

	// Passes the node library JSON to receive.
	[UnmanagedCallersOnly]
	internal static int VisualScriptDescribeLibrary(void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			return Deliver(VisualScripts.DescribeLibrary(), context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Passes the analysis JSON of the document (any bytes) to receive.
	[UnmanagedCallersOnly]
	internal static int VisualScriptAnalyze(byte* document, int documentLength, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			return Deliver(VisualScripts.AnalyzeDocument(Bytes(document, documentLength)), context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Stores a program of the document for the graph handle and passes the diagnostics array to receive.
	[UnmanagedCallersOnly]
	internal static int VisualScriptLoad(ulong graph, byte* name, int nameLength, byte* document, int documentLength, void* context,
		delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			HostReply reply = VisualScripts.LoadDocument(graph, NativeString.FromUtf8(name, nameLength), Bytes(document, documentLength));
			return Deliver(reply, context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int VisualScriptUnload(ulong graph)
	{
		try
		{
			return (int)VisualScripts.Unload(graph);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Creates the entity's graph instance with the component's variable overrides ({ "Name": { "Type", "Value" } }; none
	// when empty).
	[UnmanagedCallersOnly]
	internal static int VisualScriptCreateInstance(ulong entity, ulong graph, byte* variables, int variablesLength)
	{
		try
		{
			return (int)VisualScripts.CreateInstance(entity, graph, Bytes(variables, variablesLength));
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int VisualScriptDestroyInstance(ulong entity)
	{
		try
		{
			return (int)VisualScripts.DestroyInstance(entity);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int VisualScriptDestroyAllInstances()
	{
		try
		{
			VisualScripts.DestroyAllInstances();
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static int VisualScriptInvoke(ulong entity, int scriptEvent, float timeStep, ulong other)
	{
		try
		{
			return (int)VisualScripts.Invoke(entity, (ScriptEvent)scriptEvent, timeStep, other);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Passes the public variables of the entity's graph instance with their current values ({ "Name": { "Type", "Value" } })
	// to receive.
	[UnmanagedCallersOnly]
	internal static int VisualScriptDescribeInstanceVariables(ulong entity, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			return Deliver(VisualScripts.DescribeVariables(entity), context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Sets public variables of the entity's graph instance from stored values, as VisualScriptCreateInstance does.
	[UnmanagedCallersOnly]
	internal static int VisualScriptSetInstanceVariables(ulong entity, byte* variables, int variablesLength)
	{
		try
		{
			return (int)VisualScripts.SetInstanceVariables(entity, Bytes(variables, variablesLength));
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	private static void ResetVisualScripts()
	{
		VisualScripts = new VisualScriptRuntime(new EngineEnvironment());
		EngineLibrary.Drop();
	}

	private static ReadOnlySpan<byte> Bytes(byte* data, int length) => data != null && length > 0 ? new ReadOnlySpan<byte>(data, length) : [];

	// The reply's text goes to receive exactly when its status is Success.
	private static int Deliver(HostReply reply, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		if (reply.Status == VisualScriptStatus.Success)
		{
			byte[] text = reply.Text!;
			fixed (byte* bytes = text)
			{
				receive(context, bytes, text.Length);
			}
		}
		return (int)reply.Status;
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
