using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Strada.VisualScripting;

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
	Click = 8,
	// An input action's edge: the engine names the action while its handlers run (InputActionContext.GetHandledAction).
	ActionPressed = 9,
	ActionReleased = 10,
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
	// The Entity handed out for each entity without a script instance, so that a query, an event or a graph that meets the
	// entity again allocates nothing. Entities compare by ID and hold nothing else, so a kept one is the same as a new one.
	// The engine tells scripts of no destruction of an entity without a script, so the cache empties with the scene's
	// instances and, at MaxKeptEntities, starts over rather than growing with every entity a long run makes.
	private static readonly Dictionary<ulong, Entity> s_Entities = [];
	internal const int MaxKeptEntities = 4096;
	// The public types of the game assembly (nested public types included), by full name: what [VisualNode] can mark.
	private static readonly List<Type> s_GameTypes = [];
	// The fields ("<class>.<field>") whose too long lists DescribeInstanceFields has reported since the assembly loaded.
	private static readonly HashSet<string> s_ReportedLongLists = new(StringComparer.Ordinal);

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
			if (type.IsVisible)
			{
				s_GameTypes.Add(type);
			}
		}
		s_GameTypes.Sort(static (a, b) => string.CompareOrdinal(a.FullName, b.FullName));
		// Graphs that did not compile without the game's types may compile now: they compile again when next used, with a
		// node library that has the new script classes (spec section 5.6).
		Host.VisualScripts.OnGameAssemblyLoaded();
		EngineLibrary.Drop();
	}

	internal static void UnloadGameAssembly()
	{
		// Visual scripts reference game types too: their instances, compiled programs and node library go first.
		Host.VisualScripts.OnGameAssemblyUnloading();
		EngineLibrary.Drop();
		s_Instances.Clear();
		s_Entities.Clear();
		s_Classes.Clear();
		s_GameTypes.Clear();
		s_ReportedLongLists.Clear();
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
		// Serialized fields that cannot be fields, with why: reported once for the class instead of dropped in silence.
		List<string> skipped = [];
		foreach (FieldInfo field in GetSerializedFields(type))
		{
			ScriptFieldType fieldType = ScriptFieldCodec.GetMemberFieldType(field.FieldType);
			if (fieldType == ScriptFieldType.None)
			{
				skipped.Add($"{field.Name} ({DescribeClrType(field.FieldType)})");
				continue;
			}
			object? defaultValue = ScriptFieldCodec.ToStored(field.GetValue(defaults), fieldType);
			// The engine refuses a list of more elements, and with it the class list: such a field is no field.
			if (defaultValue is List<object?> { Count: > ScriptFieldCodec.MaxListElements } elements)
			{
				skipped.Add(FormattableString.Invariant(
					$"{field.Name} (its default holds {elements.Count} elements, more than a list's {ScriptFieldCodec.MaxListElements})"));
				continue;
			}
			fields.Add(new ScriptField { Field = field, Type = fieldType, DefaultValue = defaultValue });
		}
		if (skipped.Count > 0)
		{
			Log.Warn($"Script class {name}: not serialized or shown in the inspector, as no field type stores them: {string.Join(", ", skipped)}");
		}
		s_Classes[name] = new ScriptClass { Type = type, Fields = fields };
	}

	// A C# type as a reader names it: "Dictionary<String, Int32>", "Int32[][]", "Vector3".
	private static string DescribeClrType(Type type)
	{
		if (type.IsArray)
		{
			return $"{DescribeClrType(type.GetElementType()!)}[{new string(',', type.GetArrayRank() - 1)}]";
		}
		if (!type.IsGenericType)
		{
			return type.Name;
		}
		string name = type.Name;
		int tick = name.IndexOf('`', StringComparison.Ordinal);
		return $"{(tick < 0 ? name : name[..tick])}<{string.Join(", ", type.GetGenericArguments().Select(DescribeClrType))}>";
	}

	// The element type of a list field, the field's own type otherwise: what its attributes describe.
	private static Type GetValueType(ScriptField field) => ScriptFieldCodec.GetListElementType(field.Field.FieldType) ?? field.Field.FieldType;

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

	// The classes as JSON: [{ "Name", "Fields": [{ "Name", "Type", "Default", "Hidden"?, "Tooltip"?, "Min"?, "Max"?,
	// "AssetType"?, "Enum"?: [{ "Name", "Value" }], "Flags"? }] }]. Enumerators are listed in declaration order. A list
	// field's type is its element's with "[]" ("Int32[]"), and the asset type, the range and the enumerators describe each
	// element.
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
					writer.WriteString("Type", ScriptFieldCodec.GetTypeName(field.Type));
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
					if (ScriptFieldCodec.GetAssetType(GetValueType(field)) is { } assetType)
					{
						writer.WriteString("AssetType", assetType);
					}
					if (GetValueType(field).IsEnum)
					{
						WriteEnumerators(writer, field);
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

	private static void WriteEnumerators(Utf8JsonWriter writer, ScriptField field)
	{
		Type enumType = GetValueType(field);
		// A list's enumerators are values of its elements.
		ScriptFieldType valueType = ScriptFieldCodec.GetElementType(field.Type);
		writer.WriteStartArray("Enum");
		foreach (FieldInfo enumerator in enumType.GetFields(BindingFlags.Public | BindingFlags.Static))
		{
			writer.WriteStartObject();
			writer.WriteString("Name", enumerator.Name);
			writer.WritePropertyName("Value");
			ScriptFieldCodec.WriteValue(writer, valueType, ScriptFieldCodec.ToStored(enumerator.GetValue(null), valueType));
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
		if (enumType.IsDefined(typeof(FlagsAttribute)))
		{
			writer.WriteBoolean("Flags", true);
		}
	}

	internal static bool HasClass(string name) => s_Classes.ContainsKey(name);

	// The registered script classes, in name order: the game's types in the visual-script node library.
	internal static IReadOnlyList<Type> GetScriptClasses()
	{
		List<Type> classes = new(s_Classes.Count);
		foreach (ScriptClass scriptClass in s_Classes.Values)
		{
			classes.Add(scriptClass.Type);
		}
		return classes;
	}

	// The public types of the loaded game assembly, by full name (ordinal): the game's part of the visual-script node
	// library ([VisualNode] members and the types they use, spec-slice3 section 3.7.4). Empty while none is loaded.
	internal static IReadOnlyList<Type> GetGameTypes() => [.. s_GameTypes];

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
		s_Entities.Remove(entity);
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
			string typeName = ScriptFieldCodec.GetTypeName(field.Type);
			if (type.ValueKind != JsonValueKind.String || type.GetString() != typeName)
			{
				Log.Warn($"{className}.{stored.Name}: stored {type.GetRawText()} value ignored: the field is a {typeName} now");
				continue;
			}
			object? storedValue = ScriptFieldCodec.ReadValue(value, field.Type, out string? error);
			object? fieldValue = error == null ? ScriptFieldCodec.FromStored(storedValue, field.Field.FieldType, out error) : null;
			if (error != null)
			{
				Log.Warn($"{className}.{stored.Name}: stored value ignored: {error}");
				continue;
			}
			field.Field.SetValue(instance, fieldValue);
		}
	}

	// Sets fields of the entity's instance from stored values, as CreateInstance does: values of fields the class does not
	// have are skipped, and values that no longer fit their field are reported and skipped.
	internal static int SetInstanceFields(ulong entity, ReadOnlySpan<byte> fieldsJson)
	{
		if (!s_Instances.TryGetValue(entity, out Script? instance) || !TryGetClass(instance, out string className, out ScriptClass? scriptClass))
		{
			return Host.NotFound;
		}
		if (!fieldsJson.IsEmpty)
		{
			ApplyFields(instance, className, scriptClass, fieldsJson);
		}
		return Host.Success;
	}

	// The values the serialized fields of the entity's instance hold, in the stored form: { "Name": { "Type", "Value" } }.
	// A list holding more elements than a list can is left out (the engine would refuse every value with it), so what the
	// component stores for it stays; reported once per field while the assembly is loaded. Null when the entity has no
	// instance.
	internal static byte[]? DescribeInstanceFields(ulong entity)
	{
		if (!s_Instances.TryGetValue(entity, out Script? instance) || !TryGetClass(instance, out string className, out ScriptClass? scriptClass))
		{
			return null;
		}
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			writer.WriteStartObject();
			foreach (ScriptField field in scriptClass.Fields)
			{
				object? stored = ScriptFieldCodec.ToStored(field.Field.GetValue(instance), field.Type);
				if (stored is List<object?> { Count: > ScriptFieldCodec.MaxListElements } elements)
				{
					if (s_ReportedLongLists.Add($"{className}.{field.Field.Name}"))
					{
						Log.Warn(FormattableString.Invariant(
							$"{className}.{field.Field.Name}: holds {elements.Count} elements, more than a list's {ScriptFieldCodec.MaxListElements}: its value is not read back"));
					}
					continue;
				}
				writer.WriteStartObject(field.Field.Name);
				writer.WriteString("Type", ScriptFieldCodec.GetTypeName(field.Type));
				writer.WritePropertyName("Value");
				ScriptFieldCodec.WriteValue(writer, field.Type, stored);
				writer.WriteEndObject();
			}
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}

	// The registered class of an instance, which is named after its type.
	private static bool TryGetClass(Script instance, out string className, [NotNullWhen(true)] out ScriptClass? scriptClass)
	{
		className = instance.GetType().FullName ?? instance.GetType().Name;
		return s_Classes.TryGetValue(className, out scriptClass);
	}

	internal static bool HasInstance(ulong entity) => s_Instances.ContainsKey(entity);

	internal static Script? GetInstance(ulong entity) => s_Instances.GetValueOrDefault(entity);

	internal static bool DestroyInstance(ulong entity) => s_Instances.Remove(entity);

	internal static void DestroyAllInstances()
	{
		s_Instances.Clear();
		s_Entities.Clear();
	}

	// Runs a lifecycle method of the entity's instance (Destroy also removes the instance); script exceptions are
	// logged and reported as Failure. The values are the event's time step or other entity.
	internal static int Invoke(ulong entity, ScriptEvent scriptEvent, float timeStep, ulong other)
	{
		if (!s_Instances.TryGetValue(entity, out Script? instance))
		{
			return Host.NotFound;
		}
		// A callback the engine makes reads the frame's action edges (inside a fixed step the engine reads the step's), even
		// when a graph's call into the engine runs it.
		InputActionOrigin origin = InputActionContext.Enter(InputActionOrigin.Frame);
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
				case ScriptEvent.Click:
					instance.InvokeOnClick(GetEntity(other));
					break;
				case ScriptEvent.ActionPressed:
					instance.InvokeOnActionPressed(InputActionContext.GetHandledAction());
					break;
				case ScriptEvent.ActionReleased:
					instance.InvokeOnActionReleased(InputActionContext.GetHandledAction());
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
		finally
		{
			InputActionContext.Restore(origin);
		}
	}

	// The script instance of the entity when it has one, so scripts can test what they touched with `is`; else the Entity
	// kept for it, made at its first use.
	internal static Entity GetEntity(ulong entity)
	{
		if (s_Instances.TryGetValue(entity, out Script? instance))
		{
			return instance;
		}
		if (s_Entities.TryGetValue(entity, out Entity? kept))
		{
			return kept;
		}
		if (s_Entities.Count >= MaxKeptEntities)
		{
			s_Entities.Clear();
		}
		Entity made = new(entity);
		s_Entities.Add(entity, made);
		return made;
	}

	// How many Entity objects are kept for entities without a script (for tests).
	internal static int KeptEntityCount => s_Entities.Count;
}
