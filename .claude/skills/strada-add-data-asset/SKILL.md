---
name: strada-add-data-asset
description: Checklist for adding or changing a data asset type in Strada (a file of one reflected struct such as a curve, a physics material, an input action map, a mixer or a vehicle definition): the struct and its field table, the one registration in DataAsset.cpp, migrations with fixtures, the asset editor's drawer, the generated commands and their docs, the fuzz seeds, the MCP tool list, and the typed managed class with its loading. Use whenever a data asset type is created, renamed, or gains, loses or changes fields.
---

# Adding or changing a data asset type

A data asset type comes from **one registration** (`Strada/Source/Strada/Asset/DataAsset.h`, Docs/Architecture.md
§6.4). From it come the versioned file with its header, the loader and saver, the `AssetType` and extension mapping, hot
reload through versions, the inspector's editor, the content browser's New item, the commands `<domain>.create`, `get`
and `set`, and a place in the registry-wide fuzz test. Never write per-type code in the asset manager, the content
browser, the inspector, the automation commands or the fuzzers: improve the field table or the registry instead.
Materials keep their own code (`MaterialAsset`, `material.*`); this does not apply to them.

## 1. The struct and its field table

- The struct lives in the Asset module or below it (`Core`, `Math`): every host must know the type, and the renderer,
  physics and audio include it from there, as the renderer includes `MaterialData`. An enum the struct shares moves down
  with it. A higher module's own settings (the renderer's quality settings) are no data asset of that module's struct:
  make a project setting or a struct of its own that the module maps.
- A struct of the Asset module carries its field table (`StructTraits<T>`) in its own header; a struct of Core or Math
  gets it in `Asset/<Name>Asset.h`, with the specification, and every user of the asset includes that header.
