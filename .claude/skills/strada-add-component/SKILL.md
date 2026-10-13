---
name: strada-add-component
description: Checklist for adding or changing an ECS component in Strada (Components.h, ComponentTraits field table, registry list, entity-reference remapping, runtime systems, editor inspector, scripting API, tests, feature-test scene, docs). Use whenever a component is created, renamed, or gains/loses fields.
---

# Adding or changing a component

Components are plain structs; everything else (serialization, copies, automation, schemas, the registry) is derived
from one field table. Follow every step — the tests enforce several of them.

## 1. Define the data — `Strada/Source/Strada/Scene/Components.h`

- `struct <Name>Component` with PascalCase public fields and sensible defaults; document units (degrees, meters,
  seconds) and semantics in comments.
- Field types must have `JsonTraits`: `bool`, integers, `float`, `double`, `std::string`, `UUID`, `AssetHandle`,
  `glm::vec2/3/4`, `glm::quat`, `glm::bvec3`, `std::vector<T>`, registered enums, `ScriptFieldMap`. Add a
  `JsonTraits` specialization (with tests) for anything new.
- Asset references are `AssetHandle` (default = no asset), entity references are `UUID`.
- New enums: `enum class X : uint8_t` plus an `EnumTraits<X>` name table in `ComponentTraits.h`.

## 2. Register it — `Strada/Source/Strada/Scene/ComponentTraits.h`

```cpp
template<>
struct ComponentTraits<FooComponent>
{
	static constexpr std::string_view Name = "Foo";             // serialized name, never change once shipped
	static constexpr uint32_t Flags = ComponentFlagsNone;       // Core / Internal flags if applicable
	static constexpr std::string_view Category = ComponentCategory::Physics;  // group of the Add Component menu
	static constexpr std::string_view Description = "One sentence for users and agents.";
	static constexpr auto Fields = std::make_tuple(              // EVERY field, with hints
		Field("Speed", &FooComponent::Speed).AtLeast(0.0).Doc("Meters per second."),
		Field("Tint", &FooComponent::Tint).Range(0.0, 1.0).AsColor(),
		Field("Mesh", &FooComponent::Mesh).References("Mesh"));
};
```

Hints: `Range(min, max)` / `AtLeast(min)` (inclusive, enforced when JSON is read, so files and automation reject
out-of-range values; the inspector clamps to them), `AsColor()` (vec3/vec4), `AsAngle()` (degrees), `AsMultilineText()`,
`References("<AssetType>")` for asset fields, `VisibleWhen("<Field>", "<Value>", ...)` for fields that only matter while
an enum or bool field of the same struct has one of up to four values (editors hide them otherwise: a camera's
orthographic size, a joint's limits), `EnabledBy("<bool field>")` for a field another bool field of the table switches
on (the inspector draws the switch as a check box in front of the field, like a post-process volume's overrides),
`HiddenInEditors()` for a field the editor edits with UI of its own (a Mesh's material overrides, which the material
slot sections edit) and `Doc("...")` for anything not obvious from the name. Hints that do not fit the field's type,
visibility conditions that name no value of such a field, and switches that are not unswitched bool fields of the table
fail to compile. Defaults must lie inside their ranges (tested). Systems reading values scripts set (which skip the
range checks) can use `ClampToFieldRanges`.

`Category` is one of `ComponentCategory::All` (`Core`, `Rendering`, `Physics`, `Audio`, `Scripting`, `UI`): the
inspector's Add Component menu groups components by it and `component.types` lists it. The category test in
`ComponentRegistryTests.cpp` lists every component with its category, so add the new one there.

Append the type to `AllComponents` (order = serialization order). Optional: `ReadExtraField` for alternative input
keys (see Transform's `RotationEuler`).

## 3. Entity references

If the component stores UUIDs of other entities, extend `Scene::RemapEntityReferences` so duplication and prefab
instantiation remap them.

## 4. Runtime behavior

Hook the component into its system (renderer submission, physics body creation, audio sources, script lifecycle) and
handle the component being added/removed while the scene is running.

## 5. Editor and automation

- The inspector, `component.*` automation commands and `component.types` schemas are generated from the field table;
  good hints are what make them usable. Verify the component in the editor's inspector and `component.types` output.
- Components users create as entities of their own (lights, a character, UI elements) get a preset in
  `StradaEditor/Source/Editor/EntityPresets.cpp`, in the create menus' category where users look for it (usually the
  component's), with the values that make it useful at once; `EditorUITests.cpp` lists every menu entry.

## 6. Scripting

Expose the component to C# following `strada-add-script-api` (managed class, bindings, XML docs).

## 7. Tests (all must pass)

- `Tests/StradaTests/Source/Scene/ComponentRegistryTests.cpp` round-trips every registered component automatically;
  update the expected component count and add focused tests for non-trivial fields or behavior.
- System tests for the runtime behavior.
- Feature-test project: add an entity using the component to `Projects/FeatureTest/Assets/Scenes/Main.sscene` (open
  the project in the editor; enforced by `StradaTests`), and test its scripting API in `ComponentTests.cs` or the
  script of its area.

## 8. Changing an existing component

Renaming or removing a serialized name/field breaks saved scenes: bump `SceneSerializer::FormatVersion` and add a
migration in the serializer. Adding fields is backward compatible (missing fields keep defaults). Narrowing a field's
range rejects files that use values outside it, so it is a format change too (migrate by clamping).

## 9. Docs

Update the component table in `Docs/Architecture.md` §7.2.
