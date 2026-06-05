# rive-qt-plugin

A Qt/QML integration for [Rive](https://rive.app) runtime animations, GPU-rendered through Qt's scene graph.

Exposes `RiveView` as a QML element that loads a `.riv` file, advances its state machine off the scene graph's frame clock, and renders the artboard with Rive's own GPU renderer into a `QSGTexture` that composites zero-copy into the Qt Quick scene. `RiveView` is a `QQuickItem`; the actual drawing happens in a per-RHI render backend (Metal, D3D11/D3D12, OpenGL, or Vulkan) picked to match the window's `QRhi`.

## Status

Early spike. Functional enough to render most `.riv` files upstream ships as demos, but not yet production ready. See [Known limitations](#known-limitations--todo).

## Requirements

- Qt 6.5+ (6.10 tested)
- CMake 3.21+
- A C++20 compiler (Xcode clang / MSVC 2022 / recent GCC)
- `make` (macOS/Linux) or `msbuild` (Windows) for rive's premake-driven build
- `git` — the build bootstraps `premake5` from source on first configure

## Build (standalone viewer)

The repo ships a `CMakePresets.json` that pins the generator to Ninja (avoids the NMake+JOM trip-up on Windows kits). In Qt Creator / VS Code CMake Tools, just select a Qt kit and the preset is picked up automatically — Qt comes from the kit's toolchain file.

From the CLI you also need to point CMake at your Qt install:

```sh
git clone --recursive git@github.com:hypernuclear/rive-qt-plugin.git
cd rive-qt-plugin
cmake --preset default -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.2/macos
cmake --build --preset viewer
./build/examples/viewer/rive_viewer
```

First configure will spend 30–60s bootstrapping `premake5` and building rive-runtime. Subsequent builds are fast.

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
| `third_party/rive-runtime` | Upstream submodule, pinned to `runtime-v0.1.5`.                            |

### Why Rive's GPU renderer

Rather than reimplement Rive's path/feather/clip semantics on top of `QPainter`, the plugin drives Rive's own GPU render context (the same one its native players use) and hands Qt the resulting texture. `RiveView` resolves the window's `QRhi`, builds the matching backend, renders the artboard into a `QSGTexture`, and composites it through a `QSGSimpleTextureNode` — no CPU rasterization, no per-frame texture upload.

| Platform        | Backend(s)                                  |
|-----------------|---------------------------------------------|
| macOS / iOS     | Metal                                       |
| Windows         | D3D11, D3D12                                |
| Linux / other   | OpenGL, Vulkan                              |

Vulkan is opt-in everywhere (`RIVE_QT_WITH_VULKAN`, on by default) and auto-disables when the host Qt was built without Vulkan support. See the options block at the top of `CMakeLists.txt` for the full backend/audio/scripting matrix.

### Quirk: `-fno-rtti`

Rive is built with RTTI off (see `rive_build_config.lua` — "nonstandard for Rive"). Every plugin TU that subclasses or otherwise touches `rive::` types — the `src/rive/` wrappers, `RiveView`, and the render backends — matches that with a per-file `-fno-rtti` / `/GR-`. Without it the linker demands typeinfo symbols rive never emits. The flag is scoped per source file (see `set_source_files_properties` in `CMakeLists.txt`), so host projects that consume this plugin are unaffected.

## Known limitations / TODO

- **Idle repaints**: mitigated — the advance return value gates the repaint loop, so an artboard that reaches a settled/static state stops requesting frames instead of spinning the render loop.
- **Network loading is synchronous**: an `http(s)://` source is fetched on the render thread via a blocking `QEventLoop` inside the paint path, which stalls the UI for the duration of the download. For remote `.riv` files, pre-fetch the bytes host-side and feed them in (a `fromBytes()` entry point is the planned fix). `qrc://` and `file://` sources load fine.
- **Multi-window sharing**: `RiveFile` instances are cached by URL and shared across views. Two `RiveView`s in *different windows* (hence different render threads) minting artboards/view-models from the same cached file is not yet guarded — keep a given `.riv` to one window, or load distinct URLs, until per-file locking lands.
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
