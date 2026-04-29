cmake_minimum_required(VERSION 3.21)

project(rive_qt
    VERSION 0.1.0
    DESCRIPTION "QPainter-backed Qt/QML plugin for Rive animations"
    LANGUAGES C CXX)

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
# ON when this repo is the top-level project (standalone viewer build);
# OFF when consumed via add_subdirectory from a host project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_rive_qt_is_top_level ON)
else()
    set(_rive_qt_is_top_level OFF)
endif()

option(RIVE_QT_BUILD_EXAMPLES
    "Build the standalone viewer example" ${_rive_qt_is_top_level})

# Vulkan backend opt-in. Default ON on all platforms.
#
# When ON, the Rive PLS premake downloads Vulkan-Headers +
# VulkanMemoryAllocator from GitHub on first configure (~2-3 min
# one-time hit) and compiles SPIR-V shaders via glslangValidator +
# spirv-opt. The runtime itself doesn't need a system Vulkan SDK — we
# use Qt's QVulkanDeviceFunctions for raw vk* calls and Rive's vendored
# function-pointer dispatch for the rest — but the build does need
# glslang + spirv-tools available somewhere.
#
# Tool resolution per platform:
#   - Windows: glslangValidator + spirv-opt vendored under cmake/vendor/.
#   - macOS:   resolved at build time from $VULKAN_SDK, Homebrew
#              (/opt/homebrew/bin, /usr/local/bin), or PATH. Install
#              via `brew install glslang spirv-tools` or the Vulkan SDK
#              from https://vulkan.lunarg.com/sdk/home#mac. Vulkan on
#              macOS runs through MoltenVK (Qt's QVulkan integration
#              loads it transparently); the platform-native Metal
#              backend is also built and stays the default at runtime.
#   - Linux:   glslangValidator + spirv-tools from distro repos.
#
# Set to OFF to skip the Vulkan backend (e.g. CI without network access
# or environments where glslang isn't installed).
option(RIVE_QT_WITH_VULKAN
    "Build the Vulkan backend (downloads Vulkan-Headers + VMA)" ON)

# D3D12 backend opt-in. Windows-only. Default ON because the runtime's
# D3D12 backend is always built on Windows (no premake flag) — the only
# cost of opt-out is dropping the wrapper + d3dx12.h dependency.
option(RIVE_QT_WITH_D3D12
    "Build the D3D12 backend (Windows only)" ON)

# Lua/Luau scripting opt-in. Default ON to match every other Rive runtime
# binding (wasm, react, unity, ios, android, unreal all ship with
# scripting compiled in). Adds a one-time download of luigi-rosso/luau
# (Roblox's Lua dialect) + libhydrogen on first build. Set OFF to skip
# scripting for size-sensitive builds — `.riv` files containing scripts
# will then load but their scripted listeners/transitions silently no-op.
option(RIVE_QT_WITH_SCRIPTING
    "Build the rive runtime with Lua scripting support" ON)

# Audio opt-in. Default ON. We pass --with_rive_audio=system to premake,
# which enables miniaudio in full playback mode — rive opens the OS
# audio device directly (WASAPI on Windows, CoreAudio on macOS,
# ALSA/PulseAudio on Linux) and plays sound effects from .riv files
# without any host glue code. miniaudio runs alongside Qt's audio fine
# (modern OS audio APIs are shared-mode). If you'd rather route rive's
# audio through Qt's stack (e.g. for unified volume control), switch
# the flag in cmake/BuildRive.cmake from `system` to `external` and
# wire a custom QAudioSink — out of scope for this default.
option(RIVE_QT_WITH_AUDIO
    "Build the rive runtime with audio playback (miniaudio system mode)" ON)

# Rive "tools" mode. Enables features the editor + dev workflows expect:
#   - scripts can register without a verified Rive signature (so unsigned
#     dev/test .riv files work), per file.cpp ScriptAsset registration.
#   - the Luau compiler is built (lets scripts ship as source, not just
#     pre-compiled signed bytecode).
#   - libhydrogen runs in full crypto mode rather than verify-only.
#   - extra editor-side instrumentation hooks across viewmodel + scripted/
#     paths.
# Default ON — matches what every other Rive binding ships. Cost is
# modest (~1-3 MB across the rive lib). Flip OFF for production builds
# that only consume signed .riv files from a trusted authoring pipeline.
option(RIVE_QT_WITH_RIVE_TOOLS
    "Enable rive's tools mode (allows unsigned scripts, builds Luau compiler)" ON)

