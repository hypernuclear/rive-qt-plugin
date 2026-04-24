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
        src/rive_painter_factory.cpp
        src/rive_painter_factory.h
        src/rive_painter_renderer.cpp
        src/rive_painter_renderer.h
        src/rive_view.cpp
        src/rive_view.h
)

# Rive is built with RTTI off (see rive_build_config.lua — "nonstandard for
# Rive"). Our TUs that subclass rive's Render* types must match or the
# linker demands typeinfo symbols rive never emitted. Per-file so host
# projects don't inherit -fno-rtti.
set_source_files_properties(
    src/rive_painter_factory.cpp
    src/rive_painter_renderer.cpp
    src/rive_view.cpp
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
)

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
