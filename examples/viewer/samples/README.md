# Runtime update checks

The viewer's **Update checks** picker loads six fixtures from Rive
`runtime-v0.1.592` (`47d1cd54ccefd468db7cb26ecc4699ae1723dd85`) and the
project's `gpu_canvas.riv` shader triangle. Selecting a
check restores playback at 1×, Contain fit, the sample's artboard/view-model
preset, auto-binding, and no layout-size override. Instructions appear above the
viewer. The ordinary Sample and Artboard pickers remain available.

| Sample | What to check |
| --- | --- |
| `gpu_canvas.riv` | Expect a red top, green bottom-left, and blue bottom-right triangle on a dark square. This sample is static. Resize the viewer, reload, and reopen it. |
| `upgrade-grid-stack.riv` | Compare all six artboards. Inspect grid/stack alignment, animated track sizing/spans, and shapes participating directly in layout. |
| `upgrade-fitted-text.riv` | A file exported as 7.4. The bar should follow the fitted title without the old excessive gap. Try different widths in the Layout tab. |
| `upgrade-global-scripting.riv` | Parent shows “Initial value” and the list “Sizes”, “Colors”, “Wrong”. The last label is authored by the test script. Compare Main and GlobalName; keep Auto-bind VM enabled. |
| `upgrade-scripted-clipping.riv` | The clip follows the animated path effect continuously. Watch for a frozen clip or content outside the boundary. |
| `upgrade-image-fit.riv` | Embedded raster art stays visible and preserves authored scale during fit/alignment changes. Try replacing `imageProperty` through the View Model panel. |
| `upgrade-artboard-transforms.riv` | On Main, click the transformed shape. Try `rotation`, `xPos`, `yPos`, and `triggerProperty`; drawing and hit testing should remain aligned. |

The scripting checks use the default build options
`RIVE_QT_WITH_SCRIPTING=ON` and `RIVE_QT_WITH_RIVE_TOOLS=ON`. They are not
signed-production-script validation. Global view models work inside the
file/scripts; the Qt API does not yet expose explicit global VM editing.

## GPU canvas

All five backend adapters record imports, 2D drawing, and scripted GPU passes
through Rive's `DeferredSession`, then replay on the scene-graph render thread.
The recording factory routes scripts to the deferred Ore context and canvas
host; QImage decodes raster assets on the real device.

Choose **GPU canvas triangle** in the viewer's **Update checks** picker.
`gpu_canvas.riv` contains `GPUCanvasExample` and `GPUTriangleShader`.
The automated check requires all three triangle colors in the expected
orientation, so a canvas background with a failing script cannot pass. It also
checks paused display, changes to the Qt view's size, repeated file reloads,
and closing/recreating the window and GPU context.

Select a backend before launching the viewer:

| Platform | `QSG_RHI_BACKEND` values |
| --- | --- |
| macOS | `metal` (or `vulkan` with a Vulkan-enabled Qt and MoltenVK) |
| Windows | `d3d11`, `d3d12`, `opengl`, `vulkan` |
| Linux | `opengl`, `vulkan` |

For example, in a POSIX shell:

```sh
QSG_RHI_BACKEND=vulkan ./build/examples/viewer/rive_viewer
```

Or in PowerShell:

```powershell
$env:QSG_RHI_BACKEND = "d3d12"
.\build\examples\viewer\Release\rive_viewer.exe
```

Vulkan and D3D12 must be enabled at build time. The Vulkan backend automatically
disables if Qt lacks Vulkan support. WebGPU is an additional upstream Rive
backend; Qt 6.11 does not expose it through the scene graph. Rive's RHI backend
is an Unreal Engine adapter.

D3D12 and Vulkan GPU canvas submissions wait for the previous submission before
reusing Ore's descriptor storage. Ordinary 2D rendering retains the command
buffer ring. Metal uses its existing queue and buffer completion tracking.

This static triangle does not test animated shader uniforms, script-driven
`GPUCanvas:resize()`, transparency, or nested canvas passes. Further fixtures
are needed for those cases and for cache-as-bitmap.

## Automated checks

Build with Qt 6.6 or newer for the GPU checks, then run:

```sh
ctest --test-dir build -L runtime-upgrade --output-on-failure
```

CTest registers a render case for each fixture and compiled backend. Each case
explicitly selects its API with `RIVE_QT_TEST_BACKEND`; initialization failure,
import errors, dropped deferred draws, and blank output fail the test.
To run the triangle across all compiled backends:

```sh
ctest --test-dir build -R '^UpgradeSamples.render.*gpu-canvas$' --output-on-failure
```

For one backend, use `-R '^UpgradeSamples.render.vulkan.'` (substitute `metal`,
`d3d11`, `d3d12`, or `opengl`). On macOS, enable Metal API validation with
`MTL_DEBUG_LAYER=1`. On Windows, add `--build-config Release` when using Visual
Studio. Machines without a graphics implementation can run only the headless
cases with `ctest --test-dir build -LE gpu`.

The six upstream fixtures' artboards are imported, bound, and advanced in the
headless suite. The fitted-text test checks the 7.4 hug height; the global
scripting test verifies that Luau enumerates shared view models. Render tests
also require the scripted clip to keep changing over multiple frames. The
same `../upgrade-samples.json` catalog drives the viewer and tests.

Save inspection frames, grouped by backend:

```sh
RIVE_QT_SAMPLE_SNAPSHOT_DIR=/tmp/rive-upgrade-frames \
  ctest --test-dir build -R '^UpgradeSamples.render\.' --output-on-failure
```

CI covers Metal on macOS, D3D11/D3D12 through Windows WARP, and OpenGL/Vulkan
through Mesa on Linux. Software drivers exercise those APIs but do not replace
checks on shipping GPUs. These smoke checks supplement visual inspection;
they do not establish pixel parity across graphics backends.

## Provenance

The six `upgrade-*.riv` files are unmodified copies of the upstream paths
recorded in `../upgrade-samples.json`, at the pinned commit above. Rive's
MIT notice is included as `RIVE-LICENSE.txt`. Keep the catalog, copies, and
runtime pin in sync when upgrading again. `gpu_canvas.riv` is the unmodified
project-provided editor export added on September 14, 2026; it is not an
upstream test fixture.

To try an additional editor export, put the `.riv` in this directory and
click **Refresh** in the viewer. It appears in the ordinary Sample picker.
