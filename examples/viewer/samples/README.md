# Runtime update checks

The viewer's **Update checks** picker loads six fixtures from Rive
`runtime-v0.1.399` (`1cdd96e32f8a7366ed682e834a4d84def51498ed`). Selecting a
check restores playback at 1×, Contain fit, the sample's artboard/view-model
preset, auto-binding, and no layout-size override. Instructions appear above the
viewer. The ordinary Sample and Artboard pickers remain available.

| Sample | What to check |
| --- | --- |
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

These samples exercise regular Luau scripting, not custom GPU canvas/shader
scripts. GPU canvas still needs renderer integration: the new factory hooks
route scripts to the GPU device, but `GPUCanvas:beginRenderPass()` requires
a deferred recorder. Our backends currently draw directly. A `.riv` export
alone therefore does not complete GPU canvas support. Cache-as-bitmap also
requires the canvas host/deferred rendering integration.

## GPU canvas validation plan

These steps are pending; the six checks above do not establish GPU canvas support.

1. Integrate Rive's `DeferredSession` and `DeferredInlineHost` with the Qt
   backend, starting with Metal. Imports must route to the recording Ore
   context and canvas host, and recorded work must replay into Qt's render
   target in the right order. The inline host can replay on the existing
   render thread; a separate worker is not required.
2. Add a native pixel test based on upstream
   `tests/gm/ore_render_deferred_canvas.cpp`: clear a 200×200 GPU canvas to
   a known color, composite its image into the Qt target, and read back the
   expected pixels. This can be authored without an editor export.
3. Add an exported `.riv` that uses `context:gpuCanvas()` and an animated
   shader, with scripts, shaders, and any textures embedded. A moving gradient
   or rotating triangle is enough. Include ordinary Rive shapes in front of
   and behind the canvas plus a transparent region to inspect compositing.
   Capture a short editor preview as a visual reference.
4. Add it to the viewer catalog and GPU tests. Verify visible shader output,
   changing frames, pause/resume, layout-driven canvas resizing, transparency,
   repeated reloads, and closing/reopening windows. Capture script errors as
   failures: ordinary 2D artwork can render even if the GPU canvas failed.
   Repeat on D3D11 before claiming Windows support.

Once the integration is present, put the exported file in this directory and
click **Refresh** to try it in the viewer.

## Automated checks

Build normally, then run:

```sh
ctest --test-dir build -L runtime-upgrade --output-on-failure
```

Every fixture's artboards are imported, bound, and advanced on all platforms.
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
runtime pin in sync when upgrading again.

To try an additional editor export, put the `.riv` in this directory and
click **Refresh** in the viewer. It appears in the ordinary Sample picker.
