# Layer 1 — pure unit tests. No rive instance / GPU; compiles the helper
# source directly and links `rive` only for its headers + ABI defines.

qt_add_executable(tst_key_map
    tst_key_map.cpp
    ${CMAKE_SOURCE_DIR}/src/rive/rive_key_map.cpp
)
target_link_libraries(tst_key_map PRIVATE Qt6::Test Qt6::Core rive)
target_include_directories(tst_key_map PRIVATE
    ${CMAKE_SOURCE_DIR}/src/rive)
target_compile_features(tst_key_map PRIVATE cxx_std_20)
# rive_key_map.cpp + the test TU include rive headers → match RTTI-off ABI.
set_source_files_properties(
    tst_key_map.cpp
    ${CMAKE_SOURCE_DIR}/src/rive/rive_key_map.cpp
    TARGET_DIRECTORY tst_key_map
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")

foreach(slot
    asciiLettersMapOneToOne
    digitsMapOneToOne
    functionKeysMap
    namedAndNavigationKeysMap
    unmappedKeysReportInvalid
    modifiersAreBitwiseOred)
    add_test(NAME KeyMap.${slot} COMMAND tst_key_map ${slot})
endforeach()

if(WIN32)
    cmake_path(GET Qt6_DIR PARENT_PATH _qt_cmake_dir)
    cmake_path(GET _qt_cmake_dir PARENT_PATH _qt_lib_dir)
    cmake_path(GET _qt_lib_dir PARENT_PATH _qt_prefix)
    get_property(_all_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_all_tests} PROPERTIES
        ENVIRONMENT "PATH=${_qt_prefix}/bin;$ENV{PATH}")
endif()
