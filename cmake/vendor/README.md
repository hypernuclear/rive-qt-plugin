# Vendored build tools

Self-contained binaries the build needs at configure/build time. Checked
in so that fresh dev machines can build without external installs of
optional toolchains.

| File | Source | Notes |
|------|--------|-------|
| `premake5-windows-x64.exe` | Premake project, v5.0.0-beta8 | Drives Rive's premake-based build on Windows. See `cmake/BuildRive.cmake`. |
| `glslangValidator-windows-x64.exe` | [Khronos glslang](https://github.com/KhronosGroup/glslang), `main-tot` rolling release, version 16.2.0 | GLSL → SPIR-V compiler. Required when building Rive with `--with_vulkan` (CMake option `RIVE_QT_WITH_VULKAN=ON`). |
| `glslang-LICENSE.txt` | Khronos glslang, BSD-3-Clause + Apache-2.0 + others (multi-license) | Redistribution-license file for `glslangValidator-windows-x64.exe`. |
| `spirv-opt-windows-x64.exe` | [Google shaderc](https://github.com/google/shaderc) continuous build (`shaderc/windows/continuous_release_2019/91/20250625-115528`); upstream is [Khronos SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) v2025.3 | SPIR-V optimizer. Required when building Rive with `--with_vulkan` (Rive's shader Makefile pipes glslangValidator's output through `spirv-opt -O` for every shader). Depends on the shared DLL below. |
| `SPIRV-Tools-shared-windows-x64.dll` | Same shaderc bundle | Runtime dep of `spirv-opt-windows-x64.exe`. The build wrapper copies both into a tool-shim dir under their bare names so the DLL is loadable. |
| `spirv-tools-LICENSE.txt` | Khronos SPIRV-Tools, Apache-2.0 | Redistribution-license file for `spirv-opt-windows-x64.exe` + the shared DLL. |

## glslangValidator provenance

- Fetched from: `https://github.com/KhronosGroup/glslang/releases/download/main-tot/glslang-master-windows-Release.zip`
- Fetched on: 2026-04-27
- Zip SHA-256: `d48196522e8fcb1befef6943390741170a8a0010cc41deca816fcc9951a1ed97`
- Reported version (per `glslangValidator --version`): `Glslang Version: 11:16.2.0`

The zip ships a self-contained `bin/glslangValidator.exe` (no DLL deps);
we extract just that binary and discard the rest.

## spirv-opt provenance

- Fetched from: `https://storage.googleapis.com/shaderc/artifacts/prod/graphics_shader_compiler/shaderc/windows/continuous_release_2019/91/20250625-115528/install.zip` (the URL pointed to by Google shaderc's [Windows VS2019 release badge](https://storage.googleapis.com/shaderc/badges/build_link_windows_vs2019_release.html))
- Fetched on: 2026-04-27
- Zip SHA-256: `a3ea4ff01d54f35f9c53d755cae0d3eba504cace1d8a13d2511b269583b69d31`
- Reported version (per `spirv-opt --version`): `SPIRV-Tools v2025.3 v2025.3.rc1-4-g604c3e75`

`spirv-opt.exe` dynamically links `SPIRV-Tools-shared.dll` — both binaries
must be present in the same directory at execution time. The build
wrapper copies them as a pair into `<build>/rive-build/tool-shims/`
under their bare names (without the `-windows-x64` suffix) so
`spirv-opt.exe` finds the DLL via standard Windows loader rules.

## Updating

The `main-tot` tag is rolling — Khronos retags it whenever the main
branch advances. To refresh:

1. Re-download the zip from the URL above.
2. Compute its SHA-256 and update the table.
3. `unzip -j <zip> "bin/glslangValidator.exe" -d cmake/vendor/`
4. Rename to `glslangValidator-windows-x64.exe`.
5. Sanity-check with `glslangValidator-windows-x64.exe --version`.
6. Commit.

If Khronos starts shipping prebuilt binaries on tagged stable releases
again, switch to a pinned tag URL — that's preferable to chasing
`main-tot`.
