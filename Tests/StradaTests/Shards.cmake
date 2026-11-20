# The shards of StradaTests: one ctest entry each, so that no entry runs into ctest's timeout as the suite grows.
#
# cmake/StradaTestShards.cmake says how a shard selects its test cases by source file and how patterns match. A shard
# takes whole module folders of Source/, so that a new test file lands with its module: in the shard that names its folder,
# or in Core, which takes every folder the others do not name. StradaTests.Partition checks that the shards are a
# partition of the suite: every test case in exactly one shard, no shard empty. A shard that passes 450 s on CI's Windows
# MSVC Debug job that runs this suite (engine tests) is split by source-file patterns: add the new shard here and take
# its files out of the shard that held them (for Core: add its patterns to Core's exclusions).

# The asset formats and their importers, and the fuzz tests of the visual script, data asset, mesh and font formats.
set(STRADA_ENGINE_TEST_ASSET_FILES "*StradaTests?Source?Asset?*")
# The scripting runtime (C# and visual scripts, the node library's coverage) and scenes: their runtime, scripting,
# physics state, serialization and fuzz tests.
set(STRADA_ENGINE_TEST_SCRIPTING_FILES "*StradaTests?Source?Script?*,*StradaTests?Source?Scene?*")
# The GPU tests: the device, swapchains and readback, the renderer's passes and golden images, and the ImGui layer.
set(STRADA_ENGINE_TEST_RENDERING_FILES "*StradaTests?Source?RHI?*,*StradaTests?Source?Renderer?*,*StradaTests?Source?ImGui?*")

set(STRADA_TEST_SHARDS Asset Scripting Rendering Core)

set(STRADA_TEST_SHARD_Asset "--source-file=${STRADA_ENGINE_TEST_ASSET_FILES}")
set(STRADA_TEST_SHARD_Scripting "--source-file=${STRADA_ENGINE_TEST_SCRIPTING_FILES}")
set(STRADA_TEST_SHARD_Rendering "--source-file=${STRADA_ENGINE_TEST_RENDERING_FILES}")
# Everything the shards above do not select: Core, Project, Serialization, Audio, Physics, Platform, Math and any new folder.
set(STRADA_TEST_SHARD_Core
    "--source-file-exclude=${STRADA_ENGINE_TEST_ASSET_FILES},${STRADA_ENGINE_TEST_SCRIPTING_FILES},${STRADA_ENGINE_TEST_RENDERING_FILES}")
