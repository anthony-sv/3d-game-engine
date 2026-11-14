using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Visual scripts and their C# API. This test's entity also runs Scripts/Features.sgraph, which checks every
/// built-in node kind itself, calls the library Scripts/MathLibrary.sgraph and this script's [VisualNode] members; the
/// falling box and ball run Scripts/Contacts.sgraph next to their CollisionProbe (Features messages them and changes their
/// variables), and the UI canvas runs Scripts/Clicks.sgraph next to UITests. Each graph counts its passed checks in Checks
/// and sets Done after its last; this test waits for all of them, then checks what they counted and did, and uses them from
/// C#.</summary>
public sealed class VisualScriptTests : FeatureTestScript
{
	private const int FrameLimit = 600;
	// The checks each graph passes before it sets Done (Tools/AuthorGraphs.py writes the graphs and compares Features').
	private const int FeaturesChecks = 38;
	private const int ContactsChecks = 1;
	private const int ClicksChecks = 1;
	// The graphs, in messages.
	private const string FeaturesGraph = "Features.sgraph on Visual Script Tests";
	private const string BoxGraph = "Contacts.sgraph on Falling Box";
	private const string BallGraph = "Contacts.sgraph on Falling Ball";
	private const string CanvasGraph = "Clicks.sgraph on UI Canvas";

	/// <summary>How often Features.sgraph visited this script: a field that graphs get and set as nodes.</summary>
	[VisualNode(Category = "Feature Test/Visual Script Tests")]
	public int GraphVisits;

	private VisualScriptComponent? m_Holder;
	private Medal m_Awarded = Medal.None;
	private int m_AwardedHits;

	/// <summary>The visits so far and some more: a pure method of a script, which graphs run on the entity's own
	/// instance.</summary>
	[VisualNode(Pure = true, Category = "Feature Test/Visual Script Tests")]
	public int VisitsAnd(int more) => GraphVisits + more;

	/// <summary>Records the medal a tally earns and returns it: a method with every option of [VisualNode].</summary>
	[VisualNode(Title = "Award Medal", Category = "Feature Test/Awards", Description = "Records the medal the tally earns on the test.",
		Keywords = ["medal", "award", "prize"], Pure = false, FormerNames = ["GiveMedal"])]
	public Medal Award(Tally tally)
	{
		m_Awarded = tally.Medal;
		m_AwardedHits = tally.Hits;
		return m_Awarded;
	}

	protected override void Start()
	{
		Check("graphs are given to entities from C#", () =>
		{
			m_Holder = Entity.Create("Visual Script Holder").AddComponent<VisualScriptComponent>();
			Assert.IsNull(m_Holder.Graph);
			VisualScript? clicks = Assets.Load<VisualScript>("Scripts/Clicks.sgraph");
			Assert.IsNotNull(clicks);
			m_Holder.Graph = clicks;
			Assert.AreEqual(clicks, m_Holder.Graph);
			// The scene starts the graph at its next update.
			Assert.IsFalse(m_Holder.HasVariable("Clicks"));
		});
		Check("a timeline's Direction is Forward or Backward", () =>
		{
			TimelineDirection direction = default;
			Assert.AreEqual(TimelineDirection.Forward, direction);
			direction = TimelineDirection.Backward;
			Assert.AreEqual(1, (int)direction);
		});
	}

