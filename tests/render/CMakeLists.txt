# Layer 3 — offscreen Metal render smoke (APPLE only; the only backend the
# plugin builds on Apple). Links the real wrapper lib (which pulls in the
# Metal backend) + Qt Quick/GuiPrivate for QQuickWindow + QRhi.
qt_add_executable(tst_render_smoke
    tst_render_smoke.cpp
)
target_link_libraries(tst_render_smoke PRIVATE
    Qt6::Test Qt6::Quick Qt6::Gui Qt6::GuiPrivate
    rive_qt rive riveqt_test_helpers)
target_include_directories(tst_render_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/rive
    ${CMAKE_SOURCE_DIR}/src/backends)
target_compile_features(tst_render_smoke PRIVATE cxx_std_20)
# rive_view.h pulls in rive types → match Rive's RTTI-off ABI.
set_source_files_properties(tst_render_smoke.cpp
    TARGET_DIRECTORY tst_render_smoke
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")

add_test(NAME Render.animationDurationWithoutSourceIsZero COMMAND tst_render_smoke animationDurationWithoutSourceIsZero)
add_test(NAME Render.decodeImageUploadsTexture COMMAND tst_render_smoke decodeImageUploadsTexture)
add_test(NAME Render.rendersNonBlankFrame COMMAND tst_render_smoke rendersNonBlankFrame)
add_test(NAME Render.matchesGoldenSnapshot COMMAND tst_render_smoke matchesGoldenSnapshot)
qt_add_executable(tst_color_probe tst_color_probe.cpp)
target_link_libraries(tst_color_probe PRIVATE
    Qt6::Test Qt6::Quick Qt6::Gui Qt6::GuiPrivate
    rive_qt rive riveqt_test_helpers)
target_include_directories(tst_color_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/rive
    ${CMAKE_SOURCE_DIR}/src/backends)
target_compile_features(tst_color_probe PRIVATE cxx_std_20)
set_source_files_properties(tst_color_probe.cpp
    TARGET_DIRECTORY tst_color_probe
    PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")