- Every field has `.Doc(...)`: it is the inspector's tooltip, the commands' schema and Docs. Give numbers ranges
  (`.Range`, `.AtLeast`), arrays a `.MaxCount(n)`, asset references `.References("<type name>")` (built-in names such as
  `"Texture"`, or a data asset type's name), and switches and variants `.EnabledBy` and `.VisibleWhen` (up to 16 values).
- Only values with a generated editor and schema: no `FieldKind::Custom` field and no array of arrays, in nested structs
  and array elements too (`TypedDataAssetType` refuses them at compile time). An optional value is a bool switch and the
  value; a variant is a `Kind` enum and the fields of each kind; a map or a graph is an array of named structs with a
  `MaxCount` whose `Validate` checks unique names and acyclic parents. Anything else extends the field table first (a
  kind, its editor, its schema), never a custom field.
- `Validate` (optional, pure) states what the table cannot: unique names, keys in order, counts that depend on each
  other. It refuses and never repairs: what loads is exactly what was written.
- Default member initializers make the value-initialized struct a valid asset: registration checks it.

## 2. The registration

One line in `DataAssetRegistry::AddEngineTypes()` (`DataAsset.cpp`), sorted by name (it merges as a union):

```cpp
AddEngineType<Curve>(CurveAssetSpecification);
```

with the specification beside the field table:

```cpp
inline constexpr std::array<DocumentMigration, 0> CurveMigrations = {};
inline DataAssetTypeSpecification<Curve> const CurveAssetSpecification = {
	.Name = "Curve",                    // PascalCase; the AssetType name, the file's Type, the managed class
	.Extension = ".scurve",             // ".s" + 2 to 16 lowercase letters or digits
	.FormatVersion = 1,
	.Description = "A scalar curve of keys with their interpolation, and its behaviour before and after them.",
	.Migrations = CurveMigrations,      // FormatVersion - 1 functions
	.Validate = &ValidateCurve,
};
```

- The name gives the command domain in kebab case and the parameter in lower camel case: `Curve` gives `curve.*` and
  `curve`, `PhysicsMaterial` gives `physics-material.*` and `physicsMaterial`, `UIStyle` gives `ui-style.*` and
  `uiStyle`. A name whose domain is a built-in domain's (`Graph`, `Asset`) makes `RegisterEditorCommands` fail: the
  editor tests catch it. Choose the name for good; files, asset registries and scripts carry it.
- Registration fails loudly at startup (an assert) for a broken specification; the registry's tests list every rule.

## 3. Formats and migrations

- Adding a field with a default is compatible: no new version. Renaming, removing or changing the meaning of a field
  bumps `FormatVersion` and adds a migration (`Result<void>(Json& content)`, on the content object in place) that checks
  what it reads and refuses what it cannot convert, with a fixture of the old version under
  `Tests/Data/DataAssets/<Name>/Version<N>.<extension>` and a StradaTests case that loads it and saves the current
  version (`Tests/StradaTests/Source/Asset/DataAssetTests.cpp` shows the test type's).
- A struct that other types embed (a curve inside a vehicle definition) changes only compatibly; otherwise its header
  exports a migration of one object of it (`MigrateCurveFrom1(Json&)`), and every type that embeds it bumps its own
  version with a migration that calls it on each embedded object, with a fixture of its own.
- Files of the type that are not assets (a save slot, the player's settings) use `ReadStructDocument` and
  `WriteStructDocument` (`Serialization/StructDocument.h`), not the registry.

## 4. Readers and hot reload

- C++ users keep the handle, look the asset up when they use it
  (`AssetManager::GetAsset<TypedDataAsset<T>>(handle)`, null when it is missing or of another type, which they treat as
  none) and compare `DataAsset::GetVersion()` with the version of what they derived (GPU resources, tables, cached
  values). Versions never repeat, so the version alone tells an edit (same object, new version) and a reload (another
  object) apart. Loading an asset never loads what it references: resolve references when you use them.
- Editor edits (the inspector, `<domain>.set`, undo and redo) keep the object and move its version; a change made
  outside the editor is found by the next refresh, which unloads the asset.

## 5. The editor

- Nothing to write for the default editor: the inspector draws the field table through the FieldEditor, one undo step
  per interaction (`SetDataAssetCommand`), disabled for data assets in memory and read-only files.
- A type whose field table is not the editor it needs (a curve's keys) gets a drawer: one line in the
  `DataAssetEditor` constructor (`StradaEditor/Source/Editor/UI/DataAssetEditor.cpp`), sorted by name,
  `SetDrawer("<Name>", &Draw<Name>)`. The drawer gets a `DataAssetDrawContext` (the asset, its type, its fields as JSON,
  whether it is editable, the field editor) and returns a `DataAssetEdit` when the user changed something: a partial
  patch of whole field values, the step's name (empty: `Edit <Name>`) and a merge key (0: the inspector's interaction,
  which merges while one ImGui item stays active; a key of its own for edits that span frames without an active item,
  such as arrow-key nudges merged by time). It never writes files or assets itself, shows nothing editable when
  `Editable` is false, and may draw the fields it leaves alone with `DataAssetEditor::DrawFields(context)`, marking the
  ones it draws `.HiddenInEditors()`.
- Test the drawer headless (`HeadlessImGui`, the inspector panel): one step per interaction, its own key merging across
  frames, its description in the history.

## 6. Automation

- The commands exist as soon as the type is registered. Add the type's three rows to the data assets section of
  `Docs/Automation.md` (`EditorCommands: every command is documented in Docs/Automation.md` fails otherwise), naming its
  fields' meaning where the schema's descriptions are not enough.
- Pin the type's MCP tools in `Tests/StradaToolTests/Source/EditorIntegrationTests.cs`: add `<domain>_create`,
  `<domain>_get` and `<domain>_set` (`curve_create`, ...) to the `Assert.Contains` list of `tools/list`.
- `AutomationFuzzTests` fuzzes every registered type's commands with seeds, parameters and checks drawn from the registry:
  nothing to add. A new command shifts the fuzz's random sequence; when a chance-based check of another command then
  fails, steady that command's generator, never the seeds.
- Commands a type needs beyond create, get and set get names of their own in the type's domain (`curve.evaluate`), with
  the strada-automation-command skill.

## 7. Tests and seeds

- A FeatureTest seed file of the extension under `Projects/FeatureTest/Assets/` that sets every field away from its
  default: `Tests/StradaTests/Source/Asset/DataAssetFuzzTests.cpp` requires one per engine type and damages it.
- StradaTests: the struct's own behaviour (evaluation, validation, migrations) beside its module's tests.
- StradaEditorTests: a drawer's cases, when the type has one.

## 8. Scripts: the typed managed class

A type that scripts use gets a sealed class of the same name in `Strada-ScriptCore` deriving from `Asset` with an
internal constructor, as `Material` does. No public member lands without a use in the FeatureTest scripts
(`ScriptCoreTests` refuses an unused one), so the class arrives with its first user.

- **Names, not numbers.** Managed code identifies asset classes by type name: `AssetTypes` maps the class to `"<Name>"`
  without calling native code, and the visual script library, the script field codec and the literal codec carry the
  name. A native `AssetType` value is fetched only where a call crosses into native code (`Assets.Load<T>`, the
  loadable-type check, the type's bulk read): `Assets_FindType(name)`, cached per class after its first call; a value of
  128 or more coming from native code is named by `Assets_GetTypeName` before `AssetTypes.Create` makes the reference.
  The script bindings' loadable-type check (`IsLoadableType`, `ScriptBindingsAssets.cpp`) accepts the type when its
  class arrives. The first type (`Curve`) brings these two internal calls and this table; later types add a row.
- **The data stays native.** A class that reads data per call in managed code (a curve evaluated per sample, which must
  allocate nothing and cross no boundary) keeps a managed copy with the version it read, refreshed through one binding
  of its own that takes the held version and copies the data in bulk only when the asset's version differs.
- **Invalidation.** Before running scripts (the update, fixed update and event entries of C# scripts and graphs), the
  Script module compares `DataAssetRegistry::GetEpoch()` with the epoch it last gave the managed side and, when it moved,
  calls the managed entry point `DataAssets.Invalidate` once (added with the first user); a class then asks again on its
  next use. An access otherwise costs a comparison of two managed numbers, and a script sees an inspector edit made
  while playing on its next frame.
- Follow the strada-add-script-api skill for every member: XML docs, `[PureNode]` for reads (a curve's `Evaluate`), the
  member-type recipe in `VisualScriptNodeCoverageTests.cpp`, the FeatureTest script and graph that use it,
  `AuthorGraphs.py` run again, `FeaturesChecks`, and `Docs/ScriptingAPI.md` regenerated (`STRADA_UPDATE_DOCS=1`).

## 9. Docs

- Docs/Architecture.md §6.4 keeps the pattern; a type's own semantics (a curve's interpolation) go in the section of its
  subsystem, and its file in the formats table of §7.3.
- The guides that teach the feature (Docs/Editor.md, Docs/Scripting.md, Docs/VisualScripting.md) mention the type where
  users meet it.