# ----------------------------------------------------------------------------
# Qt
# ----------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

# Vulkan auto-degrade. The plugin's Vulkan backend (rive_vulkan_backend.cpp)
# uses QVulkanDeviceFunctions / QVulkanInstance from QtGui, which only
# exist when Qt itself was configured with Vulkan support
# (QT_FEATURE_vulkan=ON, requires Vulkan headers visible at Qt's
# configure time). The standard macOS Qt Online Installer build ships
# with QT_FEATURE_vulkan=OFF; same can happen on Linux distros that
# don't pull in vulkan headers when building Qt.
#
# Rather than fail the build when the user's Qt doesn't support Vulkan,
# silently drop the Vulkan backend and the rive --with_vulkan flag.
# Other backends (Metal on Apple, D3D11/D3D12 on Windows, GL on Linux)
# remain available. Users who want Vulkan on macOS need a Qt with
# Vulkan support — typically that means building Qt from source against
# the LunarG Vulkan SDK headers, since the prebuilt installer images
# don't ship them.
if(RIVE_QT_WITH_VULKAN AND NOT QT_FEATURE_vulkan)
    message(STATUS
        "RIVE_QT_WITH_VULKAN=ON but Qt was built without Vulkan support "
        "(QT_FEATURE_vulkan=OFF). Disabling the Vulkan backend; the "
        "rive runtime will be built without --with_vulkan too. To use "
        "Vulkan on this platform, configure Qt with -DFEATURE_vulkan=ON "
        "against the LunarG Vulkan SDK headers and rebuild Qt.")
    set(RIVE_QT_WITH_VULKAN OFF)
endif()

# ----------------------------------------------------------------------------
# rive-runtime — built via premake (see cmake/BuildRive.cmake).
# ----------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildRive.cmake)

# ----------------------------------------------------------------------------
# rive-asset-gen build-time tool + the qt_add_rive_assets() helper.
# ----------------------------------------------------------------------------
add_subdirectory(tools/rive-asset-gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/RiveAssets.cmake)

# ----------------------------------------------------------------------------
# Plugin library + QML module
# ----------------------------------------------------------------------------
# Exposed to host projects as `rive_qt::rive_qt` (alias below). The QML
# module URI (`Hypernuclear.Rive`) registers RiveView as a QML element in
# that namespace.
qt_add_qml_module(rive_qt
    URI Hypernuclear.Rive
    VERSION ${PROJECT_VERSION}
    STATIC
    SOURCES
        src/backends/rive_render_backend.cpp
        src/backends/rive_render_backend.h
        src/rive/rive_artboard.cpp
        src/rive/rive_artboard.h
        src/rive/rive_file.cpp
        src/rive/rive_file.h
        src/rive/rive_key_map.cpp
        src/rive/rive_key_map.h
        src/rive/rive_qt_factory.cpp
        src/rive/rive_qt_factory.h
        src/rive/rive_state_machine.cpp
        src/rive/rive_state_machine.h
        src/rive/rive_view_model.cpp
        src/rive/rive_view_model.h
        src/rive/rive_vm_property.cpp
        src/rive/rive_vm_property.h
        src/rive_view.cpp
        src/rive_view.h
)

# Metal backend — Apple only.
if(APPLE)
    target_sources(rive_qt PRIVATE
        src/backends/metal/rive_metal_backend.h
        src/backends/metal/rive_metal_backend.mm
    )
endif()

# D3D12 backend — Windows-only, opt-in via RIVE_QT_WITH_D3D12. Available
# whenever the runtime's d3d12 sources compiled (always on Windows).
# RIVE_QT_HAVE_D3D12 is the runtime gate read by the dispatch.
if(WIN32 AND RIVE_QT_WITH_D3D12)
    target_sources(rive_qt PRIVATE
        src/backends/d3d12/rive_d3d12_backend.h
        src/backends/d3d12/rive_d3d12_backend.cpp
    )
    target_compile_definitions(rive_qt PRIVATE RIVE_QT_HAVE_D3D12=1)
else()
    target_compile_definitions(rive_qt PRIVATE RIVE_QT_HAVE_D3D12=0)
endif()

