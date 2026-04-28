# rive-asset-gen — build-time tool that walks .riv metadata and emits
# C++ + QML constants. Used by the qt_add_rive_assets() CMake helper.
#
# The tool links against the rive runtime + the runtime's NoOpFactory
# so it can import .riv files without a real graphics device. Doesn't
# need Qt — it's a plain C++ executable invoked by the build system.

add_executable(rive-asset-gen
    main.cpp
    "${RIVE_RUNTIME_DIR}/utils/no_op_factory.cpp"
)

target_include_directories(rive-asset-gen PRIVATE
    "${RIVE_RUNTIME_DIR}/include"
    "${RIVE_RUNTIME_DIR}/renderer/include"
)

# Match Rive's RTTI-off ABI (consistent with the rest of the project).
set_source_files_properties(
    main.cpp
    "${RIVE_RUNTIME_DIR}/utils/no_op_factory.cpp"
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>"
)

target_link_libraries(rive-asset-gen PRIVATE rive)

# Trim windows.h surface for any TU that picks it up transitively.
if(WIN32)
    target_compile_definitions(rive-asset-gen PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# Expose the executable target name for qt_add_rive_assets() to depend on.
set_target_properties(rive-asset-gen PROPERTIES OUTPUT_NAME rive-asset-gen)
