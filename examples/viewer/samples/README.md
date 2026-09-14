# Runtime update checks

The viewer's **Update checks** picker loads six fixtures from Rive
`runtime-v0.1.399` (`1cdd96e32f8a7366ed682e834a4d84def51498ed`) and the
project's `gpu_canvas.riv` shader triangle. Selecting a
check restores playback at 1×, Contain fit, the sample's artboard/view-model
preset, auto-binding, and no layout-size override. Instructions appear above the
viewer. The ordinary Sample and Artboard pickers remain available.

| Sample | What to check |
| --- | --- |
| `gpu_canvas.riv` | On Metal, expect a red top, green bottom-left, and blue bottom-right triangle on a dark square. This sample is static. Resize the viewer, reload, and reopen it. |
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

The Metal backend records imports, 2D drawing, and scripted GPU passes through
Rive's `DeferredSession`, then replays them on Qt's Metal queue. The recording
factory routes scripts to the deferred Ore context and canvas host. QImage
continues to decode raster assets on the real device. Replay runs on the
existing scene-graph render thread; there is no additional worker thread.

Choose **GPU canvas triangle (Metal)** in the viewer's **Update checks**
picker. `gpu_canvas.riv` contains `GPUCanvasExample` and `GPUTriangleShader`,
including its Metal shader. The automated check requires all three triangle
colors in the expected orientation, so a canvas background with a failing
script cannot pass. It also checks paused display, changes to the Qt view's
size, repeated file reloads, and closing/recreating the window and GPU context.

Run only this check with Metal API validation enabled:

```sh
MTL_DEBUG_LAYER=1 ctest --test-dir build \
  -R '^UpgradeSamples.render.gpu-canvas$' --output-on-failure
```

This static triangle does not test animated shader uniforms, script-driven
`GPUCanvas:resize()`, transparency, or nested canvas passes. Further fixtures
are needed for those cases and for cache-as-bitmap. GPU canvas integration is
Metal-only for now; D3D11, D3D12, OpenGL, and Vulkan still render directly.

## Automated checks

Build normally, then run:

```sh
ctest --test-dir build -L runtime-upgrade --output-on-failure
```

The six upstream fixtures' artboards are imported, bound, and advanced on all
platforms. GPU canvas requires a device and is covered by the Metal render
test, rather than the headless import suite.
The fitted-text test checks the 7.4 hug height against the expected fitted
size. The global scripting test verifies that Luau enumerates the shared
view models and writes their names to the root VM. On macOS with Qt 6.6+, each fixture
also renders through the real Metal backend;
the scripted clip must keep changing over multiple frames. The same
`../upgrade-samples.json` catalog drives the picker and tests.

To save inspection frames on macOS:

```sh
RIVE_QT_SAMPLE_SNAPSHOT_DIR=/tmp/rive-upgrade-frames \
  ctest --test-dir build -R '^UpgradeSamples.render\.' --output-on-failure
```

GPU cases can skip on machines without offscreen Metal. Import errors and
blank frames fail. These smoke checks supplement visual inspection; they
do not establish pixel parity across graphics backends.

## Provenance

The six `upgrade-*.riv` files are unmodified copies of the upstream paths
recorded in `../upgrade-samples.json`, at the pinned commit above. Rive's
MIT notice is included as `RIVE-LICENSE.txt`. Keep the catalog, copies, and
runtime pin in sync when upgrading again. `gpu_canvas.riv` is the unmodified
project-provided editor export added on September 14, 2026; it is not an
upstream test fixture.

To try an additional editor export, put the `.riv` in this directory and
click **Refresh** in the viewer. It appears in the ordinary Sample picker.
