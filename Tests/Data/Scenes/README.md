# Scene format fixtures

A scene (`.sscene`) of each version of the scene format, which `Tests/StradaTests/Source/Scene/SceneFormatTests.cpp`
loads and the scene fuzz case (`SceneFuzzTests.cpp`) seeds from (Docs/Architecture.md §7.3, "Versions and migrations of
scenes and prefabs"). They hold only the Tag, Transform, Relationship, Script and VisualScript components, whose fields
are stable, and are compared as documents, not as bytes: their layout is compact, not the writer's. Script fields and
visual script variables are written in name order, as the writer writes them.

| File | Version | Covers |
|------|---------|--------|
| `Version1.sscene` | 1 | Two entities, a holder and its child: script fields of single values (a float, an int, a string, an entity inside the scene and one outside it, a prefab) and visual script variables (an entity, a double). It loads unchanged and saves as version 2. |
| `Version2.sscene` | 2 | The same entities with a list of every element type among the script fields (`Bool[]` to `Asset[]`, an empty one, an `Entity[]` naming the scene's entities, none and one outside) beside a single value, and lists among the visual script variables (`Vector3[]`, `Entity[]`, an empty `Int64[]`). |

A new version of the format adds `Version<N>.sscene` and keeps the others.
