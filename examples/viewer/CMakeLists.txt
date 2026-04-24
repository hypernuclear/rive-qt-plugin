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
)

# Hand the absolute samples directory to the QML runtime so Main.qml can
# enumerate .riv files on disk without a hand-maintained list. Used by
# Main.qml's Qt.labs.folderlistmodel.
target_compile_definitions(rive_viewer PRIVATE
    RIVE_VIEWER_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples"
)
