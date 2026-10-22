---
name: strada-add-script-api
description: Checklist for adding or changing C# scripting API in Strada (native bindings in Strada/Source/Strada/Script/ScriptBindings*.cpp, InternalCalls.cs, the managed API in Strada-ScriptCore, value checks, XML docs, the visual-script nodes the API becomes (purity, [PureNode], redirects), tests in StradaTests/TestScripts/ScriptCoreTests, the feature-test project and docs). Use whenever scripts gain, lose or change an engine function, component class, property or type.
---

# Adding or changing scripting API

Scripts call the engine through native functions bound by name: the engine's binding table
(`Strada/Source/Strada/Script/ScriptBindings*.cpp`) and the `delegate* unmanaged` fields of
`Strada-ScriptCore/Source/Interop/InternalCalls.cs` must match exactly. A missing, extra or duplicated name fails the
scripting startup, so every scripting test fails with the list of names.

## 1. Native binding

- Pick the file by area: `ScriptBindingsEntity.cpp` (entities, logging), `ScriptBindingsComponents.cpp` (component
  classes), `ScriptBindingsUI.cpp` (the UI components, `Input.IsPointerOverUI`), `ScriptBindingsRuntime.cpp` (Time,
  Input, Physics), `ScriptBindingsAssets.cpp` (Assets, Material), `ScriptBindingsApplication.cpp` (Application,
  SceneManager, Debug, Strada.Testing). Functions live in an anonymous
  namespace and are added in the file's `Register*Bindings` function. A new file gets its own `Register*Bindings`
  (declared in `ScriptGlue.h`, called from `ScriptBindings.cpp`) and is listed in `Strada/CMakeLists.txt`.
- Name: `<CSharpClass>_<Member>` (`RigidBodyComponent_AddForce`, `Physics_Raycast`).
- Signature: blittable types only. `uint8_t` for bool, `int32_t` for enums and counts, `uint64_t` for entity IDs and
  asset handles, `ScriptGlue::Vector2/3/4`, `Quaternion` and `Matrix4` by pointer. Strings go in as `char const*` plus
  an `int32_t` length (UTF-8) and come out as a returned `char const*` with an `int32_t* length` out parameter,
  pointing at engine-owned storage the caller copies at once.
- Structures crossing the boundary have the same layout in C++ and C#: no implicit padding, a `static_assert` of the
  size and key offsets, and a check in `Tests/ScriptCoreTests` (`InteropTests.NativeStructuresMatchTheEngineLayouts`).
- Component fields: `AddFieldBindings<&FooComponent::Speed, "FooComponent", "Speed">(table)` generates
  `FooComponent_GetSpeed` and `FooComponent_SetSpeed`; the setter checks the value (`FieldValue<T>::IsValid`) and
  writes through the registry, so systems watching the component see the change.
- Scripts are user code and must never crash the engine. Use `GetScene`, `FindEntity`, `FindComponent<T>` and
  `PatchComponent<T>`: they log a script error naming the function and do nothing when the scene, entity or component
  is missing. Check numbers with `CheckFinite` and enums against their `EnumTraits`, normalize rotations with
  `ToRotation`, and log through `Log::GetScriptLogger()` as `"<CSharpClass>.<Member>: <problem>"`.
- Application concerns (scene changes, quitting, test results) go through the `ScriptHost` of
  `ScriptEngine::GetHost()`, never directly to the editor or the runtime player.

## 2. Managed side (`Strada-ScriptCore`)

- Declare the field in `InternalCalls.cs` with the same name and a matching `delegate* unmanaged<...>` signature.
- Public API lives in the `Strada` namespace (`Strada.Testing` for test helpers), a type per file except small
  related types. Use `unsafe` only where pointers are needed. Every public member has XML documentation (missing docs
  are build errors).
- Components: a `sealed unsafe class FooComponent : Component` with `[NativeComponent("Foo")]` (the registry name), a
  private constructor and properties through the `NativeField` helpers.
- Programming mistakes in arguments (null) throw `ArgumentNullException`. Engine-state problems are logged by the
  native side, and the call returns a neutral value (null, false, zero).
