# RiveAssets.cmake — qt_add_rive_assets() helper for build-time codegen
# of typed access constants for .riv asset metadata.
#
# Usage:
#
#   qt_add_rive_assets(my_app
#       NAMESPACE MyApp::Assets
#       ASSETS
#           ui/menu.riv
#           ui/checkout.riv
#   )
#
# For each asset, runs rive-asset-gen at build time to produce a header
# under <build_dir>/rive_assets/<basename>_assets.h. The header defines
# `inline constexpr std::string_view` constants under the requested
# namespace (one per artboard, state machine, view-model property,
# trigger, instance, text run) so misnamed string lookups become
# compile errors instead of silent runtime no-ops.
#
# To also emit a QML singleton, pass QML_URI:
#
#   qt_add_rive_assets(my_app
#       NAMESPACE MyApp::Assets
#       QML_URI MyApp.RiveAssets
#       ASSETS ui/menu.riv
#   )
#
# Each .riv file produces a `<Basename>.qml` singleton registered under
# QML_URI; QML code does:
#
#   import MyApp.RiveAssets 1.0
#   ...
#   sm.getTrigger(Menu.ViewModels.PersonViewModel.Triggers.onFormSubmit).fire()
#
# The QML singletons are added to a separate qt_add_qml_module so they
# don't pollute the consuming target's QML module.

function(qt_add_rive_assets target)
    set(options "")
    set(oneValueArgs NAMESPACE QML_URI)
    set(multiValueArgs ASSETS)
    cmake_parse_arguments(_RA "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT TARGET rive-asset-gen)
        message(FATAL_ERROR "qt_add_rive_assets: rive-asset-gen target is not "
                            "available. Add the rive_qt subdirectory before "
                            "calling this function.")
    endif()
    if(NOT _RA_NAMESPACE)
        message(FATAL_ERROR "qt_add_rive_assets: NAMESPACE is required")
    endif()
    if(NOT _RA_ASSETS)
        message(FATAL_ERROR "qt_add_rive_assets: ASSETS list is empty")
    endif()

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/rive_assets")
    file(MAKE_DIRECTORY "${_gen_dir}")

    set(_gen_headers "")
    set(_gen_qmls "")

    foreach(_asset ${_RA_ASSETS})
        if(IS_ABSOLUTE "${_asset}")
            set(_asset_abs "${_asset}")
        else()
            set(_asset_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_asset}")
        endif()
        get_filename_component(_basename "${_asset}" NAME_WE)

        # Per-asset namespace: append the basename onto the user-supplied
        # root, capitalized to look like a namespace component. Sanitize
        # to a valid C++ identifier (the generator does the same so the
        # naming stays consistent).
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _ns_suffix "${_basename}")
        if(_ns_suffix MATCHES "^[0-9]")
            set(_ns_suffix "_${_ns_suffix}")
        endif()
        # Capitalize first letter so e.g. "menu" → "Menu" matches the
        # convention readers expect for namespaces.
        string(SUBSTRING "${_ns_suffix}" 0 1 _ns_first)
        string(SUBSTRING "${_ns_suffix}" 1 -1 _ns_rest)
        string(TOUPPER "${_ns_first}" _ns_first)
        set(_full_ns "${_RA_NAMESPACE}::${_ns_first}${_ns_rest}")
        set(_singleton_name "${_ns_first}${_ns_rest}")

        set(_out_h "${_gen_dir}/${_basename}_assets.h")
        set(_out_qml "${_gen_dir}/${_singleton_name}.qml")

        set(_gen_args
            --input "${_asset_abs}"
            --output-cpp "${_out_h}"
            --namespace "${_full_ns}"
        )
        set(_outs "${_out_h}")
        if(_RA_QML_URI)
            list(APPEND _gen_args
                --output-qml "${_out_qml}"
                --qml-singleton "${_singleton_name}")
            list(APPEND _outs "${_out_qml}")
            list(APPEND _gen_qmls "${_out_qml}")
        endif()

        add_custom_command(
            OUTPUT ${_outs}
            COMMAND $<TARGET_FILE:rive-asset-gen> ${_gen_args}
            DEPENDS rive-asset-gen "${_asset_abs}"
            COMMENT "rive-asset-gen: ${_basename}"
            VERBATIM
        )
        list(APPEND _gen_headers "${_out_h}")
    endforeach()

    # Headers get added to the consuming target. The custom command
    # binds via OUTPUT, but target_sources is what makes CMake schedule
    # the generation step relative to the target's compile order.
    target_sources(${target} PRIVATE ${_gen_headers})
    target_include_directories(${target} PRIVATE "${_gen_dir}")

    # Group the QML singletons under their own qt_add_qml_module so the
    # consuming target's existing QML module isn't polluted with names
    # we didn't ask it to expose. Module name derived from the URI.
    if(_RA_QML_URI AND _gen_qmls)
        string(REPLACE "." "_" _qml_target "${target}_rive_assets_${_RA_QML_URI}")
        # Mark each generated QML file as a singleton (so qt_add_qml_module
        # emits the qmldir singleton entry) and set its resource alias to
        # the bare filename (qt_add_qml_module rejects absolute paths
        # without an alias).
        foreach(_qml ${_gen_qmls})
            get_filename_component(_qml_basename "${_qml}" NAME)
            set_source_files_properties("${_qml}" PROPERTIES
                QT_QML_SINGLETON_TYPE TRUE
                QT_RESOURCE_ALIAS "${_qml_basename}"
                GENERATED TRUE)
        endforeach()
        qt_add_qml_module(${_qml_target}
            URI "${_RA_QML_URI}"
            VERSION 1.0
            STATIC
            QML_FILES ${_gen_qmls}
        )
        # Make the consumer link the generated QML module so its
        # singletons get registered in the QML engine. STATIC modules
        # need an explicit link from any target that imports them.
        target_link_libraries(${target} PRIVATE ${_qml_target}plugin)
    endif()
endfunction()
