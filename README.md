# rive-qt-plugin

A Qt/QML integration for [Rive](https://rive.app) runtime animations, rendered via `QPainter`.

Exposes `RiveView` as a QML element that loads a `.riv` file, advances its state machine off the scene graph's frame clock, and paints into a `QQuickPaintedItem`. No Skia, no GPU renderer — just rive-cpp core + an adapter layer that translates rive's render commands into `QPainter` calls.

## Status

Early spike. Functional enough to render most `.riv` files upstream ships as demos, but not yet production ready. See [Known limitations](#known-limitations--todo).

## Requirements

- Qt 6.5+ (6.10 tested)
- CMake 3.21+
- A C++20 compiler (Xcode clang / MSVC 2022 / recent GCC)
- `make` (macOS/Linux) or `msbuild` (Windows) for rive's premake-driven build
- `git` — the build bootstraps `premake5` from source on first configure

## Build (standalone viewer)

```sh
git clone --recursive git@github.com:hypernuclear/rive-qt-plugin.git
cd rive-qt-plugin
cmake -B build -GNinja
cmake --build build
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
    // qrc:// or file:// URL to a .riv file.
    source: url

    // How to fit the artboard inside the item's bounds. Mirrors rive::Fit.
    // Options: Contain (default), Cover, Fill, None, ScaleDown.
    fit: RiveView.Fit.Contain

    // Play/pause.
    playing: true

    // Emitted when a .riv fails to load or parse.
    onLoadFailed: reason => { /* ... */ }
}
```

## Architecture

| Piece                      | Role                                                                       |
|----------------------------|----------------------------------------------------------------------------|
| `RivePainterFactory`       | `rive::Factory` impl — creates paints, paths, gradients, images.           |
| `RivePainterRenderer`      | `rive::Renderer` impl — drives a `QPainter` through rive's commands.       |
| `RiveView`                 | `QQuickPaintedItem` that owns a `rive::File` and drives the advance.       |
| `cmake/BuildRive.cmake`    | Invokes upstream's premake/gmake2/msbuild build and imports `librive.a`.   |
| `third_party/rive-runtime` | Upstream submodule, pinned to `runtime-v0.1.5`.                            |

### Why QPainter (for now)

Getting something on screen without a parallel universe of Metal/D3D/Vulkan glue. Rive's render interface maps cleanly onto `QPainter`:

- `rive::RenderPath` → `QPainterPath`
- `rive::RenderPaint` → `QPen`/`QBrush` + composition mode
- `rive::RenderShader` → `QLinearGradient`/`QRadialGradient`
- `rive::RenderImage` → `QImage`
- `save` / `restore` / `transform` / `clipPath` → `QPainter::save`/`restore`/`setWorldTransform`/`setClipPath`

### Quirk: `-fno-rtti`

Rive is built with RTTI off (see `rive_build_config.lua` — "nonstandard for Rive"). Our C++ TUs that subclass `rive::RenderPath`/`RenderPaint`/... match that with a per-file `-fno-rtti` / `/GR-`. Without it the linker demands typeinfo symbols that rive never emits. Host projects that consume this plugin are unaffected — the flag is scoped to the three plugin source files.

## Known limitations / TODO

- **Idle CPU**: partly mitigated — `advanceAndApply`'s return value now gates the repaint loop so static final states don't burn CPU. Looping animations still cost CPU each frame because we rasterize via `QPainter`. The real fix is the GPU renderer path below.
- **Playing-animation CPU**: 100–150% CPU for a complex artboard is normal given CPU rasterization. Needs a follow-up that uses rive's GPU renderer (Metal on macOS, D3D11 on Windows, Vulkan elsewhere) into a `QSGTexture` for zero-copy compositing.
- **Feathering**: the `feather()` paint setter is not wired — feathered strokes fall back to the unfeathered shape. Sample that exposes it: `feathering-demo-tape-vst.riv`.
- **State-machine inputs**: no QML API yet for reading/writing triggers, booleans, numbers. Planned.
- **View-model binding**: not wired.
- **Pointer events**: no forwarding from Qt mouse events to rive's state-machine pointer inputs.
- **`drawImageMesh`**: stubbed. `.riv` files using skinned / warped images will have those regions render blank. Rare in practice.
- **Text**: links against rive's bundled harfbuzz + sheenbidi; works for most cases. Complex shaping not extensively tested.

## License

MIT. See `LICENSE`.

Rive runtime (`third_party/rive-runtime`) is separately MIT-licensed; see its own LICENSE.

## Credits

- [Rive](https://rive.app) for the runtime.
- Sample `.riv` files in `examples/viewer/samples/` are copied from `rive-app/rive-runtime/renderer/webgpu_player/rivs/`. For production use, bundle `.riv` files you have rights to.
