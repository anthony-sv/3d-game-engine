# Visual scripting fixtures

Shared visual-script documents (`.sgraph`) for the C++ format, the managed VM and the runtime integration, and the node
library and views file of the visual script editor's tests. They are written in the writer's canonical form: keys in
canonical order, `DumpJson` layout (tabs, one member per line, a final newline), dyadic float geometry. Each document has
unique IDs, at least one comment and at least two unplaced nodes (no `Position`). Files are of version 2 of the format,
except the migration fixture; the content form has no version.

| File | Form | Covers |
|------|------|--------|
| `Example.sgraph` | file (header + content) | The format example: a public variable with tooltip and range, an impure function, a reflected node, an unplaced node, a comment. It is the canonical-form golden, and free of the node keys of version 2 (`Split`, `Comment`, `Collapsed`, `Enabled`). |
| `Version1/Example.sgraph` | file, version 1 | The bytes `Example.sgraph` had in version 1 of the format: the migration fixture. It loads without issues to the document of `Example.sgraph` and saves to exactly its bytes. |
| `Documents/Editor.json` | content | The node keys of version 2 where the editor shows them: each `Enabled` state on `Log.Info` calls, `Disabled` on a `Flow.Delay`, on `Event.Update` and on a `Function.Entry` (which ignores it, Warning `DisabledIgnored`), `DevelopmentOnly` on a delay and on a key event; `Split.Inputs` on the `Value` of `Set:Strada.TransformComponent.Translation` with field literals (`Value.X`, `Value.Z`) and a linked field (`Value.Y`); `Split.Outputs` `Hit` and `Hit.Point` on the raycast's `Hit`, whose fields `Hit.Point.Y` and `Hit.Distance` are linked; node comments and collapsed nodes. |
| `Documents/Events.json` | content | Every event node: the nine standard events, key, mouse-button and gamepad-button events (`Gamepad` literal 1), and the custom event `Hit` (a `float` and a `Strada.Entity` parameter) raised by `Event.Send` to Self. |
| `Documents/Flow.json` | content | Every flow node: Branch, Sequence (Count 3 and the default), Switch on `int`, `string` and `Strada.ForceMode` (Cases omitted), ForLoop and ForEach with `Break` linked, While, DoOnce, DoN, FlipFlop, Gate with all four inputs linked, Delay, RetriggerableDelay, WaitFrames, WaitUntil, Tween and Interval. |
| `Documents/Data.json` | content | Every data node: Self, a Literal of each kind, Reroute of exec and of data, Select, IsValid, Convert, Make and Break, FormatString, Array.Length and Array.Get, and all sixteen operators, including mixed numeric joins and the operand-literal rule. |
| `Documents/Functions.json` | content | A pure function, an impure recursive one with a local accumulator that raises an event, a latent one (a Delay, no Return), and an `int` to `float` implicit conversion into a pure call. |
| `Documents/Variables.json` | content | A public variable of each exposable type (values in the literal encodings, the enum by name, entity and assets `"0"`), tooltips, ranges and a former name, private Matrix4, RaycastHit and component variables, variables without `Value`, and implicit conversions into `Variable.Set`. The public `Behaviour` variable is a `Strada.VisualScript` (`Value` `"0"`), the asset class track C added. |
| `Documents/Reflected.json` | content | The reflected nodes of spec section 11.1 (math, logging, transform, rigid body, `GetComponent<T>`, raycast, input, time, entity, testing). Built-in events, branches and an IsValid drive and guard them. |
| `Documents/Reflected.sources.txt` | text | For each reflected node type, the C# declaration its type string and pins come from, and the enumerators the fixtures use. |
| `Editor/Library.json` | node library (`{ "Types": [...], "Nodes": [...] }`) | A verbatim subset of the scripting runtime's node library (see below), which the editor's tests read with `VisualScriptLibrary::Parse` where they need a library but no runtime. |
| `Editor/Views.json` | file (`VisualScriptViews`, version 1) | The visual script editor's per-user views file with every member: the panes and toggles, the scroll mode, recent nodes and types, the comment colour, the hints seen, and two projects with open graphs, views of the event graph and of a function, and bookmarks. Its numbers are exact as floats. |

## How the tracks use them

- **Track A** (StradaTests, `VisualScriptSerializerTests`): `Example.sgraph` loads with zero issues and
  `DumpJson(Serialize(...))` equals the file byte for byte. Each document loads in strict use (zero issues) and round-trips
  to an equal `VisualScriptDocument`.
- **Slice 2, track F** (StradaTests, `VisualScriptSerializerTests`): `Version1/Example.sgraph` migrates to `Example.sgraph`;
  `Documents/Editor.json` joins the documents above and keeps every node key of version 2 where the editor shows it.
  The VM reads `Enabled` and `Split` (slice 2, track V): `Editor.json` analyzes with zero Errors (one `DisabledIgnored`
  on node 30; the disabled nodes 17 and 19 pass the flow on at `Then` and `Completed`, the disabled event 22 starts
  nothing) and joins the VM's fixture tests and the native runs below. Where the spec leaves a VM detail open, the VM
  decides and the document follows.
