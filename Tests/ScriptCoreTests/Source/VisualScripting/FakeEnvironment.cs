using System;
using System.Collections.Generic;
using System.Linq;
using Strada.VisualScripting;

namespace Strada.ScriptCore.Tests;

// The engine as the runtime tests choose it (spec section 5.5): entities with names, validity, components and script
// instances, input states, and every log line, recorded in order.
internal sealed class FakeEnvironment : IVisualScriptEnvironment
{
	private readonly Dictionary<ulong, FakeEntity> m_Entities = [];

	internal List<(VisualScriptLogLevel Level, string Message)> Logs { get; } = [];

	internal float? DeltaTime { get; private set; }

	internal int EntityLookups { get; private set; }

	internal HashSet<KeyCode> PressedKeys { get; } = [];

	internal HashSet<KeyCode> ReleasedKeys { get; } = [];

	internal HashSet<MouseButton> PressedButtons { get; } = [];

	internal HashSet<MouseButton> ReleasedButtons { get; } = [];

	internal HashSet<(GamepadButton Button, int Gamepad)> PressedGamepadButtons { get; } = [];

	internal HashSet<(GamepadButton Button, int Gamepad)> ReleasedGamepadButtons { get; } = [];

	// The component types the runtime fetched, in order.
	internal List<(ulong Entity, Type Type)> ComponentFetches { get; } = [];

	internal IEnumerable<string> Errors => Logs.Where(static log => log.Level == VisualScriptLogLevel.Error).Select(static log => log.Message);

	internal IEnumerable<string> Warnings => Logs.Where(static log => log.Level == VisualScriptLogLevel.Warn).Select(static log => log.Message);

	internal FakeEntity Add(ulong id, string name, Script? instance = null)
	{
		if (instance != null)
		{
			instance.ID = id;
		}
		FakeEntity entity = new(name, instance);
		m_Entities[id] = entity;
		return entity;
	}

	internal FakeEntity this[ulong id] => m_Entities[id];

	public void Log(VisualScriptLogLevel level, string message)
	{
		Logs.Add((level, message));
	}

	public Entity? GetEntity(ulong id)
	{
		EntityLookups++;
		if (id == 0)
		{
			return null;
		}
		return m_Entities.TryGetValue(id, out FakeEntity? entity) && entity.Instance != null ? entity.Instance : new Entity(id);
	}

	public string GetEntityName(ulong id) => m_Entities.TryGetValue(id, out FakeEntity? entity) ? entity.Name : string.Empty;

	public bool IsEntityValid(ulong id) => m_Entities.TryGetValue(id, out FakeEntity? entity) && entity.Valid;

	public bool HasComponent(ulong id, Type componentType) =>
		m_Entities.TryGetValue(id, out FakeEntity? entity) && entity.Valid && entity.Components.Contains(componentType);

	// A new view on every fetch, as the engine's component access makes them.
	public Component? GetComponent(ulong id, Type componentType)
	{
		ComponentFetches.Add((id, componentType));
		if (!HasComponent(id, componentType))
		{
			return null;
		}
		Component view = (Component)Activator.CreateInstance(componentType, nonPublic: true)!;
		view.Entity = GetEntity(id)!;
		return view;
	}

	public void SetDeltaTime(float timeStep)
	{
		DeltaTime = timeStep;
	}

	public bool IsKeyPressed(KeyCode key) => PressedKeys.Contains(key);

	public bool IsKeyReleased(KeyCode key) => ReleasedKeys.Contains(key);

	public bool IsMouseButtonPressed(MouseButton button) => PressedButtons.Contains(button);

	public bool IsMouseButtonReleased(MouseButton button) => ReleasedButtons.Contains(button);

	public bool IsGamepadButtonPressed(GamepadButton button, int gamepad) => PressedGamepadButtons.Contains((button, gamepad));

	public bool IsGamepadButtonReleased(GamepadButton button, int gamepad) => ReleasedGamepadButtons.Contains((button, gamepad));

	internal sealed class FakeEntity
	{
		internal FakeEntity(string name, Script? instance)
		{
			Name = name;
			Instance = instance;
		}

		internal string Name { get; }

		internal Script? Instance { get; }

		internal bool Valid { get; set; } = true;

		internal HashSet<Type> Components { get; } = [];
	}
}
