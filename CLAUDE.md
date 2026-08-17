# rasterizer

Vector artwork rendered at the size it is being seen at — as **two** FFGL
plugins for Resolume Arena/Avenue: a source (`Rasterizer`) that draws an SVG, and
an effect (`Rasterizer Over`) that draws one over the incoming clip. C++/GLSL,
CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the scale ladder, the dash arithmetic, or
anything that writes into an `NSVGshape`.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install both bundles to Resolume: `cmake --install build`
- Render a frame through the real plugin class:
  `./build/rztest --frame /tmp/f.png --file docs/example-plate.svg --size 1280x720`
- Set anything by name: `--set "Zoom=0.8" --set "Draw=2" --set "Recolour=3"`
- List parameters, with types, defaults and ranges: `./build/rztest --list`
- What a drawing parses to: `./build/rztest --doc --file yours.svg`
- What a rebuild costs on it: `./build/rztest --cost --file yours.svg`
- Regenerate the example drawing: `python3 tools/make_example_svg.py`

## OpenFX build
- `source/ofx/RasterizerOFX.cpp` → `build/Rasterizer.ofx.bundle` (target
  `RasterizerOFX`, `-DBUILD_OFX=OFF` to skip): **both** plugins in one bundle —
  `com.stoatworks.rasterizer` (generator) and `com.stoatworks.rasterizerover`
  (filter).
- **Almost nothing is mirrored.** Document, Style, Reveal, Motion, Raster,
  Compose, Settings and Controls are linked straight from source — the picture is
  built by a CPU rasteriser in both builds, so there is one implementation of all
  of it. Only the tail of `ComposeFrame` has a twin, in the fragment shader,
  marked `//= mirrored` in both. `rztest --gpu` is the test.
- `Settings.cpp` shares the *reading of the controls* too, which is what makes a
  preset the same look in Resolve as in Resolume rather than merely the same
  numbers.
- Sync offers Free and Manual only: OFX hosts carry no tempo. The two-entry
  dropdown is translated to the shared four-entry enum in `ReadParams` — an index
  passed through would select a mode with no clock behind it.
- OFX time arrives in *frames*; the plugin divides by the clip frame rate to get
  the seconds `MotionClock` wants.
- Smoke test (ofxprobe drives the Filter context; the generator's render runs
  only in a real host). **`--set-string` is required** — without a drawing, every
  numeric setting is measuring an empty frame that renders perfectly and draws
  nothing:
  ```
  ../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.rasterizerover \
    --size 480x270 --out /tmp/f.bmp --set-string "drawing=$PWD/docs/example-plate.svg" \
    --set "zoom=0.5"
  ```
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything, including the release-time traps: `tools/verify.sh`
- The central claim, measured against its own control: `./build/rztest --crisp`
- The ladder only ever snaps up: `./build/rztest --ladder`
- The cache is seldom as well as sharp: `./build/rztest --rebuilds`
- Fill and stroke are the file's own paint: `./build/rztest --style`
- Write-on timing, against `Reveal.cpp`: `./build/rztest --reveal`
- Clock, waveforms, spin, zoom octaves: `./build/rztest --motion`
- The shipped class in real GL, and the CPU/GPU mirror: `./build/rztest --gpu`
- Every factory preset through the real dropdown: `./build/rztest --presets`
- No dead controls: `python3 tools/sweep.py`

## Notes
- **The raster is rebuilt at the resolution it will be shown at, and the ladder
  always snaps UP.** Snapping to the nearest rung would have the GPU magnifying
  by up to 19%, which is the exact artefact this plugin exists to avoid.
  `--crisp` measures 1.0 px of edge at every zoom to 32x; the same raster left
  un-rebuilt measures 25 px at 32x.
- **The raster covers the visible window, not the document.** That is what keeps
  a deep zoom bounded and a small mark on a big artboard cheap.
- **Panning needs two rectangles**: `need` (on screen) and `want` (plus a
  margin). The cache is tested against `need`. One rectangle means a rebuild
  every frame — see `AGENTS.md`.
- **A zero-length dash entry hangs the host.** `nsvg__flattenShapeStroke`'s `j++`
  is only in the else branch. The ends of the reveal are special-cased rather
  than dashed, which also makes them exact.
- **`NSVGpaint` is a union.** Snapshot it whole — a `uint32_t` truncates a
  gradient pointer — and `Document::Release()` must `ResetShapes()` before
  `nsvgDelete`, or every unload leaks one allocation per gradient.
- **Outside the covered rectangle is transparent, not the clamped edge texel.**
  Both `Compose.cpp` and `Shaders.cpp` carry the bounds test.
- **nanosvg ignores `<text>` entirely** — no stub, no warning. Also `<use>`,
  `<image>`, `<clipPath>`, `<mask>`, `<pattern>`, `<filter>`. `ScanUnsupported`
  counts them and the note says so.
- **A stroke vanishes below `strokeWidth * scale = 0.01`**, one shape at a time
  as you zoom out. `Min Stroke px` is the floor.
- **Compare GPU against CPU in premultiplied space**, never by dividing the GPU
  frame by alpha.
- **`SetParamInfo` clamps a default only for `FF_TYPE_STANDARD`.** First Shape
  and Shape Count are real `FF_TYPE_INTEGER` parameters. **Option parameters hold
  the element value**, not 0..1, and are read through `Option()`.
- **Straight alpha until the last line of the fragment shader.** nanosvg
  un-premultiplies on the way out, and nothing in this path multiplies by alpha
  before the end.
- `rasterizer_core` is an **OBJECT** library, and each plugin's registration is
  listed directly in its own target — see `AGENTS.md`. `verify.sh` checks both
  failure directions, counting **unique** IDs (a universal binary has two slices
  and `strings` walks both).
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- A wrapper header must not differ from the library it wraps only by case.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. Both builds write it. It covers the failures that
all look identical from outside ("it does nothing"): a file that would not open
or parse, a file that parsed and contains nothing this renderer can draw (the
important one — live text), a raster capped for size, and a shader that would not
compile.

    ~/Library/Logs/rasterizer/rasterizer.YYYY-MM-DD.log
