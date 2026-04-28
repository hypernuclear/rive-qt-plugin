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

# Vulkan backend opt-in. Default ON because the Rive PLS premake
# downloads Vulkan-Headers + VulkanMemoryAllocator from GitHub on first
# configure (~2-3 min one-time hit), but no system Vulkan SDK install
# is required (we use Qt's QVulkanDeviceFunctions for raw vk* calls
# and Rive's vendored function-pointer dispatch for the rest).
# Set to OFF to skip the Vulkan backend (e.g. CI without network access
# or environments where the GitHub clones fail).
option(RIVE_QT_WITH_VULKAN
    "Build the Vulkan backend (downloads Vulkan-Headers + VMA)" ON)

# ----------------------------------------------------------------------------
# Qt
# ----------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)

# ----------------------------------------------------------------------------
# rive-runtime — built via premake (see cmake/BuildRive.cmake).
# ----------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildRive.cmake)

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
    src/rive/rive_input.cpp
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
