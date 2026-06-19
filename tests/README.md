# rive-qt-plugin test suite

Qt Test suite that drives the wrapper classes against the vendored
`rive-runtime`. It exists to pin the contract "our wrapper uses Rive
correctly," and **doubles as the regression gate for runtime bumps**: when
`third_party/rive-runtime` is bumped, this suite either stays green or fails
loudly at a named wrapper↔runtime contract point.

Build with the default top-level configure (`RIVE_QT_BUILD_TESTS` defaults
`ON` when this repo is top-level), then:

```sh
cmake -B build -S . -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/<platform>
cmake --build build
ctest --test-dir build --output-on-failure
```

## Layers

| Dir | What it covers | GPU? |
|-----|----------------|------|
| `unit/` | Pure helpers (`rive_key_map`) | no |
| `file/` `artboard/` `state_machine/` `view_model/` `asset_loader/` | Wrapper contract against fixtures via `rive::NoOpFactory` — load, enumerate, instance, advance, data-binding, triggers, text, focus mapping | no |
| `render/` | Offscreen Metal smoke: `decodeImage`→`makeImageTexture` on a real GPU factory, plus a non-blank `RiveView` frame grab (APPLE only) | yes |

The headless layers run anywhere (offscreen QPA, no display). The `render/`
layer is macOS/Metal-only and `QSKIP`s when no offscreen GPU context is
available, so it never blocks a GPU-less runner.

## Conventions (match these)

- One `QObject` per `tst_<name>.cpp` with `private slots:` test cases, ending
  in `QTEST_MAIN(TstX)` + `#include "tst_x.moc"`. Use `QCOMPARE`/`QVERIFY`,
  and `QSignalSpy` for the wrappers' signals.
- Headless tests register through the `riveqt_add_headless_test()` helper in
  `tests/CMakeLists.txt`, which compiles the runtime's `no_op_factory.cpp`
  in, links `rive_qt` + `rive` (the latter carries the ABI-gating defines),
  matches Rive's RTTI-off ABI, and forces the offscreen QPA.
- Register one `add_test(NAME <Suite>.<case> ...)` per slot so `ctest` output
  and branch-protection signal stay granular.
- Resolve fixtures via `riveqt_test::fixturePath/fixtureUrl/loadFixtureBytes`
  (`tests/common/test_helpers.h`) — never hard-code paths.

## Adding a headless contract test

1. Drop a small, stable `.riv` in `tests/fixtures/` (introspect names with
   `build/tools/rive-asset-gen/rive-asset-gen --input <f>.riv --output-cpp
   /tmp/x.h --namespace X`).
2. Add `tests/<area>/tst_<name>.cpp` and a one-line
   `tests/<area>/CMakeLists.txt`:
   ```cmake
   riveqt_add_headless_test(tst_<name> SUITE <Suite> SLOTS caseA caseB ...)
   ```
3. `add_subdirectory(<area>)` in `tests/CMakeLists.txt` if the area is new.

## Render goldens (opt-in)

`Render.matchesGoldenSnapshot` is skipped unless a baseline PNG is committed
under `tests/fixtures/snapshots/`. Generate one locally with
`UPDATE_SNAPSHOTS=1 ctest -R Render` and commit it. Strict pixel goldens are
kept out of the default gate because GPU/driver differences make them flaky;
the non-blank check is the portable signal.