# D3D11 backend — Windows only.
if(WIN32)
    target_sources(rive_qt PRIVATE
        src/backends/d3d11/rive_d3d11_backend.h
        src/backends/d3d11/rive_d3d11_backend.cpp
    )
    # NOMINMAX so windows.h's min/max function-like macros don't tokenize
    # std::min / std::max in Rive's headers (TUs that pull in d3d11.h see
    # windows.h transitively). WIN32_LEAN_AND_MEAN trims the rest of the
    # SDK macro surface for speed + collision avoidance. Target-wide
    # because anything reaching for the D3D backend's headers transitively
    # gets windows.h.
    target_compile_definitions(rive_qt PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# OpenGL backend — Windows + Linux. macOS is excluded: Apple deprecated
# GL, and we have Metal there anyway.
if(NOT APPLE)
    target_sources(rive_qt PRIVATE
        src/backends/gl/rive_gl_backend.h
        src/backends/gl/rive_gl_backend.cpp
    )
endif()

# Vulkan backend — opt-in via RIVE_QT_WITH_VULKAN (default ON, set in
# the options block at the top). Available on all platforms when on.
# RIVE_QT_HAVE_VULKAN is the runtime gate the dispatch and backend code
# read; keep separate from the build-system option so that tests and
# code paths stay readable.
if(RIVE_QT_WITH_VULKAN)
    target_sources(rive_qt PRIVATE
        src/backends/vulkan/rive_vulkan_backend.h
        src/backends/vulkan/rive_vulkan_backend.cpp
    )
    target_compile_definitions(rive_qt PRIVATE RIVE_QT_HAVE_VULKAN=1)
else()
    target_compile_definitions(rive_qt PRIVATE RIVE_QT_HAVE_VULKAN=0)
endif()

# Rive is built with RTTI off (see rive_build_config.lua — "nonstandard for
# Rive"). Every TU that touches rive types must match or the linker demands
# typeinfo symbols rive never emitted. Per-file so host projects don't
# inherit -fno-rtti.
set_source_files_properties(
    src/rive/rive_artboard.cpp
    src/rive/rive_file.cpp
    src/rive/rive_key_map.cpp
    src/rive/rive_qt_factory.cpp
    src/rive/rive_state_machine.cpp
    src/rive/rive_view_model.cpp
    src/rive/rive_vm_property.cpp
    src/rive_view.cpp
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
)

# D3D11 backend touches rive types (RenderContextD3DImpl, RenderTargetD3D);
# match Rive's RTTI-off ABI same as the rive/ wrappers above.
if(WIN32)
    set_source_files_properties(
        src/backends/d3d11/rive_d3d11_backend.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
    )
endif()

# D3D12 backend — same RTTI-off ABI match.
if(WIN32 AND RIVE_QT_WITH_D3D12)
    set_source_files_properties(
        src/backends/d3d12/rive_d3d12_backend.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
    )
endif()

# GL backend pulls in rive::gpu types too — same RTTI-off ABI match.
if(NOT APPLE)
    set_source_files_properties(
        src/backends/gl/rive_gl_backend.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
    )
endif()

# Vulkan backend pulls in rive::gpu::* and Rive's vkutil types — match.
if(RIVE_QT_WITH_VULKAN)
    set_source_files_properties(
        src/backends/vulkan/rive_vulkan_backend.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
    )
endif()

# The Metal backend .mm uses ARC for its id<MTL...> members.
if(APPLE)
    set_source_files_properties(
        src/backends/metal/rive_metal_backend.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-fno-rtti"
    )
endif()

# QRhi headers we use (qrhi.h, qrhi_platform.h) live under QtGui's
# private headers in 6.10. Public in 6.6+ but the Metal native handles
# struct stayed under the rhi/ subpath. Pick up Qt6::GuiPrivate so the
# include search path is correct.
find_package(Qt6 6.5 REQUIRED COMPONENTS GuiPrivate)

target_include_directories(rive_qt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include/rive_qt>
    PRIVATE
        # Qt's auto-generated qml type registration file includes our
        # headers by basename (`<rive_vm_property.h>`), so the rive/
        # subdir must be searchable directly.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/rive>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/backends>
)

target_link_libraries(rive_qt
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Qml
    PRIVATE
        Qt6::GuiPrivate
        rive
)

# Alias so consumers can uniformly say rive_qt::rive_qt.
add_library(rive_qt::rive_qt ALIAS rive_qt)

# ----------------------------------------------------------------------------
# Examples
# ----------------------------------------------------------------------------
if(RIVE_QT_BUILD_EXAMPLES)
    add_subdirectory(examples/viewer)
endif()
