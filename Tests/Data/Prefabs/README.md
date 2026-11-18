# Prefab format fixtures

A prefab (`.sprefab`) of each version of the prefab format, which `Tests/StradaTests/Source/Scene/SceneFormatTests.cpp`
loads and instantiates and the prefab fuzz case (`SceneFuzzTests.cpp`) seeds from (Docs/Architecture.md §7.3,
"Versions and migrations of scenes and prefabs"). They hold only the Tag, Transform, Relationship, Script and
VisualScript components and are compared as documents, not as bytes. Script fields and visual script variables are
written in name order, as the writer writes them.

| File | Version | Covers |
|------|---------|--------|
| `Version1.sprefab` | 1 | A turret and its barrel: an entity field that names the barrel (remapped on instantiation) and one that names an entity outside the prefab (kept), a float; the barrel's visual script variables name the turret and hold a count. |
| `Version2.sprefab` | 2 | The same entities with a list of every element type among the turret's script fields (an empty `Vector4[]` among them), an `Entity[]` naming both of the prefab's entities, one outside it and none, and lists among the barrel's variables (`Entity[]`, `Float[]`). |

A new version of the format adds `Version<N>.sprefab` and keeps the others.
