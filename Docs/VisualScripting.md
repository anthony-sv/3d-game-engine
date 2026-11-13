# Visual scripting guide

A visual script is a graph of nodes instead of code: a `.sgraph` asset that a `VisualScriptComponent` runs on its
entity, beside a C# script or instead of one. Its nodes are events (the callbacks C# scripts override, keys and buttons,
events of your own), flow control (branches, sequences, loops, delays), variables and functions, operators, and a node
for each member of the scripting API, so graphs reach the engine the way C# scripts do. Graphs run in the same .NET
runtime as C# scripts but need no C# project: a game can be made of graphs alone.

This guide goes the way you learn them: a first graph, the events and the flow, variables, functions, formulas, text
and arrays, timelines, graphs that talk to each other, nodes of your own and C# beside graphs; then what to do when a
graph has a problem, how to watch graphs run, how to stop one on a node and step through it with every value shown, and
how edits reach the game while it plays; then Orb Run, the starter game, and Prism Break, a whole game made of graphs,
and the editor's reference.
Each section ends with something to try. The [scripting guide](Scripting.md) covers C# scripts and testing a game,
[ScriptingAPI.md](ScriptingAPI.md) lists every member of the API that graphs call, and the graph commands of
[Automation.md](Automation.md) are what agents use to build and debug graphs.

**Try it:** open `Projects/PrismBreak/PrismBreak.sproj`, press Play, then open `Scripts/Game.sgraph` in the content
browser while it plays: the wires light up as the game runs them.

## A first graph

