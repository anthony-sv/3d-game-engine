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
			DataAssets.Reset();
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
			DataAssets.Reset();
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

	// The engine's data assets may have changed (its epoch moved since it last said so): the copies of their data that scripts
	// read (a curve's keys) ask the engine again on their next use. The engine calls it before scripts run.
	[UnmanagedCallersOnly]
	internal static int InvalidateDataAssets()
	{
		try
		{
			DataAssets.Invalidate();
			return Success;
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

	// Passes the analysis JSON of the document (any bytes) to receive. references is the bundle of the other graphs the
	// document's nodes refer to (any bytes; a length of 0 means none).
	[UnmanagedCallersOnly]
	internal static int VisualScriptAnalyze(byte* document, int documentLength, byte* references, int referencesLength, void* context,
		delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			return Deliver(VisualScripts.AnalyzeDocument(Bytes(document, documentLength), Bytes(references, referencesLength)), context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// Stores a program of the document for the graph handle and passes the diagnostics array to receive. references is the
	// bundle of the other graphs the document's nodes refer to (any bytes; a length of 0 means none).
	[UnmanagedCallersOnly]
	internal static int VisualScriptLoad(ulong graph, byte* name, int nameLength, byte* document, int documentLength, byte* references,
		int referencesLength, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			HostReply reply = VisualScripts.LoadDocument(graph, NativeString.FromUtf8(name, nameLength), Bytes(document, documentLength),
				Bytes(references, referencesLength));
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

	// The debugger's protocol (spec-slice4 sections 7.7 and 7.8): passes the reply JSON to receive for any request (UTF-8
	// JSON; refused requests are error replies). Failure only when the runtime itself failed.
	[UnmanagedCallersOnly]
	internal static int VisualScriptDebug(byte* request, int requestLength, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			return Deliver(VisualScripts.Debug(Bytes(request, requestLength)), context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// The flow counted in watched graphs since the last take (spec-slice4 sections 6.2 and 7.7): passes the binary trace block
	// to receive while a debugger is attached; NotFound (receive not called) otherwise.
	[UnmanagedCallersOnly]
	internal static int VisualScriptTakeTrace(void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			return Deliver(VisualScripts.TakeTrace(), context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// What live edits did and what went wrong at run time (spec-slice4 section 8.4): passes the events JSON of the live log's
	// receipts and runtime problems whose serial is after `after` to receive; NotFound (receive not called) when there are none.
	[UnmanagedCallersOnly]
	internal static int VisualScriptTakeLiveEvents(ulong after, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
	{
		try
		{
			return Deliver(VisualScripts.TakeLiveEvents(after), context, receive);
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// The steps the visual-script runtime's outermost executions ran since it started (its nested executions' included):
	// the engine's profiler reads it before and after an invoke, while it records, to give a graph's zone its steps.
	[UnmanagedCallersOnly]
	internal static int VisualScriptReadSteps(ulong* steps)
	{
		try
		{
			*steps = VisualScripts.StepsRun;
			return Success;
		}
		catch (Exception exception)
		{
			return Report(exception);
		}
	}

	// A play session starts (spec-slice4 section 8.5, ClearLiveLog): the live log drops its receipts and runtime problems, and
	// problems count from 1 again. Serials go on growing, so the engine keeps reading after the last serial it took.
	[UnmanagedCallersOnly]
	internal static int VisualScriptClearLiveEvents()
	{
		try
		{
			VisualScripts.ClearLiveEvents();
			return Success;
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

	// The reply's text goes to receive exactly when its status is Success, once (ScriptCoreTests call it directly).
	internal static int Deliver(HostReply reply, void* context, delegate* unmanaged<void*, byte*, int, void> receive)
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
