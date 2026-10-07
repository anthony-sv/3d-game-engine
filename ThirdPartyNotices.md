# Third-party notices

Strada uses the following third-party software. Each is downloaded at configure time from the listed source at a
pinned version and verified by SHA256 (see `cmake/StradaDependencies.cmake`). Their licenses apply to their code.

| Library | Version | License | Source | Used by |
|---------|---------|---------|--------|---------|
| GLFW | 3.5.1 | zlib/libpng | https://github.com/glfw/glfw | Engine (windowing, input) |
| glm | 1.0.3 | MIT (or Happy Bunny) | https://github.com/g-truc/glm | Engine (math) |
| spdlog (bundles {fmt}) | 1.17.0 | MIT | https://github.com/gabime/spdlog | Engine (logging) |
| nlohmann/json | 3.12.0 | MIT | https://github.com/nlohmann/json | Engine (serialization) |
| doctest | 2.5.3 | MIT | https://github.com/doctest/doctest | Tests only (not distributed) |
