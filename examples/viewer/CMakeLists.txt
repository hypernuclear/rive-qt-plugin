# Standalone viewer — loads any .riv from the samples/ directory and drives
# it through the plugin's RiveView. Built when the plugin is the top-level
# project (RIVE_QT_BUILD_EXAMPLES=ON by default in that case).

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)

qt_add_executable(rive_viewer
    main.cpp
)

# Glob the samples so adding/removing .riv files doesn't need a manual
# edit. CONFIGURE_DEPENDS re-runs the glob at build time.
file(GLOB RIVE_SAMPLE_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/*.riv")

qt_add_qml_module(rive_viewer
    URI RiveViewer
    VERSION 0.1
    QML_FILES
        Main.qml
    RESOURCES
        ${RIVE_SAMPLE_FILES}
)

target_link_libraries(rive_viewer
    PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
        rive_qt::rive_qt
        # The static QML module `Hypernuclear.Rive` ships both a backing
        # library (rive_qt) and a plugin target (rive_qtplugin) that holds
        # the QML type registrations. Link both — without the plugin,
        # `import Hypernuclear.Rive` fails at runtime with
        # "plugin rive_qtplugin not found".
        rive_qtplugin
)

# Hand the absolute samples directory to the QML runtime so Main.qml can
# enumerate .riv files on disk without a hand-maintained list. Used by
# Main.qml's Qt.labs.folderlistmodel.
target_compile_definitions(rive_viewer PRIVATE
    RIVE_VIEWER_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples"
)

# Smoke-test the qt_add_rive_assets() codegen helper against one of the
# bundled samples. Generates RiveViewer::Assets::DataBindingTest::*
# constants in C++ and a DataBindingTest singleton in QML under
# `RiveViewer.Assets`. Currently unused by Main.qml — present mainly to
# verify the helper builds cleanly end-to-end.
qt_add_rive_assets(rive_viewer
    NAMESPACE RiveViewer::Assets
    QML_URI RiveViewer.Assets
    ASSETS
        samples/data_binding_test.riv
)