	protected override bool Step(int frame, float deltaTime)
	{
		VisualScriptComponent? features = GetComponent<VisualScriptComponent>();
		VisualScriptComponent? box = Find("Falling Box");
		VisualScriptComponent? ball = Find("Falling Ball");
		VisualScriptComponent? canvas = Find("UI Canvas");
		if (!(IsDone(features) && IsDone(box) && IsDone(ball) && IsDone(canvas)) && frame < FrameLimit)
		{
			return false;
		}

		Check("the Features graph passes its checks", () => Expect(features, FeaturesGraph, FeaturesChecks));
		Check("the Contacts graphs count the contacts of the box and the ball", () =>
		{
			VisualScriptComponent boxGraph = Expect(box, BoxGraph, ContactsChecks);
			Assert.IsTrue(boxGraph.GetVariable<int>("CollisionsEntered") > 0, $"{BoxGraph}: no collision began");
			Assert.IsTrue(boxGraph.GetVariable<int>("CollisionsExited") > 0, $"{BoxGraph}: no collision ended");
			VisualScriptComponent ballGraph = Expect(ball, BallGraph, ContactsChecks);
			Assert.IsTrue(ballGraph.GetVariable<int>("TriggersEntered") > 0, $"{BallGraph}: it never entered the trigger");
			Assert.IsTrue(ballGraph.GetVariable<int>("TriggersExited") > 0, $"{BallGraph}: it never left the trigger");
		});
		Check("the Features graph sent to, wrote and broadcast to the Contacts graphs", () =>
		{
			// Sent 2, written 10, then the broadcast's 1 to both.
			Assert.AreEqual(11, Expect(box, BoxGraph, ContactsChecks).GetVariable<int>("Pings"), $"{BoxGraph}: pings");
			Assert.AreEqual(1, Expect(ball, BallGraph, ContactsChecks).GetVariable<int>("Pings"), $"{BallGraph}: pings");
		});
		Check("the Clicks graph counts the clicks its canvas hears", () =>
			Assert.IsTrue(Expect(canvas, CanvasGraph, ClicksChecks).GetVariable<int>("Clicks") > 0, $"{CanvasGraph}: no click reached it"));
		Check("the Features graph used the game's [VisualNode] members on this script", () =>
		{
			Assert.AreEqual(1, GraphVisits, $"{FeaturesGraph}: visits");
			Assert.AreEqual(Medal.Gold, m_Awarded, $"{FeaturesGraph}: the medal awarded");
			Assert.AreEqual(3, m_AwardedHits, $"{FeaturesGraph}: the hits of the awarded tally");
		});
		Check("C# reads and writes a graph's variables", () =>
		{
			VisualScriptComponent graph = Expect(features, FeaturesGraph, FeaturesChecks);
			Assert.IsTrue(graph.HasVariable("Score"));
			Assert.IsFalse(graph.HasVariable("Missing"));
			graph.SetVariable("Score", 40);
			Assert.AreEqual(40, graph.GetVariable<int>("Score"));
			// Read as a wider number.
			Assert.AreEqual(40L, graph.GetVariable<long>("Score"));
		});
		Check("C# raises a graph's custom events", () =>
		{
			VisualScriptComponent graph = Expect(features, FeaturesGraph, FeaturesChecks);
			Assert.IsTrue(graph.SendEvent("Add", 2));
			Assert.AreEqual(42, graph.GetVariable<int>("Score"));
		});
		Check("a graph given to an entity from C# runs", () =>
		{
			Assert.IsNotNull(m_Holder);
			Assert.IsTrue(m_Holder.HasVariable("Clicks"), "Clicks.sgraph on Visual Script Holder does not run");
			Assert.AreEqual(0, m_Holder.GetVariable<int>("Clicks"));
			m_Holder.Entity.Destroy();
		});
		return true;
	}

	private static VisualScriptComponent? Find(string entityName) => Entity.FindByName(entityName)?.GetComponent<VisualScriptComponent>();

	private static bool IsDone(VisualScriptComponent? graph) => graph != null && graph.HasVariable("Done") && graph.GetVariable<bool>("Done");

	// The graph, after checking that it ran its checks.
	private static VisualScriptComponent Expect(VisualScriptComponent? graph, string description, int checks)
	{
		Assert.IsNotNull(graph, $"{description}: the entity or its VisualScript component is missing");
		Assert.IsTrue(graph.HasVariable("Checks"), $"{description}: the graph does not run");
		Assert.IsTrue(graph.GetVariable<bool>("Done"), $"{description}: not done after {FrameLimit} frames");
		Assert.AreEqual(checks, graph.GetVariable<int>("Checks"), $"{description}: checks passed");
		return graph;
	}
}
