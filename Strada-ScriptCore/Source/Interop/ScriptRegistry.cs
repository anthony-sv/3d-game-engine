using System;
using System.Buffers;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;

namespace Strada.Interop;

// Script lifecycle events; the values are the engine's ScriptEvent.
internal enum ScriptEvent
{
	Create = 0,
	Update = 1,
	FixedUpdate = 2,
	Destroy = 3,
	CollisionEnter = 4,
	CollisionExit = 5,
	TriggerEnter = 6,
	TriggerExit = 7,
}

// The game's script classes and the instances of the running scene.
internal static class ScriptRegistry
{
	internal sealed class ScriptField
	{
		public required FieldInfo Field { get; init; }
		public required ScriptFieldType Type { get; init; }
		public required object? DefaultValue { get; init; }
	}

	internal sealed class ScriptClass
	{
		public required Type Type { get; init; }
		public required List<ScriptField> Fields { get; init; }
	}

	private static GameLoadContext? s_LoadContext;
	private static readonly SortedDictionary<string, ScriptClass> s_Classes = new(StringComparer.Ordinal);
	private static readonly Dictionary<ulong, Script> s_Instances = [];

	internal static void LoadGameAssembly(string path)
	{
		UnloadGameAssembly();
		GameLoadContext context = new(Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".");
		s_LoadContext = context;
		Assembly assembly = context.LoadFromFile(path);

		Type[] types;
		try
		{
			types = assembly.GetTypes();
		}
		catch (ReflectionTypeLoadException exception)
		{
			// Keep the classes that loaded; report why the others did not.
			foreach (Exception? loaderException in exception.LoaderExceptions)
			{
				Log.Error($"{assembly.GetName().Name}: {loaderException?.Message}");
			}
			types = [.. exception.Types.OfType<Type>()];
		}

		foreach (Type type in types)
		{
			if (type.IsClass && !type.IsAbstract && !type.ContainsGenericParameters && type.IsSubclassOf(typeof(Script)))
			{
				RegisterClass(type);
			}
		}
	}

	internal static void UnloadGameAssembly()
	{
		s_Instances.Clear();
		s_Classes.Clear();
		if (s_LoadContext == null)
		{
			return;
		}
		WeakReference context = BeginUnload();
		// Unloading completes once nothing references the game's types any more.
		for (int attempt = 0; context.IsAlive && attempt < 10; attempt++)
		{
			GC.Collect();
			GC.WaitForPendingFinalizers();
		}
		if (context.IsAlive)
		{
			Log.Warn("The previous game assembly is still referenced (for example by a static field or a running thread) and stays loaded");
		}
	}

	// A separate method so no local of the caller keeps the context alive.
	[MethodImpl(MethodImplOptions.NoInlining)]
	private static WeakReference BeginUnload()
	{
		WeakReference reference = new(s_LoadContext);
		s_LoadContext!.Unload();
		s_LoadContext = null;
		return reference;
	}

	private static void RegisterClass(Type type)
	{
		string name = type.FullName ?? type.Name;
		if (type.GetConstructor(Type.EmptyTypes) == null)
		{
			Log.Warn($"Script class {name} is ignored: it needs a public parameterless constructor");
			return;
		}

		// A default instance supplies the fields' initial values.
		Script defaults;
		try
		{
			defaults = (Script)Activator.CreateInstance(type)!;
		}
		catch (TargetInvocationException exception)
		{
			Log.Error($"Script class {name} is ignored: its constructor threw {exception.InnerException}");
			return;
		}

		List<ScriptField> fields = [];
		foreach (FieldInfo field in GetSerializedFields(type))
		{
			ScriptFieldType fieldType = ScriptFieldCodec.GetFieldType(field.FieldType);
			if (fieldType != ScriptFieldType.None)
			{
				fields.Add(new ScriptField { Field = field, Type = fieldType, DefaultValue = field.GetValue(defaults) });
			}
		}
		s_Classes[name] = new ScriptClass { Type = type, Fields = fields };
	}

	// Public instance fields and those marked [SerializeField], base classes first.
	private static List<FieldInfo> GetSerializedFields(Type type)
	{
		List<Type> hierarchy = [];
		for (Type? current = type; current != null && current != typeof(Script); current = current.BaseType)
		{
			hierarchy.Insert(0, current);
		}
		List<FieldInfo> fields = [];
		foreach (Type declaringType in hierarchy)
		{
			foreach (FieldInfo field in declaringType.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
			{
				bool serialized = field.IsPublic || field.IsDefined(typeof(SerializeFieldAttribute));
				if (serialized && !field.IsInitOnly && !field.IsDefined(typeof(CompilerGeneratedAttribute)))
				{
					fields.Add(field);
				}
			}
		}
		return fields;
	}

	// The classes as JSON: [{ "Name", "Fields": [{ "Name", "Type", "Default", "Hidden"?, "Tooltip"?, "Min"?, "Max"? }] }].
	internal static byte[] DescribeClasses()
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			writer.WriteStartArray();
			foreach ((string name, ScriptClass scriptClass) in s_Classes)
			{
				writer.WriteStartObject();
				writer.WriteString("Name", name);
				writer.WriteStartArray("Fields");
				foreach (ScriptField field in scriptClass.Fields)
				{
					writer.WriteStartObject();
					writer.WriteString("Name", field.Field.Name);
					writer.WriteString("Type", field.Type.ToString());
					writer.WritePropertyName("Default");
					ScriptFieldCodec.WriteValue(writer, field.Type, field.DefaultValue);
					if (field.Field.IsDefined(typeof(HideInInspectorAttribute)))
					{
						writer.WriteBoolean("Hidden", true);
					}
					if (field.Field.GetCustomAttribute<TooltipAttribute>() is { } tooltip)
					{
						writer.WriteString("Tooltip", tooltip.Text);
					}
					if (field.Field.GetCustomAttribute<RangeAttribute>() is { } range)
					{
						writer.WriteNumber("Min", range.Min);
						writer.WriteNumber("Max", range.Max);
					}
					writer.WriteEndObject();
				}
				writer.WriteEndArray();
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
		}
		return buffer.WrittenSpan.ToArray();
	}

