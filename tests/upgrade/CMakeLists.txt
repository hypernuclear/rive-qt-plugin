qt_add_executable(tst_runtime_upgrade
    tst_runtime_upgrade.cpp
    "${RIVE_RUNTIME_DIR}/utils/no_op_factory.cpp")
target_link_libraries(tst_runtime_upgrade PRIVATE
    Qt6::Test Qt6::Quick rive_qt rive riveqt_test_helpers)
if(APPLE)
    target_link_libraries(tst_runtime_upgrade PRIVATE Qt6::GuiPrivate)
endif()
target_include_directories(tst_runtime_upgrade PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/rive")
target_compile_options(tst_runtime_upgrade PRIVATE
    "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")
set(_catalog "${PROJECT_SOURCE_DIR}/examples/viewer/upgrade-samples.json")
target_compile_definitions(tst_runtime_upgrade PRIVATE
    RIVE_QT_UPGRADE_CATALOG="${_catalog}"
    RIVE_QT_VIEWER_SAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples/viewer/samples")

# One process per fixture. The same catalog powers the viewer and these
# regression cases. Persistent offscreen capture uses Qt 6.6's QRhi API.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_catalog}")
file(READ "${_catalog}" _json)
string(JSON _count LENGTH "${_json}" samples)
math(EXPR _last "${_count} - 1")
foreach(_index RANGE 0 ${_last})
    string(JSON _id GET "${_json}" samples ${_index} id)
    add_test(NAME UpgradeSamples.import.${_id}
        COMMAND tst_runtime_upgrade importsAndAdvances:${_id})
    set_tests_properties(UpgradeSamples.import.${_id} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "runtime-upgrade")
    if(APPLE AND Qt6_VERSION VERSION_GREATER_EQUAL 6.6)
        add_test(NAME UpgradeSamples.render.${_id}
            COMMAND tst_runtime_upgrade rendersSample:${_id})
        set_tests_properties(UpgradeSamples.render.${_id} PROPERTIES
            LABELS "runtime-upgrade;gpu" TIMEOUT 30)
    endif()
endforeach()

add_test(NAME UpgradeSamples.fittedTextUsesFittedHeight
    COMMAND tst_runtime_upgrade fittedTextUsesFittedHeight)
set_tests_properties(UpgradeSamples.fittedTextUsesFittedHeight PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "runtime-upgrade")

if(WIN32)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    get_property(_tests DIRECTORY PROPERTY TESTS)
    string(REPLACE ";" "\\;" _test_path "${_qt_bin};$ENV{PATH}")
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT "PATH=${_test_path}")
endif()