- Format with `python Tools/format.py` (it runs `dotnet format` on the C# projects).

## 3. New callbacks

A callback scripts override (like `OnClick`) is an event the engine sends, not a binding:

- Give `ScriptEvent` in `Strada/Source/Strada/Script/ScriptEngineInternal.h` and in
  `Strada-ScriptCore/Source/Interop/ScriptRegistry.cs` the same enumerator with the same value, and handle it in
  `ScriptRegistry.Invoke`.
- Add `ScriptEngine::InvokeOn<Name>` (declared in `ScriptEngine.h`, calling `Invoke` with the event) and call it where
  the scene raises the event (`Scene`'s runtime, in the documented order of the frame).
- In `Script.cs`, add the `protected virtual void On<Name>(...)` with XML documentation and the `internal void
  InvokeOn<Name>(...)` wrapper `ScriptRegistry` calls.
- Test it with a probe in `Tests/TestScripts` and override it in a `Projects/FeatureTest` script: the API coverage test
  counts overrides as uses of a callback.
- Visual scripts get the event too: a `BuiltinNode` event kind (in `Library/NodeDefinition.cs` of
  `Strada-ScriptCore/Source/VisualScripting`, its node in `Library/BuiltinNodes.cs`), its place in `ProgramCompiler`'s
  table of `ScriptEvent`s and in the range `VisualScriptRuntime.Invoke` accepts, and
  `VisualScriptEngine::InvokeOn<Name>` (`VisualScriptEngine.h`), which the scene calls right after the C# callback. Add
  the node to `Tests/Data/VisualScripting/Documents/Events.json`, and a check of it to the FeatureTest graphs, which use
  every built-in kind: edit `build_features` (or the graph builder that fits) in `Projects/FeatureTest/Tools/AuthorGraphs.py`
  and run it (`python Projects/FeatureTest/Tools/AuthorGraphs.py build/<preset>/bin/strada.exe`); it writes the graphs
  through `graph.check` and `graph.set` and runs the tests, and stops until `FeaturesChecks` in
  `Projects/FeatureTest/Scripts/Source/VisualScriptTests.cs` equals the number of Features checks.
- Add its row to the callback table of `Docs/Scripting.md` and to the list in `Docs/Architecture.md` §10.3, and the
  event node to the table in the guide's "Visual scripts" section.

## 4. Tests

- Native behavior: a probe script in `Tests/TestScripts/Source/` exercises the API and records results (often in its
  entity's name). A doctest in `Tests/StradaTests/Source/Scene/` (`SceneScripting*Tests.cpp`) runs it under
  `Testing::ScriptEngineScope` and checks the engine state and the logged errors. New test script classes change the
  class list checked in `ScriptEngineTests.cpp`.
- Pure managed logic (math, encodings, layouts): xUnit tests in `Tests/ScriptCoreTests`.
- Feature-test project: call every new public member from the `Projects/FeatureTest` scripts, in the script of its area
  (`Scripts/Source/*Tests.cs`, checks through `Check(...)`), with assertions about what it does. The API coverage test in
  `ScriptCoreTests` lists unused members; `StradaRuntimeTests` runs the project and fails on any failed check. Values the
  scripts read from the scene (entity, asset and other fields) are set by opening the project in the editor.

## 5. Visual scripts

Every public member of the API is also a node of visual scripts: the VM's library is built by reflection over the
public types of `Strada.ScriptCore` in the namespaces `Strada` and `Strada.Testing`
(`Strada-ScriptCore/Source/VisualScripting/Library/MemberNodes.cs`). Check what a change does to it.

- Which members become nodes: methods (`Call:<Type>.<Method>(<parameter type IDs>)`), property and field getters
  (`Get:`), setters (`Set:`, only for static members and members of classes: struct fields change through Make and
  Break; never for init-only, readonly or constant ones) and the public constructors of non-abstract types
  (`New:<Type>(...)`). Left out: members marked `[Obsolete]` or `[EditorBrowsable(EditorBrowsableState.Never)]`;
  `Equals`, `GetHashCode`, `ToString`, operators (they feed the operator and `Convert` nodes) and indexers; the members
  of enums, interfaces, delegates, attributes, exceptions, generic types and `Component` itself; methods with `ref`,
  `in` or `params` parameters (`out` parameters become outputs) and members whose types are no library types (pointers,
  delegates, generic types other than arrays, `Nullable<T>`); generic methods unless every type parameter is
  constrained to exactly `Component`, `Asset` or `Script` (they get a `TypeArguments` setting); the methods of
  `VisualScriptComponent` (graphs use `Event.Send` and the variable nodes; its properties are nodes).
- Pins: `Execute` and `Then` on impure nodes; `Target` on instance members (an `Entity` that defaults to the graph's
  own entity for `Entity` and component members); a pin per parameter, its name's first letter upper-cased, with the
  suffix `Parameter` when it would clash with a fixed pin; `Result` for a return value, `Value` for setters, getters and
  constructors. An optional parameter's default is the pin's default (an enum by its name; a struct declared
  `= default` gives `default(T)`, except a `Quaternion`, which keeps the identity). Titles come from the names,
  categories from the declaring types and descriptions from the XML documentation's summaries.
- Purity: getters and struct constructors are pure; so is every method of `Mathf`, `Random`, the vectors, `Quaternion`,
  `Matrix4` and `Color`, and a method marked `[PureNode]` (the internal attribute in
  `Strada-ScriptCore/Source/VisualScripting/PureNodeAttribute.cs`; not `System.Diagnostics.Contracts.Pure`, which never
  reaches the assembly). A method returning `void` without `out` parameters is never pure, whatever it is marked with,
  and setters never are; everything else is impure. Pure nodes have no flow pins and run for every node that reads them,
  so only side-effect-free queries may be pure. A query added to the API (`Input.IsKeyDown`, `Entity.FindByName`, ...)
  gets `[PureNode]` in the same change, and `NodeLibraryTests.ExactlyTheListedQueriesAreMarkedPure` (ScriptCoreTests)
  lists the marked methods.
- Renaming or re-signing a member that graphs use (its name, declaring type or parameter types) changes its node's type
  string: add a `NodeRedirect(OldType, NewType, PinRenames)` entry to `NodeRedirects.Current`
  (`Strada-ScriptCore/Source/VisualScripting/Library/NodeRedirect.cs`), with a `PinRename(Direction, From, To)` for each
  pin whose name changed with it, so stored graphs keep resolving (their analysis reports `NodeUpdated`, and
  `graph.set` stores the new type). A redirect applies only to type strings the library no longer has, so renaming a
  parameter alone breaks the links of graphs that use its pin: keep the parameter names of members graphs may use.
  StradaTests fails when a graph that a scene of `Projects/` uses no longer analyzes cleanly with every node resolved
  exactly or by a redirect ("VisualScriptEngine: the projects' scene graphs load strictly, analyze cleanly and resolve
  exactly or by redirect"), and ScriptCoreTests analyze the shared documents of `Tests/Data/VisualScripting` (their
  reflected nodes and where they come from are in `Documents/Reflected.sources.txt`).

## 6. Docs

Update `Docs/Architecture.md` §10.4 (API list and semantics), and §10.3 for new field types or lifecycle rules.