	internal static bool HasClass(string name) => s_Classes.ContainsKey(name);

	// Creates the entity's instance with the stored field values ({ "Name": { "Type", "Value" } }), replacing a current
	// one. Values that no longer fit their field (the script changed) are reported and keep the field's default.
	internal static int CreateInstance(string className, ulong entity, ReadOnlySpan<byte> fieldsJson)
	{
		if (!s_Classes.TryGetValue(className, out ScriptClass? scriptClass))
		{
			return Host.NotFound;
		}
		Script instance;
		try
		{
			instance = (Script)Activator.CreateInstance(scriptClass.Type)!;
		}
		catch (TargetInvocationException exception)
		{
			Log.Error($"{className}: the constructor threw {exception.InnerException}");
			return Host.Failure;
		}
		instance.ID = entity;
		if (!fieldsJson.IsEmpty)
		{
			ApplyFields(instance, className, scriptClass, fieldsJson);
		}
		s_Instances[entity] = instance;
		return Host.Success;
	}

	private static void ApplyFields(Script instance, string className, ScriptClass scriptClass, ReadOnlySpan<byte> fieldsJson)
	{
		using JsonDocument document = JsonDocument.Parse(fieldsJson.ToArray());
		if (document.RootElement.ValueKind != JsonValueKind.Object)
		{
			Log.Warn($"{className}: stored fields ignored: expected an object");
			return;
		}
		foreach (JsonProperty stored in document.RootElement.EnumerateObject())
		{
			ScriptField? field = scriptClass.Fields.Find(candidate => candidate.Field.Name == stored.Name);
			if (field == null)
			{
				continue;
			}
			if (stored.Value.ValueKind != JsonValueKind.Object || !stored.Value.TryGetProperty("Type", out JsonElement type)
				|| !stored.Value.TryGetProperty("Value", out JsonElement value))
			{
				Log.Warn($"{className}.{stored.Name}: stored value ignored: expected {{ \"Type\", \"Value\" }}");
				continue;
			}
			if (type.ValueKind != JsonValueKind.String || type.GetString() != field.Type.ToString())
			{
				Log.Warn($"{className}.{stored.Name}: stored {type.GetRawText()} value ignored: the field is a {field.Type} now");
				continue;
			}
			object? fieldValue = ScriptFieldCodec.ReadValue(value, field.Type, out string? error);
			if (error != null)
			{
				Log.Warn($"{className}.{stored.Name}: stored value ignored: {error}");
				continue;
			}
			field.Field.SetValue(instance, fieldValue);
		}
	}

	internal static bool HasInstance(ulong entity) => s_Instances.ContainsKey(entity);

	internal static Script? GetInstance(ulong entity) => s_Instances.GetValueOrDefault(entity);

	internal static bool DestroyInstance(ulong entity) => s_Instances.Remove(entity);

	internal static void DestroyAllInstances() => s_Instances.Clear();

	// Runs a lifecycle method of the entity's instance (Destroy also removes the instance); script exceptions are
	// logged and reported as Failure. The values are the event's time step or other entity.
	internal static int Invoke(ulong entity, ScriptEvent scriptEvent, float timeStep, ulong other)
	{
		if (!s_Instances.TryGetValue(entity, out Script? instance))
		{
			return Host.NotFound;
		}
		try
		{
			switch (scriptEvent)
			{
				case ScriptEvent.Create:
					instance.InvokeOnCreate();
					break;
				case ScriptEvent.Update:
					Time.DeltaTime = timeStep;
					instance.InvokeOnUpdate(timeStep);
					break;
				case ScriptEvent.FixedUpdate:
					Time.FixedDeltaTime = timeStep;
					instance.InvokeOnFixedUpdate(timeStep);
					break;
				case ScriptEvent.Destroy:
					s_Instances.Remove(entity);
					instance.InvokeOnDestroy();
					break;
				case ScriptEvent.CollisionEnter:
					instance.InvokeOnCollisionEnter(GetEntity(other));
					break;
				case ScriptEvent.CollisionExit:
					instance.InvokeOnCollisionExit(GetEntity(other));
					break;
				case ScriptEvent.TriggerEnter:
					instance.InvokeOnTriggerEnter(GetEntity(other));
					break;
				case ScriptEvent.TriggerExit:
					instance.InvokeOnTriggerExit(GetEntity(other));
					break;
				default:
					return Host.Failure;
			}
			return Host.Success;
		}
		catch (Exception exception)
		{
			Log.Error($"{instance.GetType().FullName}.On{scriptEvent} (entity {entity}): {exception}");
			return Host.Failure;
		}
	}

	// The script instance of the entity when it has one, so scripts can test what they touched with `is`.
	internal static Entity GetEntity(ulong entity) => s_Instances.TryGetValue(entity, out Script? instance) ? instance : new Entity(entity);
}