- **Track B** (ScriptCoreTests): every document and the content of `Example.sgraph` analyze with zero Error
  diagnostics. The test project copies this directory to its output as `VisualScripting/`.
- **Track C** (StradaTests, `VisualScriptEngineTests`): the documents load and run through `VisualScriptEngine`.
  Branches guard every call that could fail, so they run without aborting errors whether or not the entity has children
  or a rigid body. `Editor.json` runs with them (slice 2), and, wrapped in a version-2 file header, it is the alternate
  seed of `VisualScriptFuzzTests`' damaged-graphs-run case.
- **Slice 2, the editor** (StradaEditorTests, which find this directory through `STRADA_TEST_DATA_DIR`): the document
  editing and canvas model tests parse `Editor/Library.json`, the canvas model's fixture semantics analyze with its
  templates, and its parity test (under the scripting runtime) compares each entry with the runtime's library and, on a
  mismatch, writes the expected file next to the test binary. `Editor/Views.json` seeds the views file's fuzz test.
  `VisualScriptEditorFoundationTests` checks that both keep the coverage and layout described here, and
  `VisualScriptNodeTitlesTests` that the library's members are titled as it titles them.

A fixture that contradicts the spec is corrected by the track that finds it, which records the change under "Spec
deviations".

## The editor's node library

`Editor/Library.json` holds the runtime's own entries, as `graph.types` and `graph.library` report them, for 41 node
kinds and 28 types: types in ID order and nodes in library order (by category, then title, then type), each entry
unchanged. They cover every kind of node a library has (`Unknown` is only the analysis') and every kind of type the
scripting API has (there is no type of kind `Class` in it): input events with an enum setting; flow with outputs and
wildcards that depend on settings; latent nodes, one with a tick (`Flow.Tween`); every node whose pins come from its
settings (variables, functions, custom events, `Flow.Sequence`, `Flow.Switch`, `FormatString`, `Literal`, `Convert`,
`Make`, `Break` and the generic calls below); the wildcards of `Reroute`, `Select`, `IsValid`, the operators,
`Flow.ForEach` and `Array.Get`; and the scripting API's calls, getters, a setter, a constructor, the generic
`GetComponent<T>` and `Load<T>` (whose `TypeArguments` setting types their pins), an out struct whose fields split again
(`Physics.Raycast`'s `Hit`), and inputs of an enum, an asset and a colour. Every type an entry names (pins, settings,
fields, elements, bases and implicit conversions) is one of its types.

- Nodes: `Call:Strada.Assets.Load<T>(string)`, `Get:Strada.ParticleSystemComponent.SpeedMax`,
  `Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)`,
  `Get:Strada.TransformComponent.Translation`, `Set:Strada.TransformComponent.Translation`, `Convert`,
  `Call:Strada.Log.Info(string)`, `Call:Strada.Entity.GetComponent<T>()`,
  `Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)`, `Event.Create`,
  `Event.Custom`, `Event.KeyPressed`, `Event.Send`, `Event.Update`, `Flow.Branch`, `Flow.Delay`, `Flow.ForEach`,
  `Flow.Sequence`, `Flow.Switch`, `Flow.Tween`, `Function.Call`, `Function.Entry`, `Function.Return`,
  `Call:Strada.Input.IsKeyDown(Strada.KeyCode)`, `Call:Strada.Color.Lerp(Strada.Color,Strada.Color,float)`,
  `Operator.Add`, `Operator.Less`, `New:Strada.Vector3(float,float,float)`,
  `Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)`, `Break`, `Make`,
  `Get:Strada.Time.DeltaTime`, `FormatString`, `IsValid`, `Literal`, `Reroute`, `Select`, `Self`, `Array.Get`,
  `Variable.Get`, `Variable.Set`.
- Types: `Strada.Asset`, `Strada.AssetHandle`, `Strada.Color`, `Strada.Component`, `Strada.Entity`, `Strada.Entity[]`,
  `Strada.ForceMode`, `Strada.KeyCode`, `Strada.Matrix4`, `Strada.ParticleSystemComponent`, `Strada.Prefab`,
  `Strada.Quaternion`, `Strada.Ray`, `Strada.RaycastHit`, `Strada.RigidBodyComponent`, `Strada.TransformComponent`,
  `Strada.Vector3`, `Strada.Vector4`, `System.Object`, `bool`, `double`, `exec`, `float`, `int`, `long`, `string`, `uint`,
  `ulong`.

When a change to the scripting API changes one of these entries, copy the file the parity test writes over this one, or
regenerate it from an editor without a project open (whose library has no game script classes):

```sh
strada launch --headless                                          # prints the editor's process ID
strada call graph.types '{"limit": 500}' --pid <id>
strada call graph.library '{"limit": 500}' --pid <id>
strada call graph.library '{"limit": 500, "offset": 500}' --pid <id>
strada call editor.quit --pid <id>
```

Keep the entries whose `ID` or `Type` the lists above name, in the order the commands return them, and write
`{ "Types": [...], "Nodes": [...] }` in `DumpJson`'s layout.
