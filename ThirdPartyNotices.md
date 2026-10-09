# Third-party notices

Strada uses the following third-party software. Each is downloaded at configure time from the listed source at a
pinned version and verified by SHA256 (see `cmake/StradaDependencies.cmake`). Their licenses apply to their code.

| Library | Version | License | Source | Used by |
|---------|---------|---------|--------|---------|
| GLFW | 3.5.1 | zlib/libpng | https://github.com/glfw/glfw | Engine (windowing, input) |
| glm | 1.0.3 | MIT (or Happy Bunny) | https://github.com/g-truc/glm | Engine (math) |
| spdlog (bundles {fmt}) | 1.17.0 | MIT | https://github.com/gabime/spdlog | Engine (logging) |
| nlohmann/json | 3.12.0 | MIT | https://github.com/nlohmann/json | Engine (serialization) |
| EnTT | 4.0.0 | MIT | https://github.com/skypjack/entt | Engine (entity component system) |
| NVRHI | main @ 6b96fb0 (2026-10-05) | MIT | https://github.com/NVIDIA-RTX/NVRHI | Engine (rendering hardware interface) |
| Vulkan-Headers | vulkan-sdk-1.4.363.0 | Apache-2.0 / MIT | https://github.com/KhronosGroup/Vulkan-Headers | Engine (Vulkan API headers) |
| Dear ImGui | 1.92.9b (docking) | MIT | https://github.com/ocornut/imgui | Engine and editor (UI) |
| ImGuizmo | @ 18cef5e (2026-08-08) | MIT | https://github.com/CedricGuillemet/ImGuizmo | Editor (transform gizmos) |
| nativefiledialog-extended | 1.3.0 | zlib | https://github.com/btzy/nativefiledialog-extended | Editor (native file dialogs) |
| stb (stb_image, stb_image_write, stb_truetype) | @ 2c980bb | MIT or Public Domain | https://github.com/nothings/stb | Engine (image I/O, font validation and glyph rendering) |
| cgltf | 1.15 | MIT | https://github.com/jkuhlmann/cgltf | Engine (glTF 2.0 import) |
| ufbx | 0.23.1 | MIT or Unlicense | https://github.com/ufbx/ufbx | Engine (FBX and OBJ import) |
| MikkTSpace | @ 3e895b4 | zlib | https://github.com/mmikk/MikkTSpace | Engine (tangent generation) |
| Roboto Medium (font, from Dear ImGui's `misc/fonts`) | Dear ImGui 1.92.9b | Apache-2.0 | https://fonts.google.com/specimen/Roboto | Engine (default text font, compiled into the engine) |
| doctest | 2.5.3 | MIT | https://github.com/doctest/doctest | Tests only (not distributed) |