"+" > Visual Script in the content browser creates a graph (`New Visual Script.sgraph`, with an `Event.Create` and an
`Event.Update` node), double-clicking it opens it on a canvas in the editor (see
[Editing visual scripts](#editing-visual-scripts) below), and dragging it onto entities in the hierarchy, the inspector
or the scene view runs it there. A graph is stored as a document of nodes and links, which agents also edit through
the editor's automation commands (MCP tools, or `strada call` from a shell): `graph.library` lists the kinds of nodes
with their pins, `graph.types` the types, `graph.edit` applies operations (add nodes, connect pins, set values,
arrange) as one undo step, `graph.check` checks a whole document and `graph.set` saves one, and `graph.open` shows a
graph in its window. [Automation.md](Automation.md) describes the document and the commands. This graph, written
whole, turns its entity like the `Spinner` script of the [scripting guide](Scripting.md#a-first-script):

```json
{
	"Variables": [
		{ "ID": 1, "Name": "Speed", "Type": "float", "Value": 90.0, "Public": true, "Tooltip": "Turn speed in degrees per second" }
	],
	"Graph": {
		"Nodes": [
			{ "ID": 2, "Type": "Event.Update" },
			{ "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
			{ "ID": 4, "Type": "Operator.Multiply" },
			{ "ID": 5, "Type": "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)", "Inputs": { "Axis": [0, 1, 0] } },
			{ "ID": 6, "Type": "Get:Strada.Entity.Rotation" },
			{ "ID": 7, "Type": "Operator.Multiply" },
			{ "ID": 8, "Type": "Set:Strada.Entity.Rotation" }
		],
		"Links": [
			{ "From": [2, "Then"], "To": [8, "Execute"] },
			{ "From": [3, "Value"], "To": [4, "A"] },
			{ "From": [2, "DeltaTime"], "To": [4, "B"] },
			{ "From": [4, "Result"], "To": [5, "Degrees"] },
			{ "From": [5, "Result"], "To": [7, "A"] },
			{ "From": [6, "Value"], "To": [7, "B"] },
			{ "From": [7, "Result"], "To": [8, "Value"] }
		]
	}
}
```

Variables, functions and nodes share one numbering of IDs. A node's `Type` is a kind `graph.library` lists, its
`Settings` configure it (which variable `Variable.Get` reads) and its `Inputs` hold the values of inputs no link feeds
(the axis). Links go from an output to an input, each named by its node and pin. `Execute` and `Then` are flow pins: the
flow starts at the event and runs `Set Rotation`. The other nodes are pure: they have no flow pins and work out their
outputs whenever a node that runs needs them, here the turn from the speed and the frame's `DeltaTime`, and the current
rotation. The `Target` of an entity's or a component's member, left unlinked, is the entity the graph runs on. Nodes
need no `Position`: `graph.set` places those without one.

An agent builds such a graph with `graph.edit` instead, in a graph `graph.create` made from the Events template (node 2
is its `Event.Update`): names stand for the variables, `"$labels"` for the nodes it adds, two Expressions do the maths,
and Arrange lays it out. This batch keeps an angle of its own, because `EulerAngles` reads back one of several sets of
angles for a rotation, so adding to the angles read would stall:

```json
{ "graph": "asset://Scripts/Spinner.sgraph", "description": "Spin at Speed degrees per second", "operations": [
	{ "op": "AddVariable", "name": "Speed", "type": "float", "value": 90.0, "public": true, "tooltip": "Degrees per second" },
	{ "op": "AddVariable", "name": "Angle", "type": "float" },
	{ "op": "AddNode", "type": "Variable.Get", "settings": { "Variable": "Speed" }, "as": "$speed" },
	{ "op": "AddNode", "type": "Variable.Get", "settings": { "Variable": "Angle" }, "as": "$angle" },
	{ "op": "AddNode", "type": "Expression", "settings": { "Formula": "(angle + speed * dt) % 360" }, "as": "$next" },
	{ "op": "AddNode", "type": "Variable.Set", "settings": { "Variable": "Angle" }, "as": "$store" },
	{ "op": "AddNode", "type": "Expression", "settings": { "Formula": "vec3(0, a, 0)" }, "as": "$euler" },
	{ "op": "AddNode", "type": "Set:Strada.TransformComponent.EulerAngles", "as": "$turn" },
	{ "op": "Connect", "from": ["$angle", "Value"], "to": ["$next", "angle"] },
	{ "op": "Connect", "from": ["$speed", "Value"], "to": ["$next", "speed"] },
	{ "op": "Connect", "from": [2, "DeltaTime"], "to": ["$next", "dt"] },
	{ "op": "Connect", "from": ["$next", "Result"], "to": ["$store", "Value"] },
	{ "op": "Connect", "from": ["$store", "Value"], "to": ["$euler", "a"] },
	{ "op": "Connect", "from": ["$euler", "Result"], "to": ["$turn", "Value"] },
	{ "op": "Connect", "from": [2, "Then"], "to": ["$store", "Execute"] },
	{ "op": "Connect", "from": ["$store", "Then"], "to": ["$turn", "Execute"] },
	{ "op": "Arrange", "scope": "Graph" }
] }
```

The batch is `Tests/Data/VisualScripting/Agents/Spinner.json`, which the engine's tests send as it is shown here.

**Try it:** make the graph in a window (Tab, then "get speed", "multiply", "angle axis", "set rotation", and the wires
between them; several nodes are called Set Rotation, so pick the one whose subtitle reads "Target is Entity"), drop it
on a cube in the hierarchy and press Play; then select a second cube, give it the graph too and set its `Speed` to 180
in the inspector.

## Events, flow and waiting

| Event node | Runs |
|------------|------|
| `Event.Create`, `Event.Update` (`DeltaTime`), `Event.FixedUpdate` (`FixedDeltaTime`), `Event.Destroy` | When a script's `OnCreate`, `OnUpdate`, `OnFixedUpdate` and `OnDestroy` would |
| `Event.CollisionEnter`, `Event.CollisionExit`, `Event.TriggerEnter`, `Event.TriggerExit` (`Other`) | When the contacts of the C# methods of those names happen |
| `Event.Click` (`Button`) | When a UI button that is the entity or one of its descendants was clicked |
| `Event.KeyPressed`, `Event.KeyReleased` (setting `Key`), `Event.MouseButtonPressed`, `Event.MouseButtonReleased`, `Event.GamepadButtonPressed`, `Event.GamepadButtonReleased` (setting `Button`) | When the key or button went down or up this frame, before `Event.Update` |
| `Event.Custom` (settings `Name` and `Parameters`, which become outputs) | When a graph's `Event.Send` or a C# script's `SendEvent` raises it |

A graph has each event once (each key or button once, each custom event name once). An entity can run a C# script and a
graph: each event reaches its C# script first, and as for scripts, every script and graph of the scene exists before the
first `OnCreate` or `Event.Create`. `Flow.Delay`, `Flow.RetriggerableDelay`, `Flow.WaitFrames` and `Flow.WaitUntil` make
the flow wait without holding up the game: it goes on at their `Completed` output in a later frame, its loops and
sequences where it left them. `Flow.Tween` runs its `Update` output every frame for a duration, with an eased `Alpha`,
and `Flow.Interval` its `Tick` output at intervals.

**Try it:** add an `Event.KeyPressed` for Space to the Spinner that sets `Speed` to 0, then a `Flow.Delay` of 2 seconds
after it and a `Set Speed` to 90 at its `Completed`: the cube stops for two seconds whenever Space is pressed.

## Variables and values on entities

Public variables show in the inspector below the component's `Graph`, where each entity gives them values of its own
(the component's `Variables`, saved with the scene like script fields), so one graph serves many entities: this one
turns each at its own speed. They take the types C# fields take, named by type IDs (`bool`, `int`, `float`, `string`,
`Strada.Vector3`, `Strada.Color`, `Strada.Entity`, `Strada.Material`, enums such as `Strada.KeyCode`; `graph.types`
gives those types a `FieldType`). Graphs hold no entities themselves, as every entity that runs a graph shares it: an
entity a graph needs comes from a public `Strada.Entity` variable its component sets, from `Self`, an event's `Other` or
a lookup such as `Entity.FindByName`.

Private variables are the graph's own state, one value per entity that runs it (the Spinner's `Angle`), and a function's
locals hold values for one call. My Script lists them (below), and Details sets a variable's type, default value, tooltip
and range.

**Try it:** make the Spinner's `Speed` public with a range of 0 to 720, then drag the slider on each cube while the game
plays.

## Functions and libraries

A graph's functions are sub-graphs of their own: a `Function.Entry` gives the function's inputs and a `Function.Return`
takes its outputs. The "+" of My Script's Functions adds one, Details sets its inputs, outputs, locals and purity, and a
Call Function node calls it (double-click the call to open the function). A pure function has no flow pins and is worked
out where its outputs are read, so it holds only pure nodes (`ImpureInPureFunction` otherwise); an impure one runs where
the flow enters its call. A function that waits (a Delay or a Wait Until in it, or a call of another function that waits)
makes its calls wait too: the flow goes on after the call once the function has returned. Calls nest at most 64 deep.

A function library is a visual script whose kind is Library ("+" > Visual Script Library in the content browser,
`graph.create` with the template `Library`, or Details' Kind with nothing selected). It runs on no entity: it has no
variables and no event graph, only functions (and comment boxes). Other graphs call its functions with a Call Function
node whose Graph setting names the library; the call takes the function's inputs, outputs, purity and waiting. `Self`, and
the unlinked `Target`s inside a library function, mean the entity whose graph called it. A library is the place for
maths and helpers several graphs share.

- A library function may wait (Delay, Wait Until), send events and call other libraries' functions; Tween, Interval and
  Timeline belong in event graphs. Libraries cannot call each other in a cycle (`LibraryCycle`). A call of a library
  that has an error is an error itself (`ReferenceError`), and the graph keeps running its last working version.
- Renaming a library function's input or output keeps its former name, so graphs that still use the old name keep
  working, and their next save stores the new one. Removing an input or output, or the function, breaks its callers:
  the editor tells you which scripts call it (below).
- Each graph that calls a library runs its own copy of the library's functions. Editing the library reloads every graph
  that calls it, also while playing: running graphs swap to the new version, and an execution waiting inside a library
  function keeps waiting while the function still has its nodes. When an edit cancels one, the warning names the
  library's file ("the execution waiting in Delay (node 12) of Scripts/Motion.sgraph is cancelled: the edit disabled
  Delay (node 12) of Scripts/Motion.sgraph"). A broken library leaves the graphs that call it running the version that
  worked, and errors in a library function are logged with the library's path.
- Dropping a library on an entity is refused ("Motion is a function library: graphs call its functions; it runs on no
  entity"), and an entity whose VisualScript component names one shows that sentence instead of variables and runs
  nothing. The asset inspector says when a visual script is a function library and which scripts use it ("Used by
  Player.sgraph (3 nodes)"), and deleting a graph other scripts use names them ("they will show errors").

**Try it:** move the Spinner's turn into a pure function `Turn(Angle) -> Rotation` in a new library
`Scripts/Motion.sgraph`, and call it from the Spinner with a Call Function node.

## Formulas

An `Expression` node computes a formula such as `a * b + sin(t)`, in place of a chain of operator nodes. Every name in the
formula that is not followed by `(` becomes an input of that name, in the order the names first appear: `distance /
speed` has the inputs `distance` and `speed`, `exp + 1` the input `exp`, and `v.X` the input `v` only. `true`, `false`,
`pi` and `tau` are constants. An input you link takes the type of its link (`bool`, `int`, `uint`, `long`, `ulong`,
`float`, `double`, `Vector2`, `Vector3` or `Vector4`); an input you leave unlinked is a `float`, and you can type its value
on the node.

- Integers divide as integers (`7 / 2` is 3; `%` keeps the sign of the dividend) and floats as floats (`7 / 2.0` is 3.5).
  Mixed numbers widen as links do: `int` and `float` give `float`, `int` and `uint` give `long`; `int` and `ulong` have no
  common type, so convert one with `long(...)` or `double(...)`.
- Write `1e40d`, or any number ending in `d`, for a `double`; a float literal is at most 3.4028235e38. Vectors add,
  subtract, multiply and divide component by component, and multiply or divide by a number.
- Functions: `abs`, `sign`, `min`, `max`, `clamp`, `floor`, `ceil`, `round` (half to even), `trunc`, `frac`, `sqrt`, `exp`,
  `log`, `log2`, `log10`, `pow`, `atan2`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `radians`, `degrees`, `lerp` (not
  clamped), `smoothstep`, `step`, `length`, `normalize`, `dot`, `distance`, `cross`, and `vec2`, `vec3` and `vec4`. Casts:
  `int`, `uint`, `long`, `ulong`, `float` and `double`; a float cast to an integer is truncated and saturated, and NaN gives 0.
- `.X`, `.Y`, `.Z` and `.W` (or `.x`, ...) read a vector's components. `&&`, `||` and `?:` evaluate only what they need.
  There is no `^`: use `pow(a, b)`.
- An integer division or modulo by zero stops the execution with a message naming the part of the formula ("integer
  division by zero in a / b").
- Mistakes are reported on the node with the part of the formula they are about; messages about brackets and `?:` give
  the line and column (columns count characters, a tab is one). A graph keeps running its last working version while you
  fix a formula.
- An Expression is pure and costs one step, however long its formula. A formula has at most 4096 bytes, 1024 parts, 64
  levels of nesting and 32 inputs; a name has at most 64 bytes.

**Try it:** in the palette, type `=a*b+sin(t)` and press Enter, then link `a`, `b` and `t`.

## Text and arrays

The `Utilities/String` nodes (subtitled String) work on text: Length, Is Empty, Contains, Starts With, Ends With, Index
Of, Substring, Replace, To Upper, To Lower, Trim, Split, Join, To Int and To Float. They compare ordinally (case matters),
change case the same way in every language, count UTF-16 code units (as C#'s `string.Length` does) and never stop the
graph: Index Of clamps `StartIndex` and gives -1 when nothing is found (`""` is found at the clamped start); Substring
clamps `Start` and `Length`, and a negative `Length` takes the rest; Replace with an empty `Old` changes nothing; Split's
`Separator` is `,` by default, an empty one gives the whole text and Remove Empty drops empty parts; Join writes the
elements as text (None for no entity) with `Separator`, `, ` by default. To Int reads an optional sign and digits, To
Float a sign, digits, one decimal point and an exponent, white space around them allowed, the same in every language;
text that does not read, overflows or is not finite gives 0 with `Success` false.

Every type has an array type (`float[]`, `Strada.Vector3[]`, ...), but there are no arrays of arrays. Arrays can be
private variables, locals, function inputs and outputs, custom-event parameters, `Literal` values and type arguments.
Public variables cannot be arrays yet, as components cannot store them: keep the variable private and fill it in the
graph (Make Array from public variables, or the `Children` of a child entity that holds the points). An array is never
None: an array left unset, or one C# gives as null, is empty, and Is Valid of an array is always true. Arrays compare
element by element (NaN equals nothing), read as text like `[1, 2, 3]`, and a `Player[]` is not an `Entity[]`.

The `Utilities/Array` nodes (subtitled Array) never change an array: Set Element, Add Element, Remove Element, Insert
Element (before `Index`; an `Index` equal to the length appends) and Append Arrays (A's elements, then B's, in one copy)
give new arrays of the graph's element type, even when C# gave an array of a derived type. Make Array builds one from
`Count` elements (0 to 64) of its `Type`, or of the type its linked elements share (`int` and `float` give `float[]`);
without links, set `Type`. Contains and Index Of search an array, Length and Get read it. An array literal is a JSON
array of at most 4096 element values, and arrays made in graphs hold at most 65,536 elements. C# code that changes an
array it got from a graph changes it for the graph too: copy it first.

Generic methods whose type parameters have no constraints (`Assert.AreEqual<T>`, `AreNotEqual<T>`) take their type from
their links: an input of type `T[]` decides `T` as its element type; otherwise the inputs of type `T` join as Select's do
(`int` and `float` give `float`; `int` and an entity have no common type). An output of type `T[]` is the array of that
type. Setting `TypeArguments` fixes the types instead, arrays included; a method whose type parameter appears in no input
needs it.

**Try it:** keep a private `string[]` of three names, and on Space log the next one with `Array.Get` and a counter.

## Timelines

A `Flow.Timeline` plays curves over `Length` seconds (more than 0, at most 3600) and can `Loop`. Its tracks (at most 32)
are float, Vector2, Vector3, Vector4 or Color curves, each a data output of its type, or event tracks, each an exec output
that fires at its keys; each track has at most 1024 keys in time order. Curve keys have a time and a value, an
interpolation to the next key (Cubic, the default, Linear or Constant) and optional in and out slopes (automatic
otherwise); keys at one time make a jump. Automation writes the setting as JSON ([Automation.md](Automation.md) shows its
form); the editor's Timeline drawer edits it (below).

- Inputs: Play and Reverse continue from where it is; PlayFromStart and ReverseFromEnd jump to the start or the end;
  Stop pauses where it is; SetTime moves to `NewTime` (clamped) without firing anything. `Rate` scales the speed and is
  read when it starts playing; 0 or less holds it still. Each input ends its chain.
- While it plays, every update fires the event keys it crossed (in playing order), then `Update`. At the end of a
  timeline that does not loop, `Playing` becomes false and `Finished` fires, which may play it again. Looping timelines
  never finish: in a loop the end and the start are one moment, so playing forward a key at 0 fires every cycle and a key
  at `Length` never (backward the reverse).
- Outputs: `Time`, `Direction` (`TimelineDirection.Forward` or `Backward`), `Playing` and each curve's value; before it
  plays they hold the values at time 0.
- At most 1000 event keys fire in one update; the time beyond is dropped with one Warning.
- Timelines belong in the event graph. Editing a playing timeline changes its motion at the next frame without
  restarting it (its time is clamped into a shorter `Length`), so you can tune it live; disabling it stops it, and a
  disabled timeline passes its data inputs through by type, as every disabled node does: `Time` takes `NewTime`, the
  first float curve `Rate`, the other outputs their defaults.

**Try it:** add a Timeline with a float track that rises from 0 to 2 and back over a second, play it from `Event.Create`
with Loop on, and set the entity's height from it in `Update`.

## Graphs that talk to graphs

A VisualScript component runs one graph. To give an entity several behaviors, put each on a child entity with a
component of its own ("Spin", "Pickup", "Health"): `Self` in a child's graph is the child (`Get:Strada.Entity.Parent`
the entity it belongs to), and graphs talk through custom events. `Event.Send` runs the `Event.Custom` of the same
`Name` and parameter types in the graph of its `Target` at once, passing its inputs; a target without such an event only
logs a warning. A coin that gives its points to whatever touches it, then destroys itself:

```json
{
	"Variables": [
		{ "ID": 1, "Name": "Points", "Type": "int", "Value": 10, "Public": true }
	],
	"Graph": {
		"Nodes": [
			{ "ID": 2, "Type": "Event.TriggerEnter" },
			{ "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
			{ "ID": 4, "Type": "Event.Send", "Settings": { "Name": "Collected", "Parameters": [{ "Name": "Points", "Type": "int" }] } },
			{ "ID": 5, "Type": "Call:Strada.Entity.Destroy()" }
		],
		"Links": [
			{ "From": [2, "Then"], "To": [4, "Execute"] },
			{ "From": [2, "Other"], "To": [4, "Target"] },
			{ "From": [3, "Value"], "To": [4, "Points"] },
			{ "From": [4, "Then"], "To": [5, "Execute"] }
		]
	}
}
```

The player's graph counts them:

```json
{
	"Variables": [
		{ "ID": 1, "Name": "Score", "Type": "int", "Public": true }
	],
	"Graph": {
		"Nodes": [
			{ "ID": 2, "Type": "Event.Custom", "Settings": { "Name": "Collected", "Parameters": [{ "Name": "Points", "Type": "int" }] } },
			{ "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
			{ "ID": 4, "Type": "Operator.Add" },
			{ "ID": 5, "Type": "Variable.Set", "Settings": { "Variable": 1 } }
		],
		"Links": [
			{ "From": [2, "Then"], "To": [5, "Execute"] },
			{ "From": [3, "Value"], "To": [4, "A"] },
			{ "From": [2, "Points"], "To": [4, "B"] },
			{ "From": [4, "Result"], "To": [5, "Value"] }
		]
	}
}
```

`Event.Send` raises a custom event by name, as above. Its optional Graph setting names the script that should receive
it: with Graph set, the event reaches only an entity that runs that script (any other is skipped with a warning, once per
node and entity), and the editor checks the Send against that script's custom event. A missing script, a missing event
(with suggestions), other parameter types or a disabled event is a warning, never an error: the event is still sent by
name, so a typed Send is never required. Link its `Target`: a typed Send whose `Target` is left unlinked in a script sends
to Self, which runs this script, not the one Graph names (`TargetIsSelf`).

`Event.Broadcast` raises a custom event on every graph that has an event of that name with exactly these parameter types:
a project-wide message such as GameOver. With Graph set, it reaches only entities running that script, checked as a typed
Send is. The receivers are the instances alive when the node runs, in the order the entities started running their
graphs; the broadcasting entity is one of them unless IncludeSelf is off. Each receiver runs the event as Send's receiver
does (it may wait, send or broadcast again) and gets the arguments of the broadcast that reached it, also when a receiver
broadcasts the same event again from the same node. An entity destroyed by an earlier receiver is skipped, one started
during the broadcast is not reached, and an error in a receiver stops the broadcast and the graph that broadcast it.
`Count` is how many received it (none is normal and logs nothing). Every instance it examines costs one step of the
execution's budget of 1,000,000, and a pure function cannot broadcast.

Get Variable and Set Variable have an optional Graph setting naming another script: with it they take a `Target` entity
(Self when unlinked) and read or write that entity's variable. The variable must be public there; public is the one
access rule, and it is also what makes a variable editable on components. A graph naming itself may also use another
entity's private variables. In a script, an unlinked `Target` with Graph naming another script is an error
(`UnlinkedTarget`): Self runs this script, so link the entity to read from or write to; in a library function an unlinked
`Target` is the entity whose graph called it. While the game runs, the read or the write stops the execution with an
error when the `Target` is None, the entity runs no visual script, it runs another script ("Window (9) runs
Scripts/Window.sgraph, not Scripts/Door.sgraph"), the variable is gone ("Scripts/Door.sgraph has no variable Health any
more"), its type changed ("Door's Health is a string now, not a float") or it became private. The node finds the
variable by its ID, and by its name after a rename that changed the ID, so renaming Health in Door keeps readers working.

`Runs Graph` tells whether its `Target` (Self when unlinked) runs the script Graph names: false for None and for an entity
that runs no or another visual script. It is the guard for Get and Set of another script's variables. A library is never
run by an entity, so naming one is an error; in a script, an unlinked `Target` with Graph naming another script makes it
always false (`TargetIsSelf`): link the entity to test.

A graph is compiled with the other graphs it uses: the libraries it calls (and the libraries those call) are compiled
into it, and the graphs whose variables it reads or writes are checked by their kind and variables. Renaming or moving a
graph that others use, or one appearing or disappearing, recompiles the graphs that refer to it; changing another
graph's layout never does. A graph refers to at most 256 other graphs, its libraries' references included; the others
are treated as missing ("more than 256 visual scripts are referenced"). When an edit renames or deletes what other
scripts use, the editor says what happens to them (below), and agents' `graph.edit` and `graph.set` list those nodes in
`referrers`.

**Try it:** make the coin and the player above, then give the player's `Collected` a typed Send in the coin (its Graph
set to the player's script) and watch Details check the parameters.

## Your own nodes

Mark a public method, property, field or constructor of your game's scripts with `[VisualNode]` and it becomes a node,
in the palette's Game category (`Game/<type name>`, or the `Category` you give). Mark a class or struct and every public
member declared on it becomes one; mark an enum and graphs can use it (variables, switches, literals, public variables).
Types a marked member uses become types of graphs too.

```csharp
using Strada;

namespace Game;

public static class Combat
{
	[VisualNode(Pure = true, Category = "Game/Combat", Description = "The health a hit leaves, never below 0")]
	public static float Hit(float health, float damage) => health > damage ? health - damage : 0.0f;
}

public class Door : Script
{
	private bool m_Open;

	[VisualNode(Pure = true)]
	public bool IsOpen() => m_Open;

	// Graphs made when it was called Unlock keep working.
	[VisualNode(Title = "Open Door", FormerNames = ["Unlock"])]
	public void Open()
	{
		m_Open = true;
	}
}
```

- `Title` (1 to 128 characters) replaces the member's name in words (properties and fields: "Get <Title>" and "Set
  <Title>"); `Category` is 1 to 8 names of 1 to 64 characters separated by `/`; `Description` has at most 4096
  characters; each of `Keywords` 1 to 64. On a type, only `Category` applies, to the members that name none.
- Methods have flow pins unless `Pure = true`, which needs a result or `out` parameters; getters are pure, setters never.
- An instance member of a script class takes a `Target`, an entity (the graph's own when unlinked), and runs on that
  entity's script; on an entity whose script is of another class the graph stops with "<entity> has no <class> script".
  Script classes' constructors have no node.
- Not supported: `ref`, `in` and `params` parameters, pointers, delegates, ref structs, and members that return `Task`,
  `ValueTask` or `IEnumerator`. Members run to their end, so a member cannot make a graph wait: start the work in C# and
  give graphs a property to wait on with Wait Until (`[VisualNode] public bool IsLoaded`).
- Renaming: `FormerNames = ["Heal"]` (the same type) or `["Game.Hero.Heal"]` (moved to another type; for a constructor,
  the type's former full name) keeps graphs that use the old name working, and they store the new one at their next
  save. A former name that is another node's type is ignored, with a note.
- Every marked member that has no node, every attribute value outside its limits and every type or member whose
  attributes cannot be read (a dependency of the game that is not deployed) is a note of the node library, logged once
  when the library is built (the Console) and listed at the end of the palette's Game category ("3 [VisualNode] members
  have no node: see the Console") and by `graph.library`'s `notes`; the rest of the library builds as usual. A game type
  with the full name of a scripting API type is not a type of graphs.

**Try it:** mark a pure static method of your scripts `[VisualNode]`, build the scripts, and find it in the palette's
Game category.

## C# and graphs together

C# scripts reach an entity's graph through its `VisualScriptComponent`: `SendEvent(name, arguments)` raises a custom
event, `HasVariable(name)`, `GetVariable<T>(name)` and `SetVariable<T>(name, value)` read and write its variables
(public or not), and `Graph` is the graph it runs (`Assets.Load<VisualScript>(path)` loads one; a new graph starts at
the scene's next update). A label that shows the player's score, with a key that adds a bonus:

```csharp
public class ScoreLabel : Script
{
	[Tooltip("The entity whose visual script counts the score")]
	public Entity? Player;

	protected override void OnUpdate(float deltaTime)
	{
		VisualScriptComponent? graph = Player?.GetComponent<VisualScriptComponent>();
		if (graph is null || !graph.HasVariable("Score"))
		{
			// The player runs no graph with a score.
			return;
		}
		if (Input.IsKeyPressed(KeyCode.B))
		{
			// Runs the graph's Collected event at once, as a coin's Event.Send does.
			graph.SendEvent("Collected", 100);
		}
		GetComponent<UITextComponent>()!.Text = $"Score: {graph.GetVariable<int>("Score")}";
	}
}
```

`GetVariable<T>` and `SetVariable<T>` convert values only to their own type, a wider number (`int` to `long`, `float` or
`double`), a base class or `object`, and a component to its `Entity`. `SendEvent` converts its arguments as links
between nodes convert values, which also turns any value into text for a `string` parameter and a typed asset into its
`AssetHandle`; it returns false with a warning, running nothing, when the entity runs no graph, its graph has no such
event or the arguments do not fit its parameters. The variable methods throw: `InvalidOperationException` when the
entity runs no graph, `ArgumentException` for a variable the graph does not have, and `InvalidCastException` for a value
that does not convert. An error in an event raised from C# reaches the caller as an `InvalidOperationException`.

**Try it:** add `ScoreLabel` to a UI text, point its `Player` at the entity that counts the coins, and press B while
playing.

## When a graph has a problem

A graph that does not make sense does not run: `graph.check` and `graph.set` report every problem with the node it
concerns (the codes are listed in [Automation.md](Automation.md)), and the console names the first one
(`Visual script 'Scripts/Door.sgraph' has 2 errors; first: ...`) and each entity left without its graph
(`Entity 'Door' runs no visual script: ...`). An error while a graph runs (a member call that throws, a `Target`
without the component, an integer division by zero, more than 1,000,000 nodes in one go, calls nested deeper than 64)
stops that flow and is logged with the graph, the node and the entity; like an exception in a C# script, it counts as a
failure in test runs, and the game goes on.

The editor shows the problems as you edit (badges on nodes, the toolbar's counts and the problems drawer, below), and a
problem in a Timeline's setting offers Edit in Details, which shows the Timeline in Details with the problem under the
Settings header and the keyboard on Length, and Open Timeline, which opens the Timeline drawer on the node's tracks.
While the game plays, errors while running count too: the toolbar's problems pill adds them after a lightning bolt, a red
(error) or amber (warning) disc on the node's corner shows each with its count, and the drawer lists them in its Runtime
section (one row per node, entity and kind, with ×n for repeats, and Go to, Caller and Select Entity). A node's first
runtime error of a play session also shows a toast with Show and Break on errors (see
[Debugging a graph](#debugging-a-graph)). After Stop the section stays, as "Runtime (last run)", until the next Play or
its Clear.

**Try it:** add an Expression `a / b` to a graph's `Event.Update`, link `a` and `b` from two `int` variables, `b` being
0, and press Play: the node gets a red disc and the drawer's Runtime section says "integer division by zero in a / b".

## Watching a graph run

Open a graph's window while the game plays, or keep it open when you press Play, and it shows the graph running. The
toolbar's Instance list picks whose runs it shows: "Any instance" (every entity that runs the graph, together) or one of
them; when one entity alone runs the graph it is chosen for you.

- **Flow.** The exec wires the flow passes light up in blue, with dots that travel from output to input, and the nodes
  that run glow. A node that ran twice or more carries a count above its header ("×37"; "×12k" and "×1.2M" past 9,999):
  the runs since play started, or since the view menu's Reset Run Counts, each run once (a loop's iterations and a
  sequence's outputs each count). The light fades on the game's clock, not the wall clock: it halves every 0.25 seconds
  of game frames and holds while no frame runs, so a paused game, an execution stopped at a breakpoint or an agent's
  session that runs frames one call at a time keeps the flow of the last frames that ran, lit as they ran, until frames
  run again. Zoomed out, each comment box whose nodes run glows, so a large graph shows which of its regions are busy.
  The view menu's Show Flow While Playing turns it off.
- **Values on wires.** The view menu's Wire Values: While Stopped (the default) shows a value on each data wire whose
  source the stopped frame has (below); Always also shows the instance's values while it runs, read ten times a second
  for at most 200 wires in view; Off shows none. A value sits at its wire's middle, or slides along the wire to the
  nearest place where it covers no node and no other value. Hovering a wire or a pin shows its value ("= 34.5") in every
  mode but Off.
- **My Script's live values.** Each variable shows the instance's value between its name and its type: colours as
  swatches, entities by name (a click selects the entity), arrays (`[3] 1, 2, 3`) and strings on one line, cut before the
  type column with the whole value in the tooltip, and a flash when the value changes. While an execution is stopped,
  they are the stopped frame's values.
- **The game beside the graph.** While the game plays and the Game View is hidden (another tab in front of it, or
  closed), each graph window shows the game in a corner of its canvas, framed in the play colour. Its header reads
  "Game · Click to play" while the game does not have the input: the game takes the input only through the Game View,
  which the preview's expand button, or a click on its image, brings to the front with the input. Drag the header to
  move the preview to another corner, and its inner edge to resize it; close it, and the window's view menu shows it
  again. **Game View Beside** (the preview's button, the window's and the editor's View menus, the Game View's toolbar,
  or the hint the first time Play hides a graph window) docks the Game View above the graph window, at 0.45 of its
  height, or to its left when that shows a larger game, so both stay in view.
- **Timelines.** The Timeline drawer draws a playhead at the instance's time.

**Try it:** play Prism Break with `Scripts/Brick.sgraph` open beside the Game View (Game View Beside) and the Instance
list on Any instance: every prism the ball hits lights the Hit region, and the counts grow.

## Debugging a graph

A breakpoint stops a graph on a node while the game plays, so you can read every value there and step on from node to
node.

**Breakpoints.** F9 adds a breakpoint to the selected nodes, or removes theirs; so do a click on a node's top-left corner
(a faint ring shows there while the mouse is near) and the node's menu (Add Breakpoint). A breakpoint is a red disc on
that corner; Ctrl+F9 disables and enables it (a disabled one is a ring). Breakpoints are yours: they are kept per user
and project in the editor's user data, never in the `.sgraph` file, the undo history or an exported game. A breakpoint
goes with its node when the node is deleted or another kind of node takes its ID; moving, renaming, re-linking or
disabling the node keeps it. A breakpoint on a node of a function library stops every call of that function.

**Where it stops.** Before the node runs, at whichever input the flow enters: an event stops at the start of each
execution it starts, its outputs already set; a latent node (a Delay, a Wait Until, a Timeline's inputs) only where the
flow enters it, never when it completes; a disabled node too (the flow passes through it). A pure node has no flow: its
breakpoint stops each node that reads its value, before that node runs, and takes no condition or hit count.

**Conditions and hit counts.** Right-click the marker, or choose Breakpoint Settings... in the node's menu, to give the
breakpoint a condition: a formula that must be true for it to stop, such as `Health < 0`, `Index == 3` or
`State == "Serve"`. A condition reads the names the node can see, the first that has a name winning: the `Index` and
`Element` of the loops around the node, innermost first; the locals of its function; the outputs of the one event or
function entry every chain to the node starts from (a custom event's parameters, `Other`, `DeltaTime`, a function's
inputs); and the graph's variables. A name that is not a formula name is read with `_` for each character a formula
cannot spell, so the variable "Max Health" is `Max_Health`. Besides numbers, booleans and vectors, conditions compare
strings and enums with a string literal (`Kind != "Slow"`; an enum by its value's name) and entities with `None`
(`Target != None`). The popup checks the condition as you type and lists the names it can read (a click inserts one); an
unknown name gets the nearest one ("line 1, column 1: unknown name angle: did you mean Angle?"). A condition that does
not compile never stops (the marker turns amber with "!"); one that fails while running stops, with its message. "Stop
from hit" n lets hits 1 to n - 1 run on and stops from the n-th on. The popup counts how often the breakpoint was reached
and how often it hit since play started: reached but never hit means a condition that never holds, and an inactive
breakpoint says why ("not in the running program: the graph has errors", "no entity runs it").

**Which instance stops.** Breakpoints stop the entity the window's Instance list chose, or every entity under Any
instance. When one frame stops many at once (every enemy), the strip under the toolbar says "+29 more instances stopped
here" with Only Enemy 3, which keeps that one and lets the others go on. A chosen entity that was destroyed matches
nothing ("Instance: Enemy 3 (destroyed)").

**What stops, and what passes.** A stopped execution waits while the rest of its frame runs (other entities, C# scripts,
physics); then play pauses at the frame's end, and no frame runs until you continue. An execution that `Event.Send` or
`Event.Broadcast` started stops together with its sender, which waits inside its Send until the receiver is done. An
execution that engine code started inside another one (an `Event.Create` that `Entity.Instantiate` ran, an event a C#
script raised from a graph's call) cannot stop: it passes the breakpoint, and the strip shows its values as it passed
("Passed breakpoint at Spawn (node 4) on Bullet (77): it runs nested in engine code another execution called. These are
its values as it passed"). So do executions whose entity was destroyed. At most 64 executions are stopped at once; hits
beyond them pass and are counted. Break on errors (the Debugger panel's Breakpoints tab, or a runtime error's toast)
stops where an error that ends an execution happened, as a stop you read and dismiss with Continue.

**Stepping.** While an execution is stopped, its window comes to the front with the node in view, outlined in amber with
a triangle at the input the flow enters (or after the output it is about to leave by, "next: Then1"), and the strip
says where and why ("Stopped at Set Angle (node 8) on Spinner (…): breakpoint"). Continue (F5) runs every stopped
execution on, then the game. Step Over (F10) runs to the next node of this function, or of its caller after a return;
Step Into (F11, or Ctrl+F11, Cmd+F11 on macOS, which keeps F11 for Show Desktop) also goes into calls, library
functions and the receivers of Sends and Broadcasts; Step Out (Shift+F11) runs to the caller. Steps run between frames, never a frame, so
the game stays where it was. A step over a latent node waits for it ("Stepping: the execution waits in Delay (node 5) and
stops when it completes"): the game plays on, and the step stops when the wait completes, or the toolbar's Break gives
up. A step that ends its execution says so in the strip ("The execution ended. Continue (F5) resumes the game; Step (F10)
stops at this graph's next instruction"). Run to Here (Ctrl+F10, or the node's menu) runs to the selected node. Break,
in the main toolbar, stops the next node that runs of the graph whose window is in front, or of any graph.

**Every value.** The call stack and the values of the stopped frame are in the **Debugger panel** ("Visual Script
Debugger", docked with the Console). Call Stack lists the frames innermost first (a function, the call that led there,
the Send that sent the event, "engine code" for what engine code ran), with the event or the resumed wait that started
the execution; a click opens a frame's graph on its node. Values shows the node's inputs and outputs, the event's
outputs, the parameters, the locals, every variable, the loops' positions ("Index 2 of 0 … 9") and the other nodes this
execution computed, which also glow on the canvas: the path that led to the stop. A value this execution computed is
current; the others are dimmed: computed earlier by another execution, not computed (—), or unknown (computed before
the debugger watched this execution). "changed elsewhere" marks a value another execution changed while this one was
stopped, and "edited: 10 → 50" a literal an edit changed (below). Evaluate, under the values, reads a formula in the
stopped frame, such as `Health * 2`. Values cannot be edited from the debugger: edit the graph instead. Breakpoints lists
every breakpoint of the project, with Disable All, Remove All and Break on errors, and Instances lists the running
instances with what they wait for.

**The play controls while stopped.** The toolbar's Pause continues (as F5 does), Step is refused with the reason, and
Stop ends everything. When a stopped execution ends without being continued (an edit removed its node, its entity was
destroyed, a script loaded another scene), the game stays paused, and Continue resumes it. Test runs (`test.run`, and
the player's `--test`), the player and exported games attach no debugger: breakpoints are ignored there, and the
debugger costs a game nothing.

| Key | Does |
|-----|------|
| F9 | Add or remove breakpoints on the selected nodes |
| Ctrl+F9 | Enable or disable the selected nodes' breakpoints |
| Ctrl+F10 | Run to the selected node (while playing) |
| F5 | Continue: the stopped executions, then the game |
| F10 | Step over |
| F11, Ctrl+F11 | Step into (Cmd+F11 on macOS, which keeps F11 for Show Desktop) |
| Shift+F11 | Step out |

F5, F10, F11, Ctrl+F11 and Shift+F11 work wherever the keyboard is while an execution is stopped, but never behind a
dialog. When the stop paused the game, the editor takes the keyboard back from the game; Continue gives it back when
the Game View shows.

**An agent's session.** Agents debug through the editor's automation commands (MCP tools). This session
(`Tests/Data/VisualScripting/Agents/Debugging.json`, which the engine's tests run headless and through `strada mcp`)
makes the Spinner with a mistake, `% 36` for `% 360` (it shakes instead of turning), finds the mistake with a
breakpoint while the game is paused, fixes it while the execution is stopped, checks the fix and undoes and redoes it
after Stop. Each call is a tool with its arguments, followed by what it answers (the members the session checks):

```
graph_create {"path":"Scripts/Spinner.sgraph"}
  → {"path":"Scripts/Spinner.sgraph"}
graph_edit {"graph":"asset://Scripts/Spinner.sgraph","description":"Spin at Speed degrees per second","operations":[{"op":"AddVariable","name":"Speed","type":"float","value":90.0,"public":true,"tooltip":"Degrees per second"},{"op":"AddVariable","name":"Angle","type":"float"},{"op":"AddNode","type":"Variable.Get","settings":{"Variable":"Speed"},"as":"$speed"},{"op":"AddNode","type":"Variable.Get","settings":{"Variable":"Angle"},"as":"$angle"},{"op":"AddNode","type":"Expression","settings":{"Formula":"(angle + speed * dt) % 36"},"as":"$next"},{"op":"AddNode","type":"Variable.Set","settings":{"Variable":"Angle"},"as":"$store"},{"op":"AddNode","type":"Expression","settings":{"Formula":"vec3(0, a, 0)"},"as":"$euler"},{"op":"AddNode","type":"Set:Strada.TransformComponent.EulerAngles","as":"$turn"},{"op":"Connect","from":["$angle","Value"],"to":["$next","angle"]},{"op":"Connect","from":["$speed","Value"],"to":["$next","speed"]},{"op":"Connect","from":[2,"DeltaTime"],"to":["$next","dt"]},{"op":"Connect","from":["$next","Result"],"to":["$store","Value"]},{"op":"Connect","from":["$store","Value"],"to":["$euler","a"]},{"op":"Connect","from":["$euler","Result"],"to":["$turn","Value"]},{"op":"Connect","from":[2,"Then"],"to":["$store","Execute"]},{"op":"Connect","from":["$store","Then"],"to":["$turn","Execute"]},{"op":"Arrange","scope":"Graph"}]}
  → {"summary":{"errors":0},"labels":{"$next":7,"$store":8,"$turn":10}}
entity_create {"name":"Spinner","components":{"VisualScript":{"Graph":"asset://Scripts/Spinner.sgraph"}}}
  → {}
play_start {"paused":true}
  → {"state":"play","paused":true}
graph_breakpoints {"graph":"asset://Scripts/Spinner.sgraph","add":[{"node":8,"condition":"Angle > 33.5"}]}
  → {"breakpoints":[{"node":8,"title":"Set Angle","condition":"Angle > 33.5","checked":true}]}
play_advance {"frames":60}
  → {"framesRun":24,"stopped":true,"stop":{"reason":"Breakpoint","frames":[{"node":8,"pin":"Execute"}]}}
graph_debug {}
  → {"state":"Stopped","stops":[{"id":1,"frames":[{"node":8,"title":"Set Angle","functionName":"Event Graph"}],"start":{"kind":"Event","node":2}}]}
graph_values {"graph":"asset://Scripts/Spinner.sgraph","stop":1,"nodes":[7]}
  → {"source":"Stop","variables":[{"name":"Speed","value":90},{"name":"Angle","value":34.5}],"nodes":[{"node":7,"outputs":[{"name":"Result","value":34.5,"currency":"Earlier"}]}]}
graph_step {"kind":"Over"}
  → {"outcome":"Stopped","stop":{"id":2,"frames":[{"node":10}]}}
graph_values {"graph":"asset://Scripts/Spinner.sgraph","stop":2,"nodes":[7]}
  → {"nodes":[{"node":7,"outputs":[{"name":"Result","value":0,"currency":"Current"}]}],"variables":[{"name":"Angle","value":0}]}
graph_edit {"graph":"asset://Scripts/Spinner.sgraph","operations":[{"op":"SetFormula","node":7,"formula":"(angle + speed * dt) % 360"}]}
  → {"changed":true,"live":{"edits":[{"outcome":"Applied","instances":1,"instanceNotes":[{"stopsKept":1}]}]}}
graph_debug {}
  → {"state":"Stopped","stops":[{"id":2,"migrated":true}]}
graph_breakpoints {"graph":"asset://Scripts/Spinner.sgraph","clear":true}
  → {"breakpoints":[]}
graph_continue {}
  → {"outcome":"Ended","paused":true}
play_advance {"frames":30}
  → {"framesRun":30,"stopped":false}
graph_trace {"frames":10,"graphs":["asset://Scripts/Spinner.sgraph"],"nodes":[8]}
  → {"framesRun":10,"summary":{"graphs":[{"steps":[{"node":8,"exit":"Then","count":10}],"values":[{"node":8,"pin":"Value","direction":"In","count":10,"min":46.5,"max":60}]}]}}
graph_values {"graph":"asset://Scripts/Spinner.sgraph"}
  → {"variables":[{"name":"Angle","value":60}]}
play_stop {}
  → {"state":"edit"}
editor_undo {}
  → {}
graph_get {"graph":"asset://Scripts/Spinner.sgraph"}
  → {"document":{"Graph":{"Nodes":[{"ID":7,"Settings":{"Formula":"(angle + speed * dt) % 36"}}]}}}
editor_redo {}
  → {}
graph_get {"graph":"asset://Scripts/Spinner.sgraph"}
  → {"document":{"Graph":{"Nodes":[{"ID":7,"Settings":{"Formula":"(angle + speed * dt) % 360"}}]}}}
```

`play_start {"paused":true}` makes the session exact: no frame runs until `play_advance`, which runs frames of 1/60 s.
The breakpoint on Set Angle (node 8) stops in frame 24, when the angle has reached 34.5; `graph_values` of the formula
(node 7) shows 34.5, computed in the frame before. One step over, the formula gives 0, where 36 would be right: the bug,
seen. The fix reaches the stopped execution at once (`stopsKept: 1`, `migrated: true`), the breakpoints are cleared
before the game goes on, and `graph_trace` shows Set Angle taking 46.5 to 60 over ten frames.

**Try it:** in `Scripts/Game.sgraph`, set a breakpoint with the condition `Combo >= 3` on the event that starts the region
"Bricks: points by the combo and the level, then a clear", play Prism Break until it stops, and read the score's parts in
the Debugger panel; then Step Over through the region.

## Editing while playing

Keep the graph window open while the game plays. Every edit reaches the running game in the call that made it, before
the next frame: also while the game is paused, and into an execution stopped at a breakpoint. Moving, commenting and
arranging nodes send nothing. The toolbar's LIVE pill says that the game runs what you see. When an edit leaves an
error, it turns amber ("LIVE · last working version"): the game goes on with the last version that worked until you fix
it, and the canvas outlines in dashed amber the nodes and wires the game does not run yet. A click on the pill opens the
Live popover: since when the game runs this version, what of the graph it does not run (with Show first error and the
outline's switch), the last edit's receipt in full, the runtime problems, and for a function library what each graph
that calls it runs.

**What carries over.** Variables keep their values (matched by ID, then by name and type; a new variable, or one whose
type changed, starts at its `Value`), and a value the entity's component stores for a variable the edit adds waits until
it is edited or a new instance starts. Nodes keep their state (a DoOnce that ran, an open Gate) while their kind stays
and they stay enabled; disabling a node counts as removing it. Waiting nodes keep waiting while they still exist and
are enabled; otherwise the wait is cancelled with a warning. A Timeline keeps its time, clamped into a shorter Length.

**Receipts.** Each edit toasts what it did to the game: "Live: applied to 1 instance", with notes such as "Angle reset
(type changed)", "Ammo starts at its value", "1 wait cancelled", "the stopped execution continues with the edit" and "1
Timeline shortened", or "Live: not applied: 1 error (first: ...); the game keeps running the last version that worked".
For four seconds after an edit, discs on the nodes show where it cancelled a wait or reset a node's state. An edit of a
library reports for the graphs that call it, and their windows stay silent. While a step of the debugger runs inside
the old version, an edit waits for it ("applies when the running step ends"). Loads that are no edit (an entity that
starts the graph mid-play, a scene loaded) toast nothing. `graph.edit` and `graph.set` answer the same receipts in their
result's `live`, and `graph.live` lists them with the runtime problems ([Automation.md](Automation.md)).

**Into a stopped execution.** An edit that keeps the stopped node reaches the stopped execution: it continues with the
edited graph, the Debugger panel marks a literal the edit changed ("edited: 10 → 50"), and a step uses the new value. An
edit that removes the stopped node, gives it another type, disables it or changes the signature of the function it
stopped in cancels the execution with a warning; the game stays paused, and Continue resumes it. An edit that leaves an
error lets the stopped execution go on with the last version that worked, which the strip says.

Edits made while playing stay in the script after Stop, and stay undoable.

**Try it:** while the Spinner turns, change its formula to `(angle + speed * dt * 4) % 360`: the toast says "Live: applied
to 1 instance", and the cube turns four times as fast at once, keeping its angle. Undo gives the formula back just as
fast.

## A first game: Orb Run

The template Graph Starter (`graph-starter`, as `project.create`'s `template` names it) makes a project of the game Orb
Run, made of five graphs (and two for its tests) and no C#. WASD, the arrow keys or a gamepad's left stick roll a
glowing orb across a square arena to collect 12 sparks before 35 seconds run out; the last ten seconds tick in red, and
Space, Enter or a gamepad's A or Start plays again after the win or the loss. It is small enough to read in one
sitting, and each graph shows a few ideas of this guide:

| Graph | Runs on | What it shows |
|-------|---------|---------------|
| `Orb.sgraph` | Orb | A pure function (`Steer`) that reads the keys and the stick, physics in `FixedUpdate`, a sound that follows the speed, and a broadcast event it listens to |
| `Spark.sgraph` | every spark | Bobbing with formulas, and a trigger that checks what entered with Runs Graph before it broadcasts to the Game |
| `Game.sgraph` | Game | The rules: functions that place the sparks and start, win and lose a run, the clock, and the events that play again |
| `Hud.sgraph` | Screen | Remote Gets of the Game's variables, guarded by Runs Graph, a Timeline that fades the hint, and the win and lose screens |
| `CameraRig.sgraph` | Camera Rig | A camera that eases after the orb |
| `Checks.sgraph` | a function library (Tests calls it) | Reporting a check and comparing numbers and points |
| `Tests.sgraph` | Tests (the test scene) | 13 checks of the rules and the screen, in the form Prism Break's checks take |

Its graphs read as Prism Break's do, and `Templates/Tools/AuthorTemplates.py` built it through the agent tools. The
template C# Starter (`csharp-starter`) is the same game written in C#, with one graph: `Scripts/Hover.sgraph` floats
each spark's gem where `Spark.Hover`, a C# method marked `[VisualNode]`, puts it (the node
`Call:Game.Spark.Hover(float)`, see [Your own nodes](#your-own-nodes)).

**Try it:** in a project made from Graph Starter, select the Game entity and lower its `TimeLimit`, or the Orb and raise
its `Push`, then press Play; open `Scripts/Orb.sgraph` while it plays and watch Steer's wires light up as you roll.

## A complete game: Prism Break

`Projects/PrismBreak` is a neon brick breaker made of 13 visual scripts and no C#: no `Scripts` folder, no C# project.
The title shows PRISM BREAK over an arena that plays itself. Space or a click starts a game; the ball rests on the
paddle until Space or a click launches it; the mouse, A and D or the arrow keys move the paddle, and Escape pauses. The
ball breaks prisms of 1 to 4 hits, which glow cyan, magenta, gold and white by the hits they have left: each hit chimes a
semitone higher for every prism of a combo, the prism flashes, the six lights pulse in its colour and the camera kicks,
and a broken prism bursts into shards. A capsule sometimes falls from it: W widens the paddle for 8 seconds and S slows
the ball for 6. Points are 10 per hit times the combo's factor and the level; the combo resets at the paddle. The drain
takes a life; a cleared level runs a rainbow chase and a banner while the next layout builds brick by brick (levels of
33, 34 and 44 prisms, in turn); after three lives the screen shows the top five. Open
`Projects/PrismBreak/PrismBreak.sproj` and press Play.

| Graph | Runs on | What it does (open it to see) |
|-------|---------|-------------------------------|
| `Arcade.sgraph` | a function library (Game, Brick, Hud and Tests call it) | The arcade rules every graph shares: scores, the prisms' colours, the chimes' notes and pitches, digits in groups and the top-five table |
| `Ball.sgraph` | every ball | Keeps the level's speed and never flies flat, the paddle aims it, and walls tick |
| `Brick.sgraph` | every prism | Glows in the colour of the hits it has left, chimes and flashes when the ball hits it, and bursts into shards when it breaks |
| `CameraRig.sgraph` | Camera Rig | Hits shake it, the shake settles, and it sways while the title plays itself |
| `Checks.sgraph` | a function library (Tests calls it) | What the test scene's checks share: reporting a check and comparing numbers, vectors and tables |
| `Drain.sgraph` | Drain | A ball that falls in behind the paddle is lost |
| `Game.sgraph` | Game | The match: builds the levels, serves and launches the ball, scores the prisms, counts lives and combos, clears levels, ends the game with its top five, pauses, and plays itself on the title |
| `Hud.sgraph` | Screen | Reads the game's score, level, combo and lives, and shows the title, the prompts, the banner, the pause screen and the top five |
| `Lights.sgraph` | the six lights | Pulses in the colour of every prism the ball hits, runs a rainbow chase when a level is cleared and flashes red when a life is lost |
| `Paddle.sgraph` | Paddle | The mouse, the keys or the autopilot steer it, a Widen capsule makes it wider for a while, and it squashes when the ball hits it |
| `PowerUp.sgraph` | every capsule | The paddle catches it for its power, or it falls out |
| `Shard.sgraph` | every shard | Bursts out, then shrinks away |
| `Tests.sgraph` | Tests (the test scene) | 29 checks of the game's rules, physics, screen and flow, one after another |

Every feature of the language has a job there: the levels are `string[]` layouts read with Split, Substring and To Int,
the live balls an `Strada.Entity[]` and the top five an `int[]`; formulas keep the ball's speed and angle, aim it off the
paddle, shake the camera and pitch the chimes; Timelines flash the prisms, widen the paddle, slow time, pop the combo and
run the lights' chase; the libraries hold the rules the graphs share; typed Sends and Broadcasts carry hits, lost balls,
state changes and level clears between graphs, and remote Gets, guarded by Runs Graph, let the screen read the game.
Game.sgraph's functions are BuildLevel, Serve, LaunchBall, StartAttractMode, LevelClear, ClearBricks, BeginGame, GameOver
and SpawnCapsule. Every hit juices the arena: the prism sends the Game BrickHit with its colour, and the Juice region
broadcasts Pulse to the six lights and sends Shake to the camera rig. While the title plays itself (the Game's read-out
Attract), a cleared level keeps the title on screen. A player who presses Space or clicks at any moment of the demo, even
while it waits to serve again or to build its next level, gets a game of their own: every wait of the demo checks Attract
again before it goes on. The demo's points never count towards BEST. The keys steer a target that never passes the
walls, so the paddle turns round at once. Its development-only nodes are cheat keys (1 and 2 drop a W or an S capsule,
N clears the level), which run in the editor and are left out of the exported game.

**How its graphs read.** One region per behaviour, framed by a comment box titled "<What>: <how>" or a short noun phrase
(at most 72 characters) in the colour of its role: blue for events and input, orange for rules, purple for effects,
green for the title, red for what runs in development only. Regions stack from top to bottom in reading order, and the
nodes of a region run from left to right. Regions share no nodes and no links: each has its own Get nodes and calls.
Node comments say why where the reason is not plain; events are named for what happened or what to do, functions as
verbs, and a formula's inputs come after the values they read. Every graph has a description and every public variable a
tooltip. Prism Break's graphs are tested to read well: SampleGraphsTests checks that every node sits in exactly one
titled comment box, at least 16 units from any other node; that no link crosses between boxes; and that arranging a box
changes nothing. After a change to arranging, re-run `Projects/PrismBreak/Tools/Author.py` (for the Graph Garden,
`Projects/Showcase/Tools/Author.py --exhibit GraphGarden`, never the Showcase's full rebuild).

**How it was made.** `Projects/PrismBreak/Tools/Author.py` built the whole project through `strada mcp`, as an agent
does: the project and its settings, the sounds, meshes and textures `Tools/GenerateAssets.py` makes, the materials, then
every graph. It declares first: one `graph_edit` batch per graph adds its variables, custom events and functions with
their inputs and outputs, so the graphs that send to, read from and call each other never meet a missing event. Then one
`graph_edit` batch per region adds the region's nodes and links, arranges them (`Arrange` with scope `Created`, then
`Nodes`) and frames them with a titled `AddComment`, and every batch must answer no error and no warning. It writes a
graph's functions before the regions that call them. Then the prefabs, the scenes, a check of every graph, the test run
and an export.

**How it is tested.** `Scenes/Tests.sscene` runs `Tests.sgraph`: 29 checks, one after another, from a Sequence in the
event graph. Each check is a latent function in its own box. It acts, notes the time, waits with Wait Until until what
it expects comes true or its time limit passes, and reports through the Checks library, with a detail of what it saw.
Physical checks never depend on the ball's path: they teleport the ball in front of a prism they find by its row and
column. Two self-sent events watch the level banner and the camera from the moment the level is cleared. Run it with
`StradaRuntime --project Projects/PrismBreak/PrismBreak.sproj --scene Scenes/Tests.sscene --test` ("Tests passed: 29
checks"). The player also runs the title and the exported game. The editor's play mode is driven through `input.set`:
the title plays itself, Space starts and serves, the mouse steers, Escape pauses, and a lost ball costs a life; play mode
also starts games while the title clears a level and right after it lost its ball, and checks that the player keeps
their game, that BEST stays 0 and that A turns the paddle at the right wall at once. The fuzz test of damaged graphs
includes the match graph, Game.sgraph.

**Try it:** open `Scripts/Ball.sgraph` while it plays and change `minZ` in the formula of "Keep the speed and the angle:
never slower, never flat"; then set a breakpoint on the Bounce off the paddle region's Send and step through a bounce.

## Editing visual scripts

**Opening a script.** Double-click a visual script in the content browser (or use its Open menu item, the Open button
of the inspector that shows it, or Open Visual Script on a VisualScript component) and it opens in a graph window of
its own, docked beside the scene view. An entity without a graph gets one from its component's Create Visual Script,
which writes `Assets/Scripts/<entity name>.sgraph`, runs it on the selected entities and opens it. Each script has one
window, and the editor reopens the windows a project had open the next time it opens. The window holds the canvas in
the middle, **My Script** on the left (the script's graphs, variables, locals and custom events), **Details** on the
right (what is selected) and a toolbar; View > Visual Scripts lists the open windows. Scripts save themselves: every
edit is written to the file at once, the toolbar says "Saved" (or "Not saved" when a write failed), and Ctrl+Z undoes it
like any other edit, from any window. A script whose file is read-only can be looked at and copied from, not edited.

**Moving around.** Drag with the right or middle mouse button, or hold Space and drag, to pan. With a wheel mouse the
wheel zooms to the cursor, Ctrl+wheel pans up and down and Shift+wheel sideways. On a trackpad, open the toolbar's view
menu (the three dots) and set Scroll to Pan: two fingers then pan, and Ctrl+scroll (Cmd on macOS) zooms; Pan is the
default on macOS. F frames the selection (everything when nothing is selected) and Home everything. Zoomed far out,
nodes become simple boxes you can still select and move; zoomed in, every value shows on its node. Back and Forward in
the toolbar (Ctrl+[ and Ctrl+], or the mouse's side buttons) retrace where you looked, across functions too.
Ctrl+Alt+1 to 9 stores the view as a bookmark and Ctrl+1 to 9 goes back to it; the toolbar's Bookmarks menu renames
and deletes them. Bookmarks, views and pane sizes are yours alone: they are kept in the editor's user data, never in the
script.

**Adding and wiring nodes.** Press Tab, tap Space or right-click the empty canvas to open the palette, type part of a
node's name ("add force", or the initials "af") and press Enter: the node appears where you opened the palette. Holding
Space and typing opens it with what you typed. Drag from a pin to another pin to link them; the wire turns red and
its tooltip says why when the two cannot connect; a wire shows a gradient where a value converts on its way (an `int`
into a `float`). Release a wire on empty canvas and the palette lists only the nodes that connect to it, and links the
one you choose. Ctrl+drag a linked pin to move its links, Alt+click a pin or a wire to break links, Ctrl+right-drag
across wires to cut them, and double-click a wire to add a reroute point. Select with a click or by dragging a rectangle
(Shift adds; Ctrl toggles a clicked node and removes what a rectangle covers), move the selection by dragging (it snaps to the grid; hold Alt to place it freely), nudge it
with the arrow keys, and tidy it with the toolbar's Arrange or the align keys (Shift+W, A, S, D) and Q, which
straightens a flow. Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+D copy, cut, paste and duplicate, also between scripts. Delete
removes the selection and keeps the flow going through what it removed (a node between two others no longer breaks the
chain); Shift+Delete removes it without that. F1, or `?` in the toolbar, lists every key and mouse gesture.

**Values.** An input that nothing is linked to shows its value on the node; click it to change it (drag a number,
pick an enum, a colour or an asset), or select the node and change it in Details. Drop an asset from the content browser
onto an input to use it, or onto the canvas to get a value node holding it; drop an entity from the hierarchy onto the
canvas to give the graph a public variable for it, set on the entity this window shows (below). Right-click a pin for
Reset to Default, Promote to Variable and its links.

**Variables, functions and custom events.** The "+" of each section of My Script adds a variable, a function, a local
(while a function is shown) or a custom event and lets you name it at once. Drag a variable onto the canvas for a Get
or a Set (hold Ctrl for a Get, Alt for a Set), or onto a pin. Details sets a variable's type, default value, tooltip
and range and whether it is public (public variables show in the inspector, where each entity sets its own values); a
function's inputs, outputs, locals and purity; a custom event's parameters. Double-click a function call to open the
function's graph, and a Send to go to the custom event it raises in this script. Deleting a variable
or function that nodes use asks first and removes those nodes too, keeping the flow through them. Renaming keeps every
use, and a variable remembers its former names, so the values entities stored keep applying.

**Comments.** Press C to put a comment box around the selection (or a new one at the cursor) and type its title; moving
the box by its title moves what is inside it (hold Alt for the box alone). Right-click a node and choose Add Node
Comment... to add a note shown above it (Edit Node Comment... once it has one); double-click either text to edit it in
place. H collapses selected nodes to their header.

**Split pins.** Right-click a vector, colour or other struct pin and choose Split Struct Pin to show one pin per field
(a `Vector3` input becomes `X`, `Y` and `Z`), so you can link or set each field on its own; Recombine Struct Pin puts it
back. Outputs split the same way (a raycast's hit point, then its `Y`).

**Disabling nodes.** M disables the selected nodes (or enables them again): a disabled node is dimmed and passes the
flow on to its next node without running, and passes its values through where it can, so you can switch part of a graph
off without unlinking it. A disabled node's errors become warnings, so disabling a broken node keeps its graph running.
Details also offers Development Only: such a node runs in the editor and in test runs, and is left out of exported
games (a log line or a cheat key). Entry and Return nodes and nodes without flow pins cannot be disabled.

**Problems and quick fixes.** Nodes with problems get a badge, and the toolbar counts errors and warnings; click either
to see the messages, and their fix buttons where a fix is known: use the suggested node or pin, insert a conversion,
keep the first of two links, add a missing Return, and more. A script with an error does not run (while playing, the
game keeps the last version that worked), so fix errors first; warnings can wait. The problems drawer under the canvas
lists everything, errors first; click a row to go there.

**Find.** Ctrl+F finds text in the script (titles, values, comments, names): Enter goes to the next match and selects
it, and the find field keeps the keyboard; Escape closes the bar and gives the keyboard back to the canvas, where
Delete, F2, Ctrl+X or Ctrl+D act on what you found. Ctrl+Shift+F searches every script of the project. Find References
on a variable, Find Callers on a function and Find Senders on a custom event (in their right-click menus in My Script)
list their uses; senders are found in every script.


**Watching and debugging.** While the game plays, a window lights the flow and shows values ([Watching a graph
run](#watching-a-graph-run)); its view menu holds Wire Values (Off, While Stopped, Always), Show Flow While Playing, Reset
Run Counts, Game Preview and Game View Beside. F9, Ctrl+F9 and Ctrl+F10 act on the selection; the node's menu has Add
Breakpoint, Enable Breakpoint (or Disable), Breakpoint Settings... and, while playing, Run to Here; a breakpoint's
marker takes a click (add or remove) and a right click (its settings), and dragging from it moves the node. While an
execution is stopped, the strip under the toolbar says why, with its buttons (Continue, the steps, Show, Only <entity>),
the LIVE pill gains a STOPPED segment, and the main toolbar's Continue, Step Over, Step Into, Step Out and Break (their
tooltips name their keys) act as F5, F10, F11 and Shift+F11 do. Following a stop opens the stopped frame's graph (a
library's own window for a library function) with the node in view; however many steps it follows, Back returns to
the view from before debugging. The Visual Script Debugger panel (the View menu shows it; it comes to the front at the
first stop) has the tabs Call Stack, Values (with Evaluate), Breakpoints and Instances
([Debugging a graph](#debugging-a-graph)).

**Editing while playing.** Edits reach the running game at once, and their receipts, the Live popover and the outline
of what does not run yet are described in [Editing while playing](#editing-while-playing). The toolbar's Instance list
picks which entity running the script the window shows ("Any instance" while playing): My Script shows its values, and
dropped entities set its values.

**Formulas.** Type a formula in an Expression's Formula field in Details (a monospace field of a few lines), or
double-click the node's header to type it on the node. Ctrl+Enter, Enter on the node, or leaving the field commits it;
Escape cancels. Retyping a name renames the input and keeps its link, and the Inputs row under the field lists the
inputs with their types. While only numbers and operators change, a running game gets the formula as you type.
Problems show under the field with `^` under the part they are about; a click selects that part, and "Use sin" fixes a
misspelled function in place. A formula Details cannot take (over 4096 bytes) is refused with "The formula stays:
<why>"; after Ctrl+Enter the text stays in the field to be fixed. In the palette, type `=` and a formula
(`=a*b+sin(t)`) to add an Expression of it.

**Arrays.** Details edits an array's elements: a variable's or local's default, a Literal's value and an unlinked array
input (+ Add Element; drag an element's handle onto another handle of the same array to move it there, while a drop
onto another array's handle moves nothing; x removes it). Make Array's Count adds and removes element pins, keeping their
links. On the canvas a chip shows an unlinked array input's length. My Script's eye refuses to make an array public
("Arrays cannot be public: components cannot store them"), and the inspector lists a public array a graph declares as
read-only ("arrays cannot be set on components").

**Timelines.** Double-click a Timeline's header, choose Edit Timeline in its menu, or click Edit on a track in Details,
to open its tracks and curves in the drawer under the canvas. On the left, + Add Track adds a Float, Vector2, Vector3,
Vector4, Color or Event track; double-click a name to rename it (its links follow); its menu has Rename, Change Type and
Remove; the eye hides a track, and X Y Z W (R G B A) hide components. On the right, click a key to select it (Shift adds,
Ctrl toggles), or drag on empty space for a rectangle. Drag keys to move them as a block: times snap to 0.01 s (Alt:
free), Shift keeps one axis, and a block stops at the next key that is not selected and at the timeline's ends. Drag a
selected cubic key's handle to set both its slopes, or with Alt only that one. Double-click a curve to add a key with
the curve's value there, or a lane to add an event. Right-click a key for Interpolation (Cubic, Linear, Constant),
Tangents (Auto, Flat) and Delete. The wheel zooms time, Ctrl+wheel zooms values, a right or middle drag pans, and F
frames the selected keys (everything without any). While the drawer has the keyboard, Delete removes keys, Ctrl+A selects
the visible tracks' keys, and the arrows move the selected keys by 0.01 s (Shift: 0.1 s); presses less than half a
second apart are one undo step. Esc while dragging keys or a slope cancels the drag (the keys go back where they were and
no undo step is left), Esc during a rectangle gives back the selection you had, and otherwise Esc gives the keyboard back
to the canvas. The selection, the eye and the component toggles follow tracks that are renamed, or removed elsewhere
(Details, undo, an agent), and tracks with very large values (up to the largest floats) frame and draw like any other.
Details shows the Length (lowering it moves later keys onto the new end in the same step, and dragging it back up
restores them), Loop, and a row per track with a preview. While playing, a dragged key reaches the running timeline at
the next commit, within 0.1 s, without restarting it.

**Other graphs.** Nodes that name another graph get it in Details: the Graph picker lists the project's scripts or
libraries, as the setting takes, with "None (this script)" or "None (any receiver, by name)" first (a graph the project
no longer has shows "Missing (8146…)"). With a Graph, the Variable list shows that script's public variables (every
variable when it is this script), the Function list the library's functions, and Send and Broadcast get an Event list
that sets their Name and Parameters. Each change is one undo step that removes only the links whose pins went ("Set
Graph on Send Hit (removes 1 link)"); None makes a Send untyped and keeps its parameters. With nothing selected, Details
shows the script's Kind (Script or Function Library): making a script with variables or event nodes a library, or a
library other scripts call a script, asks first and says what will show errors ("Make Motion a script? 3 nodes in 2 other
scripts call its functions: they will show errors until fixed."), and a toast says it again afterwards. A public
variable shows "Used by N nodes in M other scripts" and a library's function "Called by N nodes in M scripts", with
[Find Callers]. The palette lists the functions of the project's libraries (under Libraries/<name>), the other scripts'
typed Send and Broadcast of their events, Get and Set of their public variables and Runs <name> (under
Scripts/<name>), an untyped Broadcast of this script's events, and "Get/Set X of another <script>"; typing a category
word finds entries ("health door" finds Get Health). A library's palette offers no variables or custom events of its
own. Double-clicking a node that names another graph, or its menu's first item, opens that graph's window: "Open Clamp01
in Motion", "Go to Event Hit in Door", "Select Variable Health in Door" (which selects it there), or "Open Door" when
that graph lacks what the node names; a node naming a graph the project does not have shows in Details. Back in that
window returns to the caller's window and view (opening it again if it was closed), and Forward goes there again. F2 and
Find References on such a node show nothing of this script; Find References lists the other graph's element across every
script. Renaming a custom event renames this script's untyped Sends and Broadcasts and those typed with this script.

After an edit that changes what the graph offers others, a toast says what happens to their nodes, with a button that
lists them: "Renamed Value to Amount: 3 calls in 2 other scripts follow it", "Deleted Clamp01: 1 call in 1 other script
show errors", "Made Speed private: 2 nodes in 1 other script show errors" (Find Callers, Find References or Find
Senders). Deleting a variable, function or event other scripts use asks first, counting them: "2 nodes here use it
(they are deleted too). 3 nodes in 2 other scripts use it: they will show errors until fixed.", with [Delete], [Find
References] and [Cancel]. A library's window opens on its first function, and My Script shows "Function library · used
by N nodes in M scripts" and its Functions (an event graph, variables or custom events left in it are shown so they can
be deleted, with no + to add more); an empty library shows "This library has no functions yet." with [Add Function].
While playing, a broken library's pill reads "LIVE · last working version", and its tooltip says that the graphs that
call it keep running the last version that worked.

**The window's layout.** My Script and Details stand beside the canvas when both fit with a canvas of 40 font sizes;
when the window is narrower, they stack in one column on My Script's side (My Script above Details, a splitter between
them), and in a window narrower still Details and then My Script hide and open over the canvas when needed. At the
farthest zooms comment titles are drawn above the nodes, on a plate of the comment's colour, so the regions of a large
graph stay readable; a title starts at the canvas's edge when its box starts beyond it. Details' Align and Distribute
buttons wrap onto the next line when the pane is too narrow for them, so all six alignments stay usable in the stacked
column.

**Edits made elsewhere.** An agent's or another window's edit lands in an open window at once, and its changed nodes
flash. A value you are dragging or typing in Details or My Script survives an edit of other elements; when the edit
changed the same element, your value is dropped with "Your edit of Duration was replaced by a change made elsewhere". A
gesture on the canvas survives an edit that touches none of its elements: a move, a comment's resize, a wire and a
relink go on, and so do marquees and cuts. One whose elements the edit touched ends without an edit, and the window says
why: "Your move was interrupted: Branch (node 12) was changed elsewhere". The history then holds the outside step, then
yours.

**The Edit menu's keys.** Cut, Copy, Paste, Duplicate, Delete and Select All, in the Edit menu and as keys (Ctrl+X,
Ctrl+C, Ctrl+V, Ctrl+D, Delete or Ctrl+Backspace, Ctrl+A), act on the graph window focused last whose tab is in front,
wherever the keyboard is (the console, for one), and never on the entities then; with the scene as the target they act
on the entities. A window keeps the keys it claims while it has the keyboard: the content browser's Delete,
Ctrl+Backspace and F2 act on its selected file, and the Edit menu then shows no shortcut for Delete. While a dialog is
open (a delete confirmation, the unsaved-changes prompt), no editing key runs, Undo and Redo included, so nothing
changes what the dialog acts on.

**Try it:** open Prism Break's `Scripts/Game.sgraph`, press Home to see every region, zoom into "Bricks: points by the
combo and the level, then a clear" and store the view with Ctrl+Alt+1; pan to "Pause: Escape freezes the game and
shows the pause screen", then press Ctrl+1 to come back to the bricks, and Back (Ctrl+[) to return to the pause screen.
