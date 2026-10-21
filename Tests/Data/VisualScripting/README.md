# Visual scripting fixtures

Shared visual-script documents (`.sgraph`) for the C++ format, the managed VM and the runtime integration. They are
written in the writer's canonical form: keys in canonical order, `DumpJson` layout (tabs, one member per line, a final
newline), dyadic float geometry. Each document has unique IDs, at least one comment and at least two unplaced nodes (no
`Position`).

| File | Form | Covers |
|------|------|--------|
| `Example.sgraph` | file (header + content) | The format example: a public variable with tooltip and range, an impure function, a reflected node, an unplaced node, a comment. It is the canonical-form golden. |
| `Documents/Events.json` | content | Every event node: the nine standard events, key, mouse-button and gamepad-button events (`Gamepad` literal 1), and the custom event `Hit` (a `float` and a `Strada.Entity` parameter) raised by `Event.Send` to Self. |
| `Documents/Flow.json` | content | Every flow node: Branch, Sequence (Count 3 and the default), Switch on `int`, `string` and `Strada.ForceMode` (Cases omitted), ForLoop and ForEach with `Break` linked, While, DoOnce, DoN, FlipFlop, Gate with all four inputs linked, Delay, RetriggerableDelay, WaitFrames, WaitUntil, Tween and Interval. |
| `Documents/Data.json` | content | Every data node: Self, a Literal of each kind, Reroute of exec and of data, Select, IsValid, Convert, Make and Break, FormatString, Array.Length and Array.Get, and all sixteen operators, including mixed numeric joins and the operand-literal rule. |
| `Documents/Functions.json` | content | A pure function, an impure recursive one with a local accumulator that raises an event, a latent one (a Delay, no Return), and an `int` to `float` implicit conversion into a pure call. |
| `Documents/Variables.json` | content | A public variable of each exposable type (values in the literal encodings, the enum by name, entity and assets `"0"`), tooltips, ranges and a former name, private Matrix4, RaycastHit and component variables, variables without `Value`, and implicit conversions into `Variable.Set`. There is no `Strada.VisualScript` variable yet: track C adds one with the class. |
| `Documents/Reflected.json` | content | The reflected nodes of spec section 11.1 (math, logging, transform, rigid body, `GetComponent<T>`, raycast, input, time, entity, testing). Built-in events, branches and an IsValid drive and guard them. |
| `Documents/Reflected.sources.txt` | text | For each reflected node type, the C# declaration its type string and pins come from, and the enumerators the fixtures use. |

## How the tracks use them

- **Track A** (StradaTests, `VisualScriptSerializerTests`): `Example.sgraph` loads with zero issues and
  `DumpJson(Serialize(...))` equals the file byte for byte. Each document loads in strict use (zero issues) and round-trips
  to an equal `VisualScriptDocument`.
- **Track B** (ScriptCoreTests): every document and the content of `Example.sgraph` analyze with zero Error
  diagnostics. The test project copies this directory to its output as `VisualScripting/`.
- **Track C** (StradaTests, `VisualScriptEngineTests`): the documents load and run through `VisualScriptEngine`.
  Branches guard every call that could fail, so they run without aborting errors whether or not the entity has children
  or a rigid body.

A fixture that contradicts the spec is corrected by the track that finds it, which records the change under "Spec
deviations".
