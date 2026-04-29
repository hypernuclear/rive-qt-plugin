# BuildRive.cmake
#
# Drives rive-runtime's premake/gmake2/msbuild build from CMake and exposes
# the resulting static libraries + headers as the `rive` INTERFACE target.
#
# Why premake: rive-runtime is built with premake5, not CMake. Rather than
# porting ~hundreds of sources to our CMakeLists, we invoke upstream's own
# build script (build/build_rive.sh on macOS/Linux, build_rive.ps1 on
# Windows). The script self-bootstraps premake5 on first run. Expect the
# first configure to spend 30-60s on that; subsequent configures are fast
# because premake's output is cached.
#
# Build scope:
#   - Core `rive` static lib and its in-tree deps (harfbuzz, sheenbidi,
#     yoga, miniaudio).
#   - Text + layout enabled via premake's default `--with_rive_text
#     --with_rive_layout`.
#   - On Apple and Windows the `rive_pls_renderer` GPU lib is also
#     built so the plugin can render zero-copy via Metal (Apple) or
#     D3D11 (Windows). Image decoding inside .riv files is currently
#     disabled on both via `--no-rive-decoders`: rive_decoders pulls
#     libpng/libjpeg/libwebp from GitHub, which is more dependency
#     surface than we want for v1. The QPainter path used to decode
#     via QImage; the GPU paths will need a follow-up QImage-backed
#     factory wrapper to restore that behaviour.
#
# Consumer usage:
#   target_link_libraries(<target> PRIVATE rive)

