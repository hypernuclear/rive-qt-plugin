# Wrapper test suite. Driven by CTest; built only when RIVE_QT_BUILD_TESTS
# is ON (default when this repo is top-level). Layers:
#   - unit/        pure helpers (no rive, no GPU)
#   - file/ artboard/ state_machine/ view_model/ asset_loader/
#                  headless wrapper-contract tests via rive::NoOpFactory
#   - render/      offscreen Metal render smoke (APPLE only)
#
# Each test links the `rive_qt` wrapper lib + the `rive` INTERFACE target
# (which carries the runtime include roots AND the ABI-gating defines —
# WITH_RIVE_SCRIPTING / WITH_RIVE_TOOLS / WITH_RIVE_AUDIO / RIVE_VULKAN —
# so wrapper member layouts match what the runtime was compiled with).
# The headless factory (rive::NoOpFactory) is compiled per-target from the
# runtime's utils/, exactly as tools/rive-asset-gen does.

# Absolute paths handed to tests as compile definitions so fixtures load
# regardless of the working directory (mirrors the viewer's
# RIVE_VIEWER_SAMPLES_DIR define).
set(RIVE_QT_TEST_FIXTURES_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(RIVE_QT_TEST_SNAPSHOTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/snapshots")

# Shared helpers: fixture-path resolver + image snapshot compare/update.
add_library(riveqt_test_helpers STATIC
    common/test_helpers.cpp
    common/test_helpers.h
)
target_link_libraries(riveqt_test_helpers PUBLIC Qt6::Core Qt6::Gui)
target_include_directories(riveqt_test_helpers PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common")
target_compile_features(riveqt_test_helpers PUBLIC cxx_std_20)
target_compile_definitions(riveqt_test_helpers PUBLIC
    RIVE_QT_TEST_FIXTURES_DIR="${RIVE_QT_TEST_FIXTURES_DIR}"
    RIVE_QT_TEST_SNAPSHOTS_DIR="${RIVE_QT_TEST_SNAPSHOTS_DIR}")

# Convenience function: register a test executable that drives the wrapper
# headlessly. Compiles the runtime's NoOpFactory in, links rive_qt + rive +
# helpers, sets the rive include roots, matches Rive's RTTI-off ABI, and
# forces the offscreen QPA so QGuiApplication-based tests need no display.
#   riveqt_add_headless_test(<name> SUITE <Suite> SLOTS slotA slotB ...)
# Tests register as `<Suite>.<slot>` (Suite defaults to the target name).
function(riveqt_add_headless_test name)
    cmake_parse_arguments(ARG "" "SUITE" "SLOTS" ${ARGN})
    if(NOT ARG_SUITE)
        set(ARG_SUITE "${name}")
    endif()

    qt_add_executable(${name}
        ${name}.cpp
        "${RIVE_RUNTIME_DIR}/utils/no_op_factory.cpp"
    )
    target_link_libraries(${name} PRIVATE
        Qt6::Test rive_qt rive riveqt_test_helpers)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/rive"
        "${CMAKE_SOURCE_DIR}/src/backends")
    target_compile_features(${name} PRIVATE cxx_std_20)
    # TUs that include rive headers must match Rive's RTTI-off build or the
    # linker demands typeinfo rive never emitted (project-wide convention).
    set_source_files_properties(
        ${name}.cpp
        "${RIVE_RUNTIME_DIR}/utils/no_op_factory.cpp"
        TARGET_DIRECTORY ${name}
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")

    foreach(slot IN LISTS ARG_SLOTS)
        add_test(NAME ${ARG_SUITE}.${slot} COMMAND ${name} ${slot})
    endforeach()

    # Offscreen QPA: QGuiApplication spins up with no window server, so the
    # suite runs on headless CI. Windows also needs Qt's bin/ on PATH for
    # the test DLLs.
    get_property(_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_tests} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        cmake_path(GET Qt6_DIR PARENT_PATH _qt_cmake_dir)
        cmake_path(GET _qt_cmake_dir PARENT_PATH _qt_lib_dir)
        cmake_path(GET _qt_lib_dir PARENT_PATH _qt_prefix)
        set_tests_properties(${_tests} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${_qt_prefix}/bin;$ENV{PATH}")
    endif()
endfunction()

add_subdirectory(unit)
add_subdirectory(file)
add_subdirectory(artboard)
add_subdirectory(state_machine)
add_subdirectory(view_model)
add_subdirectory(asset_loader)

# Render smoke is Metal/macOS-only today (the only backend built on Apple).
if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/render/CMakeLists.txt")
    add_subdirectory(render)
endif()
