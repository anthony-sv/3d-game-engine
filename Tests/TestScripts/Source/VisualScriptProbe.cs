using System;
using Strada.Testing;

namespace Strada.Tests;

// Uses the C# API of visual scripts on the graph its Target entity runs, the asset at GraphPath, which has the custom events
// Add(int Amount), Boost(float Amount), Name(string Text, Entity Who) and Fail() (an integer division by zero), and the
// variables Score (public int, 1), Speed (float, 0.5), Label (string) and Partner (Entity). It also gives a Holder entity it
// creates that graph. Each part reports as a check (TestReporter).
public sealed class VisualScriptProbe : Script
{
	public Entity? Target;
	public string GraphPath = "";

	protected override void OnCreate()
	{
		VisualScriptComponent graph = Target!.GetComponent<VisualScriptComponent>()!;
		TestReporter.Run("variables", () => VariableChecks(graph));
		TestReporter.Run("references", () => ReferenceChecks(graph));
		TestReporter.Run("events", () => EventChecks(graph));
		TestReporter.Run("refused events", () => RefusedEventChecks(graph));
		TestReporter.Run("errors", () => ErrorChecks(graph));
		TestReporter.Run("graphs", () => GraphChecks(graph, GraphPath));
	}

	private static void VariableChecks(VisualScriptComponent graph)
	{
		Assert.IsTrue(graph.HasVariable("Score"));
		Assert.IsTrue(graph.HasVariable("Label"), "private variables count");
		Assert.IsFalse(graph.HasVariable("Missing"));
		Assert.AreEqual(1, graph.GetVariable<int>("Score"));
		graph.SetVariable("Score", 5);
		Assert.AreEqual(5, graph.GetVariable<int>("Score"));
		// Read as wider numbers and as objects; written from narrower numbers.
		Assert.AreEqual(5L, graph.GetVariable<long>("Score"));
		Assert.AreEqual(5.0, graph.GetVariable<double>("Score"));
		Assert.AreEqual<object?>(5, graph.GetVariable<object>("Score"));
		Assert.AreEqual(0.5f, graph.GetVariable<float>("Speed"));
		graph.SetVariable("Speed", 2);
		Assert.AreEqual(2.0f, graph.GetVariable<float>("Speed"));
	}

	private void ReferenceChecks(VisualScriptComponent graph)
	{
		Assert.IsNull(graph.GetVariable<Entity>("Partner"));
		// A component stands for its entity; null is none, and an empty text for strings.
		graph.SetVariable("Partner", Transform);
		Assert.IsTrue(graph.GetVariable<Entity>("Partner") == this);
		graph.SetVariable<Entity?>("Partner", null);
		Assert.IsNull(graph.GetVariable<Entity>("Partner"));
		graph.SetVariable("Label", "text");
		Assert.AreEqual("text", graph.GetVariable<string>("Label"));
		graph.SetVariable<string?>("Label", null);
		Assert.AreEqual("", graph.GetVariable<string>("Label"));
	}

	private void EventChecks(VisualScriptComponent graph)
	{
		Assert.IsTrue(graph.SendEvent("Add", 3));
		Assert.AreEqual(8, graph.GetVariable<int>("Score"));
		// An int argument widens to the float parameter; a script instance is an Entity.
		Assert.IsTrue(graph.SendEvent("Boost", 2));
		Assert.AreEqual(4.0f, graph.GetVariable<float>("Speed"));
		Assert.IsTrue(graph.SendEvent("Name", "Nobody", null));
		Assert.AreEqual("Nobody", graph.GetVariable<string>("Label"));
		Assert.IsNull(graph.GetVariable<Entity>("Partner"));
		Assert.IsTrue(graph.SendEvent("Name", "Probe", this));
		Assert.AreEqual("Probe", graph.GetVariable<string>("Label"));
		Assert.IsTrue(graph.GetVariable<Entity>("Partner") == this);
	}

	private static void RefusedEventChecks(VisualScriptComponent graph)
	{
		// Unknown events and arguments that do not fit are logged and do not run.
		Assert.IsFalse(graph.SendEvent("Missing"));
		Assert.IsFalse(graph.SendEvent("Add"));
		Assert.IsFalse(graph.SendEvent("Add", 1, 2));
		Assert.IsFalse(graph.SendEvent("Add", "three"));
		Assert.IsFalse(graph.SendEvent("Add", (object?)null));
		Assert.AreEqual(8, graph.GetVariable<int>("Score"));
	}

	private static void ErrorChecks(VisualScriptComponent graph)
	{
		ArgumentException unknown = Assert.Throws<ArgumentException>(() => graph.GetVariable<int>("Missing"));
		Assert.AreEqual(typeof(ArgumentException), unknown.GetType());
		Assert.AreEqual("name", unknown.ParamName);
		Assert.Throws<ArgumentException>(() => graph.SetVariable("Missing", 1));
		Assert.Throws<InvalidCastException>(() => graph.GetVariable<string>("Score"));
		Assert.Throws<InvalidCastException>(() => graph.GetVariable<int>("Speed"));
		Assert.Throws<InvalidCastException>(() => graph.GetVariable<int>("Partner"));
		Assert.Throws<InvalidCastException>(() => graph.SetVariable("Score", "x"));
		Assert.Throws<InvalidCastException>(() => graph.SetVariable("Score", 1.5f));
		Assert.Throws<ArgumentNullException>(() => graph.GetVariable<int>(null!));
		Assert.Throws<ArgumentNullException>(() => graph.SetVariable(null!, 1));
		Assert.Throws<ArgumentNullException>(() => graph.HasVariable(null!));
		Assert.Throws<ArgumentNullException>(() => graph.SendEvent(null!));
		Assert.Throws<ArgumentNullException>(() => graph.SendEvent("Add", null!));
		// An error in the event reaches the caller, carrying the logged message.
		InvalidOperationException failed = Assert.Throws<InvalidOperationException>(() => graph.SendEvent("Fail"));
		Assert.IsTrue(failed.Message.EndsWith("integer division by zero", StringComparison.Ordinal), failed.Message);
		Assert.AreEqual(8, graph.GetVariable<int>("Score"));
	}

	private static void GraphChecks(VisualScriptComponent graph, string path)
	{
		VisualScript? script = Assets.Load<VisualScript>(path);
		Assert.IsNotNull(script);
		Assert.AreEqual(script, graph.Graph);
		// An entity without a graph instance: nothing to read or raise, until a graph is set and the scene starts it.
		VisualScriptComponent holder = Create("Holder").AddComponent<VisualScriptComponent>();
		Assert.IsNull(holder.Graph);
		Assert.IsFalse(holder.HasVariable("Score"));
		Assert.IsFalse(holder.SendEvent("Add", 1));
		Assert.Throws<InvalidOperationException>(() => holder.GetVariable<int>("Score"));
		Assert.Throws<InvalidOperationException>(() => holder.SetVariable("Score", 1));
		holder.Graph = script;
		Assert.AreEqual(script, holder.Graph);
		holder.Graph = null;
		Assert.IsNull(holder.Graph);
		holder.Graph = script;
		Assert.IsTrue(holder.Graph == graph.Graph);
	}
}
