namespace Strada.Tests;

// What a script sees of the project's input actions (Docs/Architecture.md section 7.4): the action Action as OnUpdate reads
// it each frame (its value, vector and level, and how many frames saw its edges), as OnFixedUpdate reads it each step (how
// many steps saw its edges), and the action events it receives, in the order they came (Events: "+Jump,-Jump," for a press
// and a release), each also logged as "ActionProbe <entity name>: pressed Jump" so that a test orders them against a graph's.
// With OverrideValue set, OnUpdate of frame OverrideFrame (from 1) puts that value on OverrideAction, and the frame after
// clears it. OnCreate keeps whether the action was pressed (CreatedPressed: OnCreate of an entity a graph instantiates reads
// the frame's edges) and how many actions Actions names (ActionNameCount, -1 without an asset). With SendEvent set, each
// OnFixedUpdate raises that custom event of the entity's graph. With QueryCalls set, each OnUpdate times that many calls of
// IsActionDown (NanosecondsPerQuery: what one action query through the engine costs, for the input actions benchmark).
public sealed class ActionProbe : Script
{
	public string Action = "Jump";
	public InputActions? Actions;
	public string SendEvent = string.Empty;

	public bool CreatedPressed;
	public int ActionNameCount = -1;
	public int QueryCalls;
	public double NanosecondsPerQuery;
	// How many of the timed calls read the action down.
	public int QueryDowns;

	public float Value;
	public Vector2 Vector;
	public bool Down;
	public int Frames;
	public int UpdatePresses;
	public int UpdateReleases;
	public int Steps;
	public int StepPresses;
	public int StepReleases;
	// A step whose press and release both read true.
	public int StepTaps;
	public int Presses;
	public int Releases;
	public string Events = string.Empty;

	public string OverrideAction = string.Empty;
	public float OverrideValue;
	public int OverrideFrame;

	protected override void OnCreate()
	{
		CreatedPressed = Input.IsActionPressed(Action);
		ActionNameCount = Actions?.ActionNames.Length ?? -1;
	}

	protected override void OnUpdate(float deltaTime)
	{
		Frames++;
		Value = Input.GetAction(Action);
		Vector = Input.GetActionVector(Action);
		Down = Input.IsActionDown(Action);
		UpdatePresses += Input.IsActionPressed(Action) ? 1 : 0;
		UpdateReleases += Input.IsActionReleased(Action) ? 1 : 0;
		if (QueryCalls > 0)
		{
			int down = 0;
			long start = System.Diagnostics.Stopwatch.GetTimestamp();
			for (int call = 0; call < QueryCalls; call++)
			{
				down += Input.IsActionDown(Action) ? 1 : 0;
			}
			long elapsed = System.Diagnostics.Stopwatch.GetTimestamp() - start;
			NanosecondsPerQuery = elapsed * 1.0e9 / System.Diagnostics.Stopwatch.Frequency / QueryCalls;
			QueryDowns = down;
		}
		if (OverrideAction.Length == 0)
		{
			return;
		}
		if (Frames == OverrideFrame)
		{
			Input.SetActionOverride(OverrideAction, OverrideValue);
		}
		else if (Frames == OverrideFrame + 1)
		{
			Input.ClearActionOverride(OverrideAction);
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		Steps++;
		bool pressed = Input.IsActionPressed(Action);
		bool released = Input.IsActionReleased(Action);
		StepPresses += pressed ? 1 : 0;
		StepReleases += released ? 1 : 0;
		StepTaps += pressed && released ? 1 : 0;
		if (SendEvent.Length > 0)
		{
			GetComponent<VisualScriptComponent>()?.SendEvent(SendEvent);
		}
	}

	protected override void OnActionPressed(string action)
	{
		Presses += action == Action ? 1 : 0;
		Events += "+" + action + ",";
		Log.Info($"ActionProbe {Name}: pressed {action}");
	}

	protected override void OnActionReleased(string action)
	{
		Releases += action == Action ? 1 : 0;
		Events += "-" + action + ",";
		Log.Info($"ActionProbe {Name}: released {action}");
	}
}
