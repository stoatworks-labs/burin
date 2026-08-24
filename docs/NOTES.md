# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*burin — SVG rendered at the size it is seen: two FFGL plugins + OpenFX; RELEASED v0.1.0 2026-08-17, CONFIRMED working in Resolume Arena; OFX still untested in Resolve*

**burin** (started 2026-08-17) — vector artwork rendered at the resolution
the frame actually needs. `~/Projects/resolume/burin`, **MIT, intended
public** as `stoatworks-labs/burin`.

**RELEASED 2026-08-17, v0.1.0**, all six homes done:
`github.com/stoatworks-labs/burin` (6 assets),
demo `burin.stoatworks-labs.com`,
card `stoatworks-labs.com/software/burin`,
video `https://www.youtube.com/watch?v=7LqRVcUemkQ`,
Reel `https://www.instagram.com/reel/DcIejrUiJ7f/`.

**Named for the engraver's tool** — a steel rod cut to a lozenge point, pushed
through copper to raise a line. Started life as "rasterizer"; renamed before the
repo existed. The rename was safe by substitution because the NAME used -izer
and the CONCEPT used British -iser (`Rasteriser`, "rasterise"), so they never
collided — but nanosvg's own `NSVGrasterizer`, `nsvgCreateRasterizer` and
`nsvgDeleteRasterizer` all contain it and had to be repaired afterwards.

**Two FFGL plugins from one class** (the fleet's usual shape): `Burin`
(source, `BU01`) and `Burin Over` (effect, `BU02`), plus an OpenFX bundle
carrying both. Built on **nanosvg**, vendored unmodified at `external/nanosvg`.

**The one idea:** the raster is rebuilt at the resolution it will be shown at,
and the ladder **always snaps UP**. Half-octave rungs; snapping to the *nearest*
would let the GPU magnify by up to 19%, which is the exact artefact the plugin
exists to avoid. `burintest --crisp` measures 1.0 px of edge transition at every
zoom from 1x to 32x, against 1/3/3/7/13/25 px for the same raster left
un-rebuilt — **the control is part of the test**, not a note.

**The raster covers the visible window, not the document**, which is what bounds
a deep zoom. Panning needs **two rectangles**: `need` (on screen) and `want`
(plus an eighth margin, and what gets built); the cache is tested against
`need`. One rectangle means a rebuild every frame — cost 300 rebuilds in 300
frames before they were separated.

**Unusually little is mirrored.** The picture is built by a CPU rasteriser in
*both* builds, so the OFX target links Document/Style/Reveal/Motion/Raster/
Compose/Settings/Controls straight from source. `Settings.cpp` shares the
*reading of the controls* too, which is what makes a preset genuinely the same
look in Resolve as in Resolume. Only the tail of `ComposeFrame` has a twin, in
the fragment shader. `burintest --gpu` reports them **bit-exact** (worst 0 levels).

**Traps found the hard way, all in AGENTS.md:**
- **A zero-length dash entry hangs the host.** `nsvg__flattenShapeStroke`'s
  `j++` lives only in the else branch. The reveal special-cases both ends
  instead of dashing them, which also makes them exact.
- **`NSVGpaint` is a union.** A `uint32_t` snapshot truncates a gradient pointer
  to 32 bits, and `nsvg__deletePaint` frees the gradient only while `type` still
  says gradient — so `Document::Release()` must `ResetShapes()` **before**
  `nsvgDelete` or every unload leaks one allocation per gradient inside
  Resolume. Proven both ways with `leaks(1)`: 2 gradients, 128 bytes.
- **A wrapper header must not differ from the library it wraps only by case.**
  `NanoSVG.h` including `"nanosvg.h"` resolved to itself on macOS.
- **Outside the cover must read transparent, not the clamped edge texel** —
  clamping smears the raster's lit border across the whole frame.
- **nanosvg renders no `<text>` at all** — no stub, no warning. Biggest
  user-facing limitation; counted and reported by `ScanUnsupported`.
- **A stroke vanishes below `strokeWidth * scale = 0.01`**, one shape at a time
  as you zoom out. `Min Stroke px` is the floor.
- **Compare GPU against CPU in premultiplied space** — dividing the GPU frame by
  alpha reported 255 levels of disagreement on pixels whose alpha was 1/255.

`tools/verify.sh` runs everything including the OFX `CFBundleExecutable` +
codesign checks from [new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md). `sweep.py` finds
0 dead controls of 37; its CONTEXT/ENDS tables are most of the file and every
entry is a false failure that actually happened (Fit==Fill on a square frame;
Position at both ends is the drawing off-screen *both* times; Phase spans one
cycle so its ends coincide; Colour Spread rotates the hue of the default
**white**, which has none).

**Cost, measured** on the 464-shape example with the write-on running (the worst
case — its pixels change every frame): 8.0 ms rasterise at 720p, 13.6 at 1080p,
44.6 at 4K. Only the rasterise is paid in Resolume, and only on a rebuild.

`docs/example-plate.svg` is an original astrolabe plate from
`tools/make_example_svg.py` — 464 shapes chosen for wildly varied path lengths,
separate fill-only and stroke-only shapes, and sub-pixel centre detail. No live
text, deliberately.

`web/` is a demo that ports the *decisions* and hands the drawing to the
**browser's** SVG engine rather than shipping nanosvg to WASM; its ladder agrees
with the C++ at every probe. LIVE at `burin.stoatworks-labs.com`.

**`LoadString` is a `windows.h` macro** (→ `LoadStringA`), so `Document::LoadText`
is called that on purpose. It surfaced as an unresolved-symbol LINK error on the
FIRST TAG and cannot reproduce on macOS — verify.sh passes clean. CI is the only
proof. Same hazard: `LoadImage`, `DrawText`, `GetObject`, `CreateFile`,
`Rectangle`, `Polygon`, `GetCurrentTime`.

**Confirmed working in Resolume Arena 2026-08-17**, by Allan, after the v0.1.0
release. Both FFGL plugins load and render. That is "it loads and it draws" — it
was NOT a sweep of the controls, so two questions stay open and must not be
reported as settled: whether `FF_TYPE_INTEGER` with a range comes out as a typed
spinner (still unproven fleet-wide, same as [flipbook](https://github.com/stoatworks-labs/flipbook/blob/main/docs/NOTES.md) (`flipbook`) — and a plugin
that loads would look identical either way, because the fallback is a working
control with the wrong widget), and whether the twice-an-octave rebuild is
visible mid-zoom in a room.

**The OpenFX build has NOT been opened in Resolve, Nuke or Natron.** It builds,
exports `OfxGetPlugin` and passes the signing checks, which is as far as
flipbook's got too.

Related: [flipbook](https://github.com/stoatworks-labs/flipbook/blob/main/docs/NOTES.md) (`flipbook`) (donor for CMake, harness, Diag, presets),
[ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md), [plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md),
**release workflow** (working-practice note, kept in Claude memory).
