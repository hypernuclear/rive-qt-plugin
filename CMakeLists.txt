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
        src/backends/rive_render_backend.h
        src/rive/rive_artboard.cpp
        src/rive/rive_artboard.h
        src/rive/rive_event.h
        src/rive/rive_file.cpp
        src/rive/rive_file.h
        src/rive/rive_input.cpp
        src/rive/rive_input.h
        src/rive/rive_key_map.cpp
        src/rive/rive_key_map.h
        src/rive/rive_state_machine.cpp
        src/rive/rive_state_machine.h
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

# Rive is built with RTTI off (see rive_build_config.lua — "nonstandard for
# Rive"). Every TU that touches rive types must match or the linker demands
# typeinfo symbols rive never emitted. Per-file so host projects don't
# inherit -fno-rtti.
set_source_files_properties(
    src/rive/rive_artboard.cpp
    src/rive/rive_file.cpp
    src/rive/rive_input.cpp
    src/rive/rive_key_map.cpp
    src/rive/rive_state_machine.cpp
    src/rive_view.cpp
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
)

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
