# The shards of StradaRuntimeTests: one ctest entry each, so that no entry runs into ctest's timeout as the suite grows.
#
# cmake/StradaTestShards.cmake says how a shard selects its test cases by source file and how patterns match.
# StradaRuntimeTests.Partition checks that the shards are a partition of the suite: every test case in exactly one shard,
# no shard empty. A shard that passes 450 s on CI's Windows MSVC Debug job is split by source-file patterns: add the new
# shard here and take its files out of the shard that held them (for Core: add its patterns to Core's exclusions).

# The repository's sample projects, each played several times through the player, and what a game keeps for its player
# between runs (SaveGameTests.cpp: a few short runs of generated games, kept out of Core, which holds the player's oldest
# and longest cases).
set(STRADA_RUNTIME_TEST_SAMPLE_FILES "*StradaRuntimeTests?Source?SampleTests.cpp,*StradaRuntimeTests?Source?SaveGameTests.cpp")
# The repository's templates, each through a project made from it.
set(STRADA_RUNTIME_TEST_TEMPLATE_FILES "*StradaRuntimeTests?Source?TemplateTests.cpp")

set(STRADA_TEST_SHARDS Samples Templates Core)

set(STRADA_TEST_SHARD_Samples "--source-file=${STRADA_RUNTIME_TEST_SAMPLE_FILES}")
set(STRADA_TEST_SHARD_Templates "--source-file=${STRADA_RUNTIME_TEST_TEMPLATE_FILES}")
# Everything else: the player's command line, test runs, exported games, windows and screenshots, profiles and logs, and
# Moto's sound captures (MotoSoundTests.cpp: five runs of the sample's game, kept out of Samples for its time).
set(STRADA_TEST_SHARD_Core "--source-file-exclude=${STRADA_RUNTIME_TEST_SAMPLE_FILES},${STRADA_RUNTIME_TEST_TEMPLATE_FILES}")
