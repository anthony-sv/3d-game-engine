# Engine content fixture

A tiny engine content tree for the editor's tests, in the layout an engine has when it is not a build tree
(`Strada/Source/Strada/Core/EngineLayout.h`): `Templates/` (project templates), `Examples/` (example projects and their
`catalog.json`), `Docs/` (the guides) and `Agents/` (the agent kit and the game-making skill). It holds no `engine.json`,
so a layout resolved from an executable path in this folder reads its content from here.

`Testing::MakeEditorLocations` (`Tests/StradaEditorTests/Source/EditorLocationsTestUtilities.h`) gives editors under test
this content, so that no test reads the engine's own templates, examples or guides. Keep every file small: each one
exists for a test that names it.
