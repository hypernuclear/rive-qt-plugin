qt_add_executable(tst_runtime_upgrade
    tst_runtime_upgrade.cpp
    "${RIVE_RUNTIME_DIR}/utils/no_op_factory.cpp")
target_link_libraries(tst_runtime_upgrade PRIVATE
    Qt6::Test Qt6::Quick rive_qt rive riveqt_test_helpers)
target_link_libraries(tst_runtime_upgrade PRIVATE Qt6::GuiPrivate)
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
set(_gpu_backends)
if(APPLE)
    list(APPEND _gpu_backends metal)
elseif(WIN32)
    list(APPEND _gpu_backends d3d11 opengl)
    if(RIVE_QT_WITH_D3D12)
        list(APPEND _gpu_backends d3d12)
    endif()
else()
    list(APPEND _gpu_backends opengl)
endif()
if(RIVE_QT_WITH_VULKAN)
    list(APPEND _gpu_backends vulkan)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_catalog}")
file(READ "${_catalog}" _json)
string(JSON _count LENGTH "${_json}" samples)
math(EXPR _last "${_count} - 1")
foreach(_index RANGE 0 ${_last})
    string(JSON _id GET "${_json}" samples ${_index} id)
    string(JSON _requires_gpu ERROR_VARIABLE _missing GET "${_json}" samples ${_index} requiresGpu)
    if(NOT _requires_gpu)
        add_test(NAME UpgradeSamples.import.${_id}
            COMMAND tst_runtime_upgrade importsAndAdvances:${_id})
        set_tests_properties(UpgradeSamples.import.${_id} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "runtime-upgrade")
    endif()
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.6)
        foreach(_backend IN LISTS _gpu_backends)
            add_test(NAME UpgradeSamples.render.${_backend}.${_id}
                COMMAND tst_runtime_upgrade rendersSample:${_id})
            set_tests_properties(UpgradeSamples.render.${_backend}.${_id} PROPERTIES
                ENVIRONMENT "RIVE_QT_TEST_BACKEND=${_backend}"
                LABELS "runtime-upgrade;gpu;${_backend}" TIMEOUT 60
                FAIL_REGULAR_EXPRESSION "requires the deferred recorder;rive deferred:.*([Dd][Rr][Oo][Pp]|TRIPWIRE);Validation Error;D3D12 ERROR;D3D11 ERROR")
        endforeach()
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
