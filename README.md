# rive-qt-plugin

A Qt/QML integration for [Rive](https://rive.app) runtime animations, GPU-rendered through Qt's scene graph.

Exposes `RiveView` as a QML element that loads a `.riv` file, advances its state machine off the scene graph's frame clock, and renders the artboard with Rive's own GPU renderer into a `QSGTexture` that composites zero-copy into the Qt Quick scene. `RiveView` is a `QQuickItem`; the actual drawing happens in a per-RHI render backend (Metal, D3D11/D3D12, OpenGL, or Vulkan) picked to match the window's `QRhi`.

## Status

Early spike. Functional enough to render most `.riv` files upstream ships as demos, but not yet production ready. See [Known limitations](#known-limitations--todo).

## Requirements

- Qt 6.5+ (6.12 tested)
- CMake 3.21+
- A C++20 compiler (Xcode clang / MSVC 2022 / recent GCC)
- `make` (macOS/Linux) or `msbuild` (Windows) for rive's premake-driven build
- `git` — the build bootstraps `premake5` from source on first configure

## Build (standalone viewer)

The repo ships a `CMakePresets.json` that pins the generator to Ninja. In VS Code, open this repository folder and run **CMake: Select Configure Preset**. Use a local preset in the ignored `CMakeUserPresets.json` that inherits `default` and supplies your Qt installation through `CMAKE_PREFIX_PATH`.

Run **CMake: Configure**, then **CMake: Set Launch/Debug Target** → `rive_viewer`. **CMake: Run Without Debugging** builds and launches it. **F5** uses the supplied C++/QML launch configuration; the **Build Viewer** task also follows the selected preset.

From the CLI you also need to point CMake at your Qt install:

```sh
git clone --recursive git@github.com:hypernuclear/rive-qt-plugin.git
cd rive-qt-plugin
cmake --preset default -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12.0/macos
cmake --build --preset viewer
./build/examples/viewer/rive_viewer
```

First configure will spend 30–60s bootstrapping `premake5` and building rive-runtime. Subsequent builds are fast.

On macOS, Xcode must include the Metal compiler. If the shader build reports a missing Metal Toolchain, install it with `xcodebuild -downloadComponent MetalToolchain` and rebuild.

The viewer's **Update checks** picker includes seven samples for the current
runtime update, with instructions for visual inspection. See the
[sample guide](examples/viewer/samples/README.md) for coverage and automated checks.

## Use in your own Qt project

Add as a submodule:

```sh
git submodule add git@github.com:hypernuclear/rive-qt-plugin.git third_party/rive-qt-plugin
git submodule update --init --recursive
```

In your `CMakeLists.txt`:

```cmake
add_subdirectory(third_party/rive-qt-plugin)
target_link_libraries(my_app PRIVATE rive_qt::rive_qt)
```

In QML:

```qml
import Hypernuclear.Rive

RiveView {
    anchors.fill: parent
    source: "qrc:/path/to/animation.riv"
    fit: RiveView.Fit.Contain
}
```

## QML API

```qml
RiveView {
    // qrc://, file:// or http(s):// URL to a .riv file.
    source: url

    // Optional: pick a non-default artboard / state machine by name.
    artboard: "Main"
    stateMachineName: "State Machine 1"

    // How to fit the artboard inside the item's bounds. Mirrors rive::Fit.
    // Options: Fill, Contain (default), Cover, FitWidth, FitHeight, None,
    // ScaleDown, Layout. Pair Fit.Layout with `layoutSize` for responsive
    // artwork authored with Rive's layout system.
    fit: RiveView.Fit.Contain
    alignment: RiveView.Alignment.Center

    // Play/pause and playback rate (1.0 = real time, 0 = freeze).
    playing: true
    speed: 1.0

    // Mouse/keyboard/touch are forwarded to the state machine by default.
    // Set false to drive pointer input manually via stateMachine.pointer*().
    inputForwarding: true

    // Data binding: read/write the artboard's view-model properties.
    onViewModelChanged: {
        if (viewModel)
            viewModel.number("score").value = 42
    }

    // Emitted when a .riv fails to load or parse.
    onLoadFailed: reason => { /* ... */ }
}
```

## Architecture

| Piece                      | Role                                                                       |
|----------------------------|----------------------------------------------------------------------------|
| `RiveView`                 | `QQuickItem` that owns a `rive::File`, drives the advance, and renders into a `QSGTexture`. |
| `RiveQtFactory`            | `rive::Factory` impl — mints the GPU render context's paints, paths, images. |
| `RiveRenderBackend`        | Per-RHI backend interface; one impl per `src/backends/<api>/` (Metal, D3D11, D3D12, GL, Vulkan). Owns the graphics-device interop and paints the artboard into a `QSGTexture`. |
| `RiveStateMachine` / `RiveViewModelInstance` | QObject wrappers exposing the state machine and data-bound view model to QML. |
| `cmake/BuildRive.cmake`    | Invokes upstream's premake/gmake2/msbuild build and imports `librive.a`.   |
| `third_party/rive-runtime` | Upstream submodule, pinned to `runtime-v0.1.592` (`47d1cd54ccef`).          |

### Why Rive's GPU renderer

Rather than reimplement Rive's path/feather/clip semantics on top of `QPainter`, the plugin drives Rive's own GPU render context (the same one its native players use) and hands Qt the resulting texture. `RiveView` resolves the window's `QRhi`, builds the matching backend, renders the artboard into a `QSGTexture`, and composites it through a `QSGSimpleTextureNode` — no CPU rasterization, no per-frame texture upload.

| Platform        | Backend(s)                                  |
|-----------------|---------------------------------------------|
| macOS / iOS     | Metal                                       |
| Windows         | D3D11, D3D12                                |
| Linux / other   | OpenGL, Vulkan                              |

Upstream also offers WebGPU (Dawn/wagyu), but [Qt 6.11's scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html) has no WebGPU backend. Supporting it here requires a separate device/texture interop path. Upstream's RHI adapter targets Unreal Engine.

Vulkan is opt-in everywhere (`RIVE_QT_WITH_VULKAN`, on by default) and auto-disables when the host Qt was built without Vulkan support. See the options block at the top of `CMakeLists.txt` for the full backend/audio/scripting matrix.

### Quirk: `-fno-rtti`

Rive is built with RTTI off (see `rive_build_config.lua` — "nonstandard for Rive"). Every plugin TU that subclasses or otherwise touches `rive::` types — the `src/rive/` wrappers, `RiveView`, and the render backends — matches that with a per-file `-fno-rtti` / `/GR-`. Without it the linker demands typeinfo symbols rive never emits. The flag is scoped per source file (see `set_source_files_properties` in `CMakeLists.txt`), so host projects that consume this plugin are unaffected.

## Known limitations / TODO

- **GPU canvas**: all five backend adapters use Rive's deferred recorder and inline replay. The viewer includes a shader triangle check, with backend-specific render tests for colors, orientation, resize, reload, and window recreation. See [sample checks](examples/viewer/samples/README.md) for commands and validation limits.

- **Idle repaints**: mitigated — the advance return value gates the repaint loop, so an artboard that reaches a settled/static state stops requesting frames instead of spinning the render loop.
- **Hosted/referenced asset fetches still block**: the `.riv` *source* itself is now fetched off the render thread (`http(s)://` asynchronously on the GUI thread, `qrc`/`file` read inline before paint), so a remote source no longer stalls the UI. But *hosted/referenced assets* (images/fonts/audio not embedded in the `.riv`) are still resolved synchronously by the asset loader during import on the render thread — a `.riv` that pulls large CDN assets can still hitch on first load. Embed assets, or keep them small, until async asset loading lands.
- **Multi-window `raw()` access**: `RiveFile` is cached by URL and shared across views; its instancing/enumeration is mutex-guarded, so two `RiveView`s in different windows (different render threads) can now mint artboards/view-models from the same file safely. The one unguarded path is `RiveFile::raw()` — the bare `rive::File*` used by view-model property wrappers to resolve assets — so sharing a single `.riv`'s *view-model* across windows is still not safe.
- **Text**: links against rive's bundled harfbuzz + sheenbidi; works for most cases. Complex shaping not extensively tested. A process-wide font fallback (`RiveView.setFallbackFontPath`) covers unresolved font assets.
- **Distribution**: consumed today via `add_subdirectory` (source). There are no `install()`/export rules yet, so the module isn't packaged as a findable binary `find_package` component.

### Implemented since the early spike

State-machine driving, **data-bound view models** (`RiveViewModelInstance` + typed `RiveVM*Property` wrappers), **pointer/keyboard input forwarding** (on by default, `inputForwarding`), responsive **layout** (`Fit.Layout` + `layoutSize`), playback `speed`, and `alignment` are all wired. Note Rive deprecated legacy SM inputs (boolean/number/trigger) and runtime events in favor of data binding — drive interactivity through the bound view model's properties, not through SM inputs.

## License

MIT. See `LICENSE`.

Rive runtime (`third_party/rive-runtime`) is separately MIT-licensed; see its own LICENSE.

## Credits

- [Rive](https://rive.app) for the runtime.
- Sample `.riv` files in `examples/viewer/samples/` are copied from `rive-app/rive-runtime/renderer/webgpu_player/rivs/`. For production use, bundle `.riv` files you have rights to.