# Resolve paths relative to this module's directory so consumer projects
# adding us via add_subdirectory still find the submodule correctly —
# CMAKE_SOURCE_DIR would point at the host's top-level in that case.
get_filename_component(_rive_qt_plugin_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(RIVE_RUNTIME_DIR "${_rive_qt_plugin_dir}/third_party/rive-runtime")
set(RIVE_BUILD_DIR "${CMAKE_BINARY_DIR}/rive-build")

# Pick rive's build config so its CRT matches Qt's on Windows. MSVC refuses
# to link objects with mismatched _ITERATOR_DEBUG_LEVEL or RuntimeLibrary —
# a Qt Debug app (/MDd) can't consume a /MT rive. We track CMAKE_BUILD_TYPE
# and force premake to emit the dynamic CRT via rive's
# --windows_runtime=dynamic_{debug,release} option, since Qt on Windows is
# built dynamic.
#
# Multi-config generators (Visual Studio, Xcode) leave CMAKE_BUILD_TYPE
# empty and pick the config at build time. Our wrapper bakes one config in,
# so we can't satisfy both Debug and Release from a single configure;
# warn and fall back to release. Use Ninja (single-config) if you need
# Debug rive on Windows.
#
# On macOS/Linux there's no CRT to match; debug rive doubles binary size
# and the perf hit isn't worth it during development, so we stay on
# release regardless of CMAKE_BUILD_TYPE.
if(WIN32)
    if(DEFINED CMAKE_CONFIGURATION_TYPES AND CMAKE_CONFIGURATION_TYPES)
        message(WARNING
            "BuildRive: multi-config generator detected. rive will be built "
            "as release/dynamic only — per-config CRT selection requires "
            "regenerating premake at build time, which the current wrapper "
            "doesn't support. Use Ninja for Debug builds.")
        set(RIVE_CONFIG "release")
    else()
        string(TOLOWER "${CMAKE_BUILD_TYPE}" _rive_cmake_build_type_lower)
        if(_rive_cmake_build_type_lower STREQUAL "debug")
            set(RIVE_CONFIG "debug")
        else()
            set(RIVE_CONFIG "release")
        endif()
    endif()
    set(RIVE_WINDOWS_RUNTIME_FLAG "--windows_runtime=dynamic_${RIVE_CONFIG}")
else()
    set(RIVE_CONFIG "release")
    set(RIVE_WINDOWS_RUNTIME_FLAG "")
endif()
set(RIVE_OUT_DIR "${RIVE_BUILD_DIR}/out/${RIVE_CONFIG}")

# Vulkan opt-in. When ON, pass --with_vulkan to premake (triggers
# download of Vulkan-Headers + VMA from GitHub on first run, adds
# rive's vulkan/* sources to rive_pls_renderer.lib). The CMake option
# defaulted ON in the top-level CMakeLists; we read it here.
if(RIVE_QT_WITH_VULKAN)
    set(RIVE_VULKAN_FLAG "--with_vulkan")
    # Rive's dependency.lua names download dirs as <project>_<tag> with
    # `/` -> `_` substitution but preserves case + dashes, so the on-disk
    # name is KhronosGroup_Vulkan-Headers_<tag>, not lowercased.
    set(RIVE_VULKAN_HEADERS_DIR
        "${RIVE_BUILD_DIR}/dependencies/KhronosGroup_Vulkan-Headers_vulkan-sdk-1.4.321/include")
else()
    set(RIVE_VULKAN_FLAG "")
endif()

# Scripting opt-in. When ON, pass --with_rive_scripting to premake.
# Triggers download of luigi-rosso/luau (rive_0_33) + libhydrogen
# (rive_0_1), turns luau_vm into a real static lib (rather than a
# dummy stub), and adds WITH_RIVE_SCRIPTING + libhydrogen.c to the rive
# core lib's compile. Without this flag, scripted listeners and
# transitions in .riv files silently no-op.
if(RIVE_QT_WITH_SCRIPTING)
    set(RIVE_SCRIPTING_FLAG "--with_rive_scripting")
else()
    set(RIVE_SCRIPTING_FLAG "")
endif()

# Rive tools mode. When ON, pass --with_rive_tools to premake. Gated
# on scripting (tools mode without scripting just adds editor hooks
# nobody calls). Effects: unsigned scripts in .riv files register
# normally instead of being silently dropped at runtime; libhydrogen
# is compiled with full crypto rather than verify-only; the Luau
# compiler is built (luau_compiler.lib) so source-form scripts work.
if(RIVE_QT_WITH_SCRIPTING AND RIVE_QT_WITH_RIVE_TOOLS)
    set(RIVE_TOOLS_FLAG "--with_rive_tools")
else()
    set(RIVE_TOOLS_FLAG "")
endif()

# Audio. When ON, pass --with_rive_audio=system to premake. `system`
# enables miniaudio in full playback mode — rive opens the OS audio
# device (WASAPI on Windows, CoreAudio on macOS, ALSA/PulseAudio on
# Linux) and plays sound effects from .riv files directly. The
# alternative `external` mode compiles miniaudio with MA_NO_DEVICE_IO
# (decode only, no playback); use that when the host wants to route
# rive's PCM through its own audio stack (e.g. Qt's QAudioSink) rather
# than miniaudio's. We default to `system` since the common case is
# "designer adds a sound effect to a Rive button click" and `system`
# Just Works without glue code.
if(RIVE_QT_WITH_AUDIO)
    set(RIVE_AUDIO_FLAG "--with_rive_audio=system")
else()
    set(RIVE_AUDIO_FLAG "")
endif()

# Generated wrapper premake file. build_rive.sh expects a premake5.lua in
# the working directory; upstream's root has premake5_v2.lua instead. The
# wrapper just resolves the submodule path and dofiles into upstream.
set(RIVE_PREMAKE_WRAPPER "${RIVE_BUILD_DIR}/premake5.lua")

file(MAKE_DIRECTORY "${RIVE_BUILD_DIR}")
# On Apple and Windows we also build rive_pls_renderer for zero-copy
# rendering — Metal on Apple, D3D11 on Windows. The PLS premake script's
# own platform filters in renderer/premake5_pls_renderer.lua gate which
# backend sources are included (src/metal/* on Apple, src/d3d*/* on
# Windows), so the same dofile invocation builds the right thing for
# each host.
if(APPLE OR WIN32)
    file(WRITE "${RIVE_PREMAKE_WRAPPER}"
"-- Auto-generated by cmake/BuildRive.cmake — do not edit.\n"
"RIVE_RUNTIME_DIR = '${RIVE_RUNTIME_DIR}'\n"
"dofile(RIVE_RUNTIME_DIR .. '/build/rive_build_config.lua')\n"
"dofile(RIVE_RUNTIME_DIR .. '/premake5_v2.lua')\n"
"dofile(RIVE_RUNTIME_DIR .. '/renderer/premake5_pls_renderer.lua')\n")
else()
    file(WRITE "${RIVE_PREMAKE_WRAPPER}"
"-- Auto-generated by cmake/BuildRive.cmake — do not edit.\n"
"RIVE_RUNTIME_DIR = '${RIVE_RUNTIME_DIR}'\n"
"dofile(RIVE_RUNTIME_DIR .. '/build/rive_build_config.lua')\n"
"dofile(RIVE_RUNTIME_DIR .. '/premake5_v2.lua')\n")
endif()

# Static libs produced by the rive build that we consume.
if(WIN32)
    set(_rive_lib_prefix "")
    set(_rive_lib_suffix ".lib")
else()
    set(_rive_lib_prefix "lib")
    set(_rive_lib_suffix ".a")
endif()

set(RIVE_CORE_LIB   "${RIVE_OUT_DIR}/${_rive_lib_prefix}rive${_rive_lib_suffix}")
set(RIVE_HARFBUZZ   "${RIVE_OUT_DIR}/${_rive_lib_prefix}rive_harfbuzz${_rive_lib_suffix}")
set(RIVE_SHEENBIDI  "${RIVE_OUT_DIR}/${_rive_lib_prefix}rive_sheenbidi${_rive_lib_suffix}")
set(RIVE_YOGA       "${RIVE_OUT_DIR}/${_rive_lib_prefix}rive_yoga${_rive_lib_suffix}")
set(RIVE_MINIAUDIO  "${RIVE_OUT_DIR}/${_rive_lib_prefix}miniaudio${_rive_lib_suffix}")

set(RIVE_ALL_LIBS
    "${RIVE_CORE_LIB}"
    "${RIVE_HARFBUZZ}"
    "${RIVE_SHEENBIDI}"
    "${RIVE_YOGA}"
    "${RIVE_MINIAUDIO}"
)

if(APPLE OR WIN32)
    set(RIVE_PLS_RENDERER "${RIVE_OUT_DIR}/${_rive_lib_prefix}rive_pls_renderer${_rive_lib_suffix}")
    list(APPEND RIVE_ALL_LIBS "${RIVE_PLS_RENDERER}")
endif()

# luau_vm: real static lib when --with_rive_scripting is on (else it's
# a dummy.cpp stub built unconditionally and not linked). libhydrogen
# is compiled into the rive core lib directly (premake5_v2.lua line
# 163-165), so no separate lib to link there.
if(RIVE_QT_WITH_SCRIPTING)
    set(RIVE_LUAU_VM "${RIVE_OUT_DIR}/${_rive_lib_prefix}luau_vm${_rive_lib_suffix}")
    list(APPEND RIVE_ALL_LIBS "${RIVE_LUAU_VM}")
endif()

# Wrapper script. Two reasons we need one:
#
# 1. Ninja pre-creates the parent directory of any custom-command OUTPUT, so
#    by the time build_rive.{sh,ps1} runs, "out/release/" already exists.
#    Upstream's script treats a pre-existing OUT dir as an incremental build
#    and fails when its marker file `.rive_premake_args` is missing.
#    Solution: drop the dir if the marker is absent, then delegate.
#
# 2. On macOS we scrub the PATH of Anaconda/homebrew toolchain shims —
#    observed `warning: /opt/homebrew/anaconda3/bin/arm64-apple-darwin20.0.0-ranlib`
#    corrupting librive.a's symbol table. Xcode's clang/ar/ranlib will be
#    found via /usr/bin / CommandLineTools.
#
# On Windows we generate a .ps1 script on disk and invoke with -File. Passing
# multi-statement logic via -Command through CMake's list expansion is a
# quoting nightmare (observed: `&` at arg boundary interpreted as the
# pipeline operator and rejected).
if(WIN32)
    set(RIVE_BUILD_WRAPPER "${RIVE_BUILD_DIR}/build_rive_wrapper.ps1")
    set(RIVE_PREMAKE_TAG "v5.0.0-beta8")
    set(RIVE_PREMAKE_INSTALL_DIR "${RIVE_RUNTIME_DIR}/build/dependencies/premake-core/bin/${RIVE_PREMAKE_TAG}_release")
    set(RIVE_PREMAKE_VENDORED "${CMAKE_CURRENT_LIST_DIR}/vendor/premake5-windows-x64.exe")
    set(RIVE_GLSLANG_VENDORED "${CMAKE_CURRENT_LIST_DIR}/vendor/glslangValidator-windows-x64.exe")
    set(RIVE_SPIRV_OPT_VENDORED "${CMAKE_CURRENT_LIST_DIR}/vendor/spirv-opt-windows-x64.exe")
    set(RIVE_SPIRV_TOOLS_DLL_VENDORED "${CMAKE_CURRENT_LIST_DIR}/vendor/SPIRV-Tools-shared-windows-x64.dll")

    file(TO_NATIVE_PATH "${RIVE_BUILD_DIR}" _rive_build_dir_native)
    file(TO_NATIVE_PATH "${RIVE_RUNTIME_DIR}/build/build_rive.sh" _rive_build_sh_native)
    file(TO_NATIVE_PATH "${RIVE_RUNTIME_DIR}/build/setup_windows_dev.ps1" _rive_setup_ps1_native)
    file(TO_NATIVE_PATH "${RIVE_PREMAKE_INSTALL_DIR}" _rive_premake_install_native)
    file(TO_NATIVE_PATH "${RIVE_PREMAKE_VENDORED}" _rive_premake_vendored_native)
    file(TO_NATIVE_PATH "${RIVE_GLSLANG_VENDORED}" _rive_glslang_vendored_native)
    file(TO_NATIVE_PATH "${RIVE_SPIRV_OPT_VENDORED}" _rive_spirv_opt_vendored_native)
    file(TO_NATIVE_PATH "${RIVE_SPIRV_TOOLS_DLL_VENDORED}" _rive_spirv_tools_dll_vendored_native)

    file(WRITE "${RIVE_BUILD_WRAPPER}"
"# Auto-generated by cmake/BuildRive.cmake — do not edit.\n"
"$ErrorActionPreference = 'Stop'\n"
"Set-Location -LiteralPath '${_rive_build_dir_native}'\n"
"$out = 'out/${RIVE_CONFIG}'\n"
"if ((Test-Path $out) -and -not (Test-Path \"$out/.rive_premake_args\")) {\n"
"    Remove-Item -Recurse -Force $out\n"
"}\n"
"# rive's build_rive.sh aborts hard when the cached premake args from a\n"
"# prior build don't match this run's args. That's the right call for\n"
"# rive's CLI users, but here CMake is the source of truth and a config\n"
"# change (e.g. flipping the CRT flag, toggling decoders) should just\n"
"# rebuild. Wipe if any expected flag is missing so build_rive.sh\n"
"# reconfigures cleanly.\n"
"$expectedFlags = @('${RIVE_WINDOWS_RUNTIME_FLAG}', '--no-rive-decoders', '${RIVE_VULKAN_FLAG}', '${RIVE_SCRIPTING_FLAG}', '${RIVE_TOOLS_FLAG}', '${RIVE_AUDIO_FLAG}')\n"
"$argsFile = Join-Path $out '.rive_premake_args'\n"
"if (Test-Path $argsFile) {\n"
"    $existing = Get-Content -Raw -LiteralPath $argsFile\n"
"    foreach ($f in $expectedFlags) {\n"
"        if ($f -ne '' -and $existing -notmatch [regex]::Escape($f)) {\n"
"            Write-Host \"BuildRive: stale premake config (need $f); wiping $out\"\n"
"            Remove-Item -Recurse -Force $out\n"
"            break\n"
"        }\n"
"    }\n"
"}\n"
"\n"
"# Rive's build_rive.sh shells out to sh. Qt Creator's build env doesn't\n"
"# inherit Git-for-Windows' usr\\bin where sh.exe lives, so find it and\n"
"# prepend to PATH.\n"
"if (-not (Get-Command sh -ErrorAction SilentlyContinue)) {\n"
"    $candidates = @(\n"
"        \"$Env:ProgramFiles\\Git\\usr\\bin\",\n"
"        \"$Env:ProgramFiles\\Git\\bin\",\n"
"        \"\${Env:ProgramFiles(x86)}\\Git\\usr\\bin\",\n"
"        \"$Env:LOCALAPPDATA\\Programs\\Git\\usr\\bin\"\n"
"    )\n"
"    $git = (Get-Command git -ErrorAction SilentlyContinue).Source\n"
"    if ($git) {\n"
"        $candidates += (Join-Path (Split-Path (Split-Path $git)) 'usr\\bin')\n"
"    }\n"
"    foreach ($d in $candidates) {\n"
"        if ($d -and (Test-Path (Join-Path $d 'sh.exe'))) {\n"
"            $Env:Path = \"$d;$Env:Path\"\n"
"            break\n"
"        }\n"
"    }\n"
"    if (-not (Get-Command sh -ErrorAction SilentlyContinue)) {\n"
"        Write-Error 'No sh.exe found. Install Git for Windows.'\n"
"        exit 1\n"
"    }\n"
"}\n"
"\n"
"# Rive's PLS premake invokes 'make' literally to drive shader\n"
"# compilation, and the recipes shell out to 'python3' for the GLSL\n"
"# minifier. Neither is on a stock Windows PATH. Qt's MinGW toolchain\n"
"# (installed alongside the MSVC kit by the Qt installer) ships both,\n"
"# so we lean on it before erroring. mingw32-make.exe is renamed to\n"
"# make.exe via a copy in a build-local shim dir — Rive's invocation\n"
"# isn't configurable.\n"
"$toolShimDir = Join-Path '${_rive_build_dir_native}' 'tool-shims'\n"
"if (-not (Get-Command make -ErrorAction SilentlyContinue)) {\n"
"    $mingwMake = $null\n"
"    foreach ($pat in @(\n"
"        \"$Env:ProgramFiles\\Qt\\Tools\\llvm-mingw*\\bin\\mingw32-make.exe\",\n"
"        \"$Env:ProgramFiles\\Qt\\Tools\\mingw*\\bin\\mingw32-make.exe\",\n"
"        \"C:\\Qt\\Tools\\llvm-mingw*\\bin\\mingw32-make.exe\",\n"
"        \"C:\\Qt\\Tools\\mingw*\\bin\\mingw32-make.exe\"\n"
"    )) {\n"
"        $hit = @(Get-ChildItem -Path $pat -ErrorAction SilentlyContinue) | Select-Object -First 1\n"
"        if ($hit) { $mingwMake = $hit.FullName; break }\n"
"    }\n"
"    if (-not $mingwMake) {\n"
"        $mm = Get-Command mingw32-make -ErrorAction SilentlyContinue\n"
"        if ($mm) { $mingwMake = $mm.Source }\n"
"    }\n"
"    if (-not $mingwMake) {\n"
"        Write-Error \"GNU make not found. Install Qt's MinGW kit via the Qt Maintenance Tool, or install GNU make through MSYS2/winget/scoop and ensure it's on PATH.\"\n"
"        exit 1\n"
"    }\n"
"    New-Item -ItemType Directory -Force -Path $toolShimDir | Out-Null\n"
"    Copy-Item -Force $mingwMake (Join-Path $toolShimDir 'make.exe')\n"
"    $Env:Path = \"$toolShimDir;$Env:Path\"\n"
"    Write-Host \"BuildRive: using $mingwMake as make.exe\"\n"
"}\n"
"\n"
"# Resolve a real python3. The stock Windows PATH on this machine has\n"
"# the WindowsApps\\python3.exe stub which redirects to the Microsoft\n"
"# Store rather than executing — it has to be skipped or the make\n"
"# recipes that invoke 'python3' will fail with the install-prompt.\n"
"$realPython = $null\n"
"$pyCmd = Get-Command python3 -ErrorAction SilentlyContinue\n"
"if ($pyCmd -and $pyCmd.Source -notlike '*\\WindowsApps\\*') {\n"
"    $realPython = $pyCmd.Source\n"
"}\n"
"if (-not $realPython) {\n"
"    foreach ($pat in @(\n"
"        \"$Env:ProgramFiles\\Qt\\Tools\\llvm-mingw*\\python\\bin\\python3.exe\",\n"
"        \"$Env:ProgramFiles\\Qt\\Tools\\mingw*\\python\\bin\\python3.exe\",\n"
"        \"C:\\Qt\\Tools\\llvm-mingw*\\python\\bin\\python3.exe\",\n"
"        \"C:\\Qt\\Tools\\mingw*\\python\\bin\\python3.exe\"\n"
"    )) {\n"
"        $hit = @(Get-ChildItem -Path $pat -ErrorAction SilentlyContinue) | Select-Object -First 1\n"
"        if ($hit) { $realPython = $hit.FullName; break }\n"
"    }\n"
"}\n"
"if (-not $realPython) {\n"
"    Write-Error \"python3 not found. Qt's MinGW kit ships Python 3, or install from python.org and ensure python3.exe is on PATH (not the Microsoft Store stub).\"\n"
"    exit 1\n"
"}\n"
"$pyDir = Split-Path $realPython\n"
"$Env:Path = \"$pyDir;$Env:Path\"\n"
"Write-Host \"BuildRive: using $realPython for shader minifier\"\n"
"\n"
"# glslangValidator (Khronos GLSL->SPIR-V compiler) is needed when\n"
"# Rive is built with --with_vulkan; the spirv Makefile target shells\n"
"# out to it for every shader. Search order:\n"
"#   1. Vendored copy in cmake/vendor/ (checked in for self-contained\n"
"#      builds — see cmake/vendor/README.md for provenance).\n"
"#   2. System Vulkan SDK ($Env:VULKAN_SDK / $Env:VK_SDK_PATH /\n"
"#      C:\\VulkanSDK\\<version>\\Bin\\) for users who already have it.\n"
"#   3. Already-on-PATH copy from elsewhere.\n"
"# Skipped entirely when --with_vulkan isn't passed.\n"
"$expectsVulkan = '${RIVE_VULKAN_FLAG}' -ne ''\n"
"if ($expectsVulkan -and -not (Get-Command glslangValidator -ErrorAction SilentlyContinue)) {\n"
"    $glslang = $null\n"
"    $vendoredGlslang = '${_rive_glslang_vendored_native}'\n"
"    if (Test-Path $vendoredGlslang) {\n"
"        $glslang = $vendoredGlslang\n"
"    } elseif ($Env:VULKAN_SDK -and (Test-Path (Join-Path $Env:VULKAN_SDK 'Bin\\glslangValidator.exe'))) {\n"
"        $glslang = Join-Path $Env:VULKAN_SDK 'Bin\\glslangValidator.exe'\n"
"    } elseif ($Env:VK_SDK_PATH -and (Test-Path (Join-Path $Env:VK_SDK_PATH 'Bin\\glslangValidator.exe'))) {\n"
"        $glslang = Join-Path $Env:VK_SDK_PATH 'Bin\\glslangValidator.exe'\n"
"    } else {\n"
"        # Pick the newest installed SDK if any.\n"
"        $hits = @(Get-ChildItem -Path 'C:\\VulkanSDK\\*\\Bin\\glslangValidator.exe' -ErrorAction SilentlyContinue) | Sort-Object FullName -Descending\n"
"        if ($hits) { $glslang = $hits[0].FullName }\n"
"    }\n"
"    if (-not $glslang) {\n"
"        Write-Error @\"\n"
"BuildRive: glslangValidator not found, but --with_vulkan was requested.\n"
"\n"
"Expected the vendored copy at:\n"
"    $vendoredGlslang\n"
"\n"
"If that file is missing (e.g. a partial git clone), restore it from the\n"
"repo or install the Vulkan SDK from\n"
"https://vulkan.lunarg.com/sdk/home#windows.\n"
"\n"
"Or skip the Vulkan backend by re-configuring with:\n"
"    -DRIVE_QT_WITH_VULKAN=OFF\n"
"\"@\n"
"        exit 1\n"
"    }\n"
"    # Mirror the make.exe-shim approach: drop the binary into a\n"
"    # build-local dir under the bare name 'glslangValidator.exe' (not\n"
"    # 'glslangValidator-windows-x64.exe') so make recipes that hard-\n"
"    # code 'glslangValidator' find it.\n"
"    if (-not (Test-Path $toolShimDir)) { New-Item -ItemType Directory -Force -Path $toolShimDir | Out-Null }\n"
"    $shimGlslang = Join-Path $toolShimDir 'glslangValidator.exe'\n"
"    if (-not (Test-Path $shimGlslang) -or ((Get-Item $shimGlslang).LastWriteTimeUtc -lt (Get-Item $glslang).LastWriteTimeUtc)) {\n"
"        Copy-Item -Force $glslang $shimGlslang\n"
"    }\n"
"    # Make sure the shim dir is on PATH (the make-detection block\n"
"    # only prepends it when 'make' wasn't already found, so on systems\n"
"    # with system make this is the first time we'd add it).\n"
"    if ($Env:Path -notlike \"*$toolShimDir*\") {\n"
"        $Env:Path = \"$toolShimDir;$Env:Path\"\n"
"    }\n"
"    Write-Host \"BuildRive: using $glslang for SPIR-V compilation\"\n"
"}\n"
"\n"
"# spirv-opt — same story as glslangValidator. Rive's shader Makefile\n"
"# pipes every glslangValidator output through 'spirv-opt -O ...' for\n"
"# Vulkan-bound shaders. spirv-opt.exe dynamically links\n"
"# SPIRV-Tools-shared.dll, so both go into the shim dir under their\n"
"# bare names (DLL must sit alongside the .exe for the loader).\n"
"if ($expectsVulkan -and -not (Get-Command spirv-opt -ErrorAction SilentlyContinue)) {\n"
"    $spvOpt = $null\n"
"    $spvDll = $null\n"
"    $vendoredSpvOpt = '${_rive_spirv_opt_vendored_native}'\n"
"    $vendoredSpvDll = '${_rive_spirv_tools_dll_vendored_native}'\n"
"    if ((Test-Path $vendoredSpvOpt) -and (Test-Path $vendoredSpvDll)) {\n"
"        $spvOpt = $vendoredSpvOpt\n"
"        $spvDll = $vendoredSpvDll\n"
"    } elseif ($Env:VULKAN_SDK -and (Test-Path (Join-Path $Env:VULKAN_SDK 'Bin\\spirv-opt.exe'))) {\n"
"        $spvOpt = Join-Path $Env:VULKAN_SDK 'Bin\\spirv-opt.exe'\n"
"        # SDK install has spirv-opt statically linked; no DLL beside it.\n"
"    } elseif ($Env:VK_SDK_PATH -and (Test-Path (Join-Path $Env:VK_SDK_PATH 'Bin\\spirv-opt.exe'))) {\n"
"        $spvOpt = Join-Path $Env:VK_SDK_PATH 'Bin\\spirv-opt.exe'\n"
"    } else {\n"
"        $hits = @(Get-ChildItem -Path 'C:\\VulkanSDK\\*\\Bin\\spirv-opt.exe' -ErrorAction SilentlyContinue) | Sort-Object FullName -Descending\n"
"        if ($hits) { $spvOpt = $hits[0].FullName }\n"
"    }\n"
"    if (-not $spvOpt) {\n"
"        Write-Error @\"\n"
"BuildRive: spirv-opt not found, but --with_vulkan was requested.\n"
"\n"
"Expected the vendored copy at:\n"
"    $vendoredSpvOpt\n"
"\n"
"If that file is missing, restore it from the repo or install the\n"
"Vulkan SDK from https://vulkan.lunarg.com/sdk/home#windows.\n"
"\n"
"Or skip the Vulkan backend by re-configuring with:\n"
"    -DRIVE_QT_WITH_VULKAN=OFF\n"
"\"@\n"
"        exit 1\n"
"    }\n"
"    if (-not (Test-Path $toolShimDir)) { New-Item -ItemType Directory -Force -Path $toolShimDir | Out-Null }\n"
"    $shimSpvOpt = Join-Path $toolShimDir 'spirv-opt.exe'\n"
"    if (-not (Test-Path $shimSpvOpt) -or ((Get-Item $shimSpvOpt).LastWriteTimeUtc -lt (Get-Item $spvOpt).LastWriteTimeUtc)) {\n"
"        Copy-Item -Force $spvOpt $shimSpvOpt\n"
"    }\n"
"    if ($spvDll) {\n"
"        $shimSpvDll = Join-Path $toolShimDir 'SPIRV-Tools-shared.dll'\n"
"        if (-not (Test-Path $shimSpvDll) -or ((Get-Item $shimSpvDll).LastWriteTimeUtc -lt (Get-Item $spvDll).LastWriteTimeUtc)) {\n"
"            Copy-Item -Force $spvDll $shimSpvDll\n"
"        }\n"
"    }\n"
"    if ($Env:Path -notlike \"*$toolShimDir*\") {\n"
"        $Env:Path = \"$toolShimDir;$Env:Path\"\n"
"    }\n"
"    Write-Host \"BuildRive: using $spvOpt for SPIR-V optimization\"\n"
"}\n"
"\n"
"# Pin the tag rive uses for its PREMAKE_INSTALL_DIR path calculation.\n"
"$Env:RIVE_PREMAKE_TAG = '${RIVE_PREMAKE_TAG}'\n"
"\n"
"$installDir = '${_rive_premake_install_native}'\n"
)
    file(APPEND "${RIVE_BUILD_WRAPPER}"
"# Vendored x86-64 premake5.exe runs natively on Windows x64. PE-header\n"
"# check so a stale ARM64 install left over from this repo's earlier\n"
"# native-ARM64 push gets wiped automatically.\n"
"function Get-PeMachine($path) {\n"
"    try {\n"
"        $fs = [System.IO.File]::OpenRead($path)\n"
"        try {\n"
"            $buf = New-Object byte[] 4096\n"
"            [void]$fs.Read($buf, 0, 4096)\n"
"            $peOffset = [BitConverter]::ToInt32($buf, 0x3C)\n"
"            $machine = [BitConverter]::ToUInt16($buf, $peOffset + 4)\n"
"            switch ($machine) {\n"
"                0x014c { 'x86' }\n"
"                0x8664 { 'x64' }\n"
"                0xAA64 { 'arm64' }\n"
"                default { 'other' }\n"
"            }\n"
"        } finally { $fs.Close() }\n"
"    } catch { 'error' }\n"
"}\n"
"$installedExe = Join-Path $installDir 'premake5.exe'\n"
"$installedArch = if (Test-Path $installedExe) { Get-PeMachine $installedExe } else { '' }\n"
"if ($installedArch -ne 'x64' -and (Test-Path $installDir)) {\n"
"    Write-Host \"Wiping stale premake install (detected arch='$installedArch', need x64)\"\n"
"    Remove-Item -Recurse -Force $installDir\n"
"}\n"
"if (-not (Test-Path (Join-Path $installDir 'premake5.exe'))) {\n"
"    Write-Host \"Installing vendored premake5.exe to $installDir\"\n"
"    New-Item -ItemType Directory -Force -Path $installDir | Out-Null\n"
"    Copy-Item -Force '${_rive_premake_vendored_native}' (Join-Path $installDir 'premake5.exe')\n"
"    Copy-Item -Force '${_rive_premake_vendored_native}' (Join-Path $installDir 'premake5')\n"
"}\n"
"\n"
"# Ensure fxc/msbuild on PATH for the rive build itself.\n"
"if (-not (Get-Command fxc -ErrorAction SilentlyContinue)) {\n"
"    & '${_rive_setup_ps1_native}'\n"
"}\n"
"\n"
"# Hand off to rive's script. Lower ErrorActionPreference around the\n"
"# sh call: with 'Stop', PowerShell escalates any native-command stderr\n"
"# (e.g. premake's harmless 'field flags deprecated' warning) into a\n"
"# terminating error, killing the build even though sh itself returned\n"
"# 0. Drive success off $LASTEXITCODE instead.\n"
"$prevPref = $ErrorActionPreference\n"
"$ErrorActionPreference = 'Continue'\n"
"& sh '${_rive_build_sh_native}' '${RIVE_CONFIG}' '${RIVE_WINDOWS_RUNTIME_FLAG}' '--no-rive-decoders' '${RIVE_VULKAN_FLAG}' '${RIVE_SCRIPTING_FLAG}' '${RIVE_TOOLS_FLAG}' '${RIVE_AUDIO_FLAG}' 2>&1\n"
"$riveExit = $LASTEXITCODE\n"
"$ErrorActionPreference = $prevPref\n"
"\n"
"if ($riveExit -ne 0) {\n"
"    # Diagnostic on failure: re-run premake directly so errors surface\n"
"    # (rive's pipe through grep can swallow them).\n"
"    Write-Host ''\n"
"    Write-Host '=== rive script failed. Re-running premake5 directly for diagnostics ==='\n"
"    $pm = Join-Path $installDir 'premake5.exe'\n"
"    if (Test-Path $pm) {\n"
"        & $pm 'vs2022' \"--config=${RIVE_CONFIG}\" \"--out=out/${RIVE_CONFIG}\" '--with_rive_text' '--with_rive_layout' '${RIVE_WINDOWS_RUNTIME_FLAG}' '--no-rive-decoders' '${RIVE_VULKAN_FLAG}' '${RIVE_SCRIPTING_FLAG}' '${RIVE_TOOLS_FLAG}' '${RIVE_AUDIO_FLAG}' 2>&1\n"
"        Write-Host \"=== premake5 standalone exit: $LASTEXITCODE ===\"\n"
"    }\n"
"}\n"
"exit $riveExit\n"
)
else()
    set(RIVE_BUILD_WRAPPER "${RIVE_BUILD_DIR}/build_rive_wrapper.sh")
    # Apple builds the PLS renderer too. Decoders stay disabled — see the
    # comment at the top of this file for why.
    if(APPLE)
        set(_rive_extra_args " --no-rive-decoders")
    else()
        set(_rive_extra_args "")
    endif()
    if(RIVE_VULKAN_FLAG)
        set(_rive_extra_args "${_rive_extra_args} ${RIVE_VULKAN_FLAG}")
    endif()
    if(RIVE_SCRIPTING_FLAG)
        set(_rive_extra_args "${_rive_extra_args} ${RIVE_SCRIPTING_FLAG}")
    endif()
    if(RIVE_TOOLS_FLAG)
        set(_rive_extra_args "${_rive_extra_args} ${RIVE_TOOLS_FLAG}")
    endif()
    if(RIVE_AUDIO_FLAG)
        set(_rive_extra_args "${_rive_extra_args} ${RIVE_AUDIO_FLAG}")
    endif()
    file(WRITE "${RIVE_BUILD_WRAPPER}"
"#!/bin/bash\n"
"# Auto-generated by cmake/BuildRive.cmake — do not edit.\n"
"set -e\n"
"cd \"${RIVE_BUILD_DIR}\"\n"
"RIVE_OUT=\"out/${RIVE_CONFIG}\"\n"
"if [ -d \"$RIVE_OUT\" ] && [ ! -f \"$RIVE_OUT/.rive_premake_args\" ]; then\n"
"    rm -rf \"$RIVE_OUT\"\n"
"fi\n"
"# Rive's build_rive.sh refuses to reconfigure when the cached premake\n"
"# args differ from this run's. Detect a mismatch and wipe so we don't\n"
"# fight the cache when toggling renderer / decoder options.\n"
"if [ -f \"$RIVE_OUT/.rive_premake_args\" ] && [ -n \"${_rive_extra_args}\" ]; then\n"
"    if ! grep -q -- '${_rive_extra_args}' \"$RIVE_OUT/.rive_premake_args\"; then\n"
"        rm -rf \"$RIVE_OUT\"\n"
"    fi\n"
"fi\n"
"# Wipe a stale Windows-built premake5.exe left from a shared-FS\n"
"# Windows build — it'd fool rive's bootstrap test into using a PE\n"
"# binary on a Unix host.\n"
"RIVE_PREMAKE_INSTALL=\"${RIVE_RUNTIME_DIR}/build/dependencies/premake-core/bin/v5.0.0-beta7_release\"\n"
"if [ -f \"$RIVE_PREMAKE_INSTALL/premake5.exe\" ] && [ ! -x \"$RIVE_PREMAKE_INSTALL/premake5\" ]; then\n"
"    rm -rf \"${RIVE_RUNTIME_DIR}/build/dependencies/premake-core\"\n"
"fi\n"
"exec \"${RIVE_RUNTIME_DIR}/build/build_rive.sh\" \"${RIVE_CONFIG}\"${_rive_extra_args}\n")
    execute_process(COMMAND chmod +x "${RIVE_BUILD_WRAPPER}")
endif()

if(APPLE)
    set(RIVE_BUILD_CMD
        "${CMAKE_COMMAND}" -E env
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/Library/Developer/CommandLineTools/usr/bin"
        "${RIVE_BUILD_WRAPPER}"
    )
elseif(WIN32)
    set(RIVE_BUILD_CMD
        powershell -ExecutionPolicy Bypass -NoProfile -File "${RIVE_BUILD_WRAPPER}"
    )
else()
    set(RIVE_BUILD_CMD "${RIVE_BUILD_WRAPPER}")
endif()

# Track rive's source tree so cmake/ninja re-invokes the build wrapper
# when any rive .cpp/.hpp/.h/.mm file changes. Without this, edits to
# the vendored rive sources (whether for local debugging or applying
# upstream patches) get silently ignored — the .lib file's mtime
# satisfies ninja's up-to-date check, the wrapper never runs, msbuild
# is never asked to look at the source delta. CONFIGURE_DEPENDS makes
# CMake re-glob each build so a newly-added file picks up automatically.
file(GLOB_RECURSE _rive_src_files CONFIGURE_DEPENDS
    "${RIVE_RUNTIME_DIR}/src/*.cpp"
    "${RIVE_RUNTIME_DIR}/src/*.hpp"
    "${RIVE_RUNTIME_DIR}/src/*.h"
    "${RIVE_RUNTIME_DIR}/src/*.mm"
    "${RIVE_RUNTIME_DIR}/include/*.hpp"
    "${RIVE_RUNTIME_DIR}/include/*.h"
    "${RIVE_RUNTIME_DIR}/renderer/src/*.cpp"
    "${RIVE_RUNTIME_DIR}/renderer/src/*.hpp"
    "${RIVE_RUNTIME_DIR}/renderer/src/*.h"
    "${RIVE_RUNTIME_DIR}/renderer/src/*.mm"
    "${RIVE_RUNTIME_DIR}/renderer/include/*.hpp"
    "${RIVE_RUNTIME_DIR}/renderer/include/*.h"
)

# Custom build step. BYPRODUCTS is what lets downstream targets take a file
# dependency on the libs (OUTPUT-only wouldn't, because the wrapper premake
# file is regenerated on every configure).
#
# DEPENDS lists every rive source file (globbed above) plus the wrapper.
# When any of them changes, ninja re-runs the wrapper, which delegates to
# msbuild on Windows / make on Unix — both do their own incremental
# compilation, so the rebuild only touches the changed TUs. Steady-state
# overhead when nothing changed: ninja stat call per source (~milliseconds),
# wrapper not invoked.
add_custom_command(
    OUTPUT ${RIVE_ALL_LIBS}
    COMMAND ${RIVE_BUILD_CMD}
    WORKING_DIRECTORY "${RIVE_BUILD_DIR}"
    DEPENDS "${RIVE_PREMAKE_WRAPPER}" ${_rive_src_files}
    COMMENT "Building rive-runtime (first build ~30-60s, then incremental)"
    VERBATIM
)

add_custom_target(rive_external DEPENDS ${RIVE_ALL_LIBS})

# Public-facing target. INTERFACE because we don't own any compiled sources
# for it — we just stitch together the upstream-built libs + headers.
add_library(rive INTERFACE)
add_dependencies(rive rive_external)
target_link_libraries(rive INTERFACE ${RIVE_ALL_LIBS})

# Rive's public headers. Internal-to-rive includes (harfbuzz/src,
# sheenbidi/Headers) aren't needed by consumers since rive wraps them.
# The renderer/ headers (rive/renderer/render_context.hpp etc.) live in a
# separate include root from rive-runtime's core; expose both. They're
# part of the source tree regardless of whether we build the PLS lib —
# RiveQtFactory references the types virtually so no PLS symbols need
# to resolve at link time on platforms where we don't build it.
target_include_directories(rive INTERFACE
    "${RIVE_RUNTIME_DIR}/include"
    "${RIVE_RUNTIME_DIR}/renderer/include"
)

# OpenGL: Rive's GL headers (rive/renderer/gl/gles3.hpp ->
# glad_custom.h -> <glad/gles2.h>) need two extra include roots.
# RIVE_DESKTOP_GL is the gate that switches gles3.hpp's includes from
# Android/WebGL paths to glad_custom.h; Rive's premake defines it for
# consumers too, but premake's "defines outside project" don't cross
# into our CMake build so we re-assert it here.
if(APPLE OR WIN32 OR (UNIX AND NOT APPLE))
    target_include_directories(rive INTERFACE
        "${RIVE_RUNTIME_DIR}/renderer/glad"
        "${RIVE_RUNTIME_DIR}/renderer/glad/include"
    )
    target_compile_definitions(rive INTERFACE RIVE_DESKTOP_GL)
endif()

# Vulkan: Rive's vulkan headers (rive/renderer/vulkan/*) are gated by
# RIVE_VULKAN — entire files are #ifdef'd. Vulkan-Headers v1.4.321
# is downloaded by Rive's premake into ${RIVE_BUILD_DIR}/dependencies/
# during the rive_external build step, so the path exists by the time
# any consumer TU compiles. VK_NO_PROTOTYPES suppresses vulkan.h's
# extern "C" declarations of vk* functions, which would otherwise
# create link-time dependencies on vulkan-1.lib — we reach all vk*
# calls via Qt's QVulkanDeviceFunctions / Rive's VulkanContext
# function-pointer table.
if(RIVE_QT_WITH_VULKAN)
    target_include_directories(rive INTERFACE "${RIVE_VULKAN_HEADERS_DIR}")
    target_compile_definitions(rive INTERFACE RIVE_VULKAN VK_NO_PROTOTYPES)
endif()

# Scripting: Rive's File / StateMachineInstance / etc. headers gate
# their scripting member declarations on WITH_RIVE_SCRIPTING. Consumers
# (RiveFile, RiveStateMachine wrappers) need to see the same gate the
# runtime was compiled with so member offsets and ABI match.
if(RIVE_QT_WITH_SCRIPTING)
    target_compile_definitions(rive INTERFACE WITH_RIVE_SCRIPTING)
endif()

# Tools mode: same ABI-match concern. Runtime compiles a different
# member layout for ViewModelInstance* and friends when WITH_RIVE_TOOLS
# is on (extra observation hooks); consumers must agree.
if(RIVE_QT_WITH_SCRIPTING AND RIVE_QT_WITH_RIVE_TOOLS)
    target_compile_definitions(rive INTERFACE WITH_RIVE_TOOLS)
endif()

# Audio: rive's headers gate audio component types and state-machine
# input wiring on WITH_RIVE_AUDIO. Consumers must see the same gate so
# any audio-aware codepath compiles consistently. Required for .riv
# files containing audio (rive engine drops audio assets at import
# without this define).
if(RIVE_QT_WITH_AUDIO)
    target_compile_definitions(rive INTERFACE WITH_RIVE_AUDIO)
endif()

# Windows: PLS uses D3D11 + DXGI; D3DCompiler is needed for runtime
# shader compilation (rive's premake build emits HLSL shaders that the
# pipeline manager compiles on first use). opengl32 covers the GL
# backend's link surface (Rive's GL backend dispatches via GLAD-loaded
# function pointers, so we don't strictly need the import lib for
# extension entry points — but glGetString and the few non-extension
# core entry points still link directly against opengl32).
if(WIN32)
    target_link_libraries(rive INTERFACE
        d3d11
        d3d12
        dxgi
        dxguid
        d3dcompiler
        opengl32
    )
    # D3D12 backend pulls in <d3dx12.h>. The Microsoft DirectX-Headers
    # repo ships these under `directx/` but Rive includes `<d3dx12.h>`
    # at the top level — premake downloads the headers into
    # dependencies/microsoft_DirectX-Headers_<tag>/include/directx/ on
    # first build. Hardcode the tag (matches Vulkan-Headers pattern):
    # using GLOB at configure time misses fresh build dirs where rive
    # hasn't run yet, so the include path goes unrecorded. Tag pinned
    # in renderer/premake5_pls_renderer.lua. Bump when rive bumps it.
    set(RIVE_DX_HEADERS_DIR
        "${RIVE_BUILD_DIR}/dependencies/microsoft_DirectX-Headers_v1.615.0/include/directx")
    target_include_directories(rive INTERFACE "${RIVE_DX_HEADERS_DIR}")
endif()

# Linux: GL backend needs libGL. Apple GL on macOS comes via Qt's link
# of the OpenGL.framework, so no extra link there (we don't actually
# build the GL backend on Apple anyway — Metal is preferred).
if(UNIX AND NOT APPLE)
    target_link_libraries(rive INTERFACE GL)
endif()

# Frameworks rive's Apple font backend (src/text/font_hb_apple.mm) needs,
# plus Metal/QuartzCore for the PLS Metal renderer's command-buffer
# encoding and CAMetalLayer interop.
if(APPLE)
    target_link_libraries(rive INTERFACE
        "-framework Foundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework Metal"
        "-framework QuartzCore"
    )
endif()
