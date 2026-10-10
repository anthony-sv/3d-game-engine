---
name: strada-add-script-api
description: Checklist for adding or changing C# scripting API in Strada (native bindings in Strada/Source/Strada/Script/ScriptBindings*.cpp, InternalCalls.cs, the managed API in Strada-ScriptCore, value checks, XML docs, tests in StradaTests/TestScripts/ScriptCoreTests, the feature-test project and docs). Use whenever scripts gain, lose or change an engine function, component class, property or type.
---

# Adding or changing scripting API

Scripts call the engine through native functions bound by name: the engine's binding table
(`Strada/Source/Strada/Script/ScriptBindings*.cpp`) and the `delegate* unmanaged` fields of
`Strada-ScriptCore/Source/Interop/InternalCalls.cs` must match exactly. A missing, extra or duplicated name fails the
scripting startup, so every scripting test fails with the list of names.

## 1. Native binding

- Pick the file by area: `ScriptBindingsEntity.cpp` (entities, logging), `ScriptBindingsComponents.cpp` (component
  classes), `ScriptBindingsRuntime.cpp` (Time, Input, Physics), `ScriptBindingsAssets.cpp` (Assets, Material),
  `ScriptBindingsApplication.cpp` (Application, SceneManager, Debug, Strada.Testing). Functions live in an anonymous
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

## 3. Tests

- Native behavior: a probe script in `Tests/TestScripts/Source/` exercises the API and records results (often in its
  entity's name). A doctest in `Tests/StradaTests/Source/Scene/` (`SceneScripting*Tests.cpp`) runs it under
  `Testing::ScriptEngineScope` and checks the engine state and the logged errors. New test script classes change the
  class list checked in `ScriptEngineTests.cpp`.
- Pure managed logic (math, encodings, layouts): xUnit tests in `Tests/ScriptCoreTests`.
- Feature-test project: call every new public member from the `Projects/FeatureTest` scripts, in the script of its area
  (`Scripts/Source/*Tests.cs`, checks through `Check(...)`), with assertions about what it does. The API coverage test in
  `ScriptCoreTests` lists unused members; `StradaRuntimeTests` runs the project and fails on any failed check. Values the
  scripts read from the scene (entity, asset and other fields) are set by opening the project in the editor.

## 4. Docs

Update `Docs/Architecture.md` §10.4 (API list and semantics), and §10.3 for new field types or lifecycle rules.
