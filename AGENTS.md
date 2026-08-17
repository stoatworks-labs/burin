# burin — orientation for another LLM (or a newcomer)

**What it is:** vector artwork rendered at the size it is being seen at, as
**two** FFGL 2.1 plugins for Resolume Arena/Avenue, plus an OpenFX build of both
for Resolve/Nuke/Natron/Vegas. `Burin` is a source that draws an SVG;
`Burin Over` draws one over the incoming clip. C++17 + GLSL 4.1, CMake,
universal macOS `.bundle` and a Windows `.dll`. Public, MIT,
`github.com/stoatworks-labs/burin`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the scale ladder, the dash arithmetic, or
anything that writes into an `NSVGshape`.

---

## The one idea

**The raster is rebuilt at the resolution it will be shown at, and the ladder
always snaps up.**

A bitmap zoomed eight times is eight times blurrier. A vector zoomed eight times
is not — but only if somebody re-rasterises it, and re-rasterising a
thousand-path drawing on every frame of a zoom is not a budget a VJ rig has. So
the device scale is snapped onto a ladder of half-octave rungs and the raster is
rebuilt only when the rung changes. Between rebuilds the GPU scales what is
already there, and it never has to cover more than half an octave.

**Snapping UP rather than to the nearest rung is the whole difference between a
plugin that is sharp and one that is usually sharp.** Nearest would mean the GPU
sometimes *magnifying* the raster — by up to 19% at the worst point of each rung,
plainly visible on a hard edge, and exactly the artefact this plugin exists to
avoid. Snapping up means the raster is always at least as fine as the screen
needs and the hardware only ever minifies, which costs sharpness nothing. The
price is up to twice the pixel area; Detail is the control that buys it back.

`burintest --crisp` is where that stops being a claim. It renders a hard edge at six
zoom levels and measures the width of its alpha transition in device pixels:

```
    zoom   re-rasterised   frozen at 1x
       1x        1.0 px        1.0 px
       2x        1.0 px        3.0 px
       4x        1.0 px        3.0 px
       8x        1.0 px        7.0 px
      16x        1.0 px       13.0 px
      32x        1.0 px       25.0 px
```

The right-hand column is the same code with the ladder taken away — one raster
built at 1x and then magnified. **It is part of the test, not a note**: a
measurement that cannot fail proves nothing, and this is the control that shows
the left-hand column means something.

### What else falls out of it

**The raster covers the visible window, not the document.** This is what keeps a
deep zoom bounded: at 50x on a large drawing, covering the whole document would
need two thirds of a gigapixel, and covering only what is on screen costs the
same at 50x as at 1x. It is also what makes the plugin cheap on a small mark in
the corner of a big artboard — the raster is the size of the mark.

**Panning is nearly free, and that needs TWO rectangles.** `need` is what is
actually on screen; `want` is that plus an eighth on each side, and is what gets
built. The cache is tested against `need`. Testing against `want` — which is the
obvious single-rectangle version — means the cached cover never contains the next
frame's request, because that request already had the margin added to it. It
cost 300 rebuilds in 300 frames of pan before the two were separated, and it
reads as "the cache does not work" rather than as an off-by-one.

**A still frame never rebuilds, and a six-octave zoom rebuilds thirteen times in
six hundred frames.** `burintest --rebuilds` asserts both. The ladder has to be
*seldom* as well as sharp, and "seldom" is a claim about frequency, which can
only be checked by counting.

---

## The traps

Ordered by how much they will cost you.

**A zero-length dash entry hangs the host.** The write-on is implemented as a
two-entry stroke dash, and in nanosvg's `nsvg__flattenShapeStroke` the loop over
the polyline is `for (j = 1; j < r->npoints2; )` with the `j++` **only in the
else branch** — the branch taken when the segment fits inside the remaining dash.
A `dashLen` of zero means every segment overflows, so the other branch runs
forever, splitting and flipping and appending points without ever advancing `j`.
That is Resolume hanging and growing until it is killed.

So the ends of the reveal are **special-cased rather than expressed as dashes**:
at progress 0 the stroke is switched off, at 1 the dash is removed, and in
between both entries are clamped to a positive floor. That also makes the ends
exact, which the flattening does not otherwise guarantee — see below. **Never
write a dash entry that could be zero.**

**`NSVGpaint` is a union, and the gradient in it is freed conditionally.** The
struct is a `type` beside an anonymous union of an `unsigned int color` and an
`NSVGgradient*`. Two things follow and both bite:

- A snapshot taken through `.color` **truncates a 64-bit pointer to 32 bits**, so
  a gradient-filled shape cannot be restored from it. `ShapeInfo` stores the
  whole `NSVGpaint` by value for this reason.
- `nsvg__deletePaint` frees the gradient **only while `type` still says
  gradient**. The render path recolours the live image every frame, which means
  writing a flat colour over that union and switching `type` to
  `NSVG_PAINT_COLOR` — so a document unloaded while recoloured leaks one
  allocation per gradient, every time the operator picks a different file, inside
  Resolume's process. `Document::Release()` calls `ResetShapes()` **before**
  `nsvgDelete` and that line is the entire fix. Measured both ways with
  `leaks(1)`: two gradients, 128 bytes, every unload.

**Outside the covered rectangle must be TRANSPARENT, not the clamped edge
texel.** This is the opposite of the usual choice and it is not interchangeable.
The cover is the visible window intersected with the drawing's content box, so it
is routinely smaller than the frame; clamping there takes the raster's lit border
and smears it across every pixel outside the cover, and the drawing appears as a
full-frame wash of its own edge colour. `Raster.cpp` pads the content clip by
three texels so there is a transparent border for the rule to meet, and the two
together make the join continuous. Both `Compose.cpp` and `Shaders.cpp` carry the
bounds test; removing it from either is a wash of colour in that build only.

**`windows.h` claims ordinary-looking method names, and the result is a LINK
error.** `Document::LoadText` is called that because `LoadString` is a macro
expanding to `LoadStringA`/`LoadStringW`. Translation units that reach windows.h
through the FFGL SDK then call `Document::LoadStringA` while `Document.cpp`
compiles `Document::LoadString`, and the linker reports an unresolved symbol
carrying a suffix nobody wrote. **It cannot reproduce on macOS or Linux**, so
`verify.sh` cannot catch it and CI is the only proof — as it was, on the first
tag of this repo. Avoid `LoadString`, `LoadImage`, `LoadIcon`, `DrawText`,
`GetObject`, `SendMessage`, `CreateFile`, `DeleteFile`, `CopyFile`,
`Rectangle`, `Ellipse`, `Polygon` and `GetCurrentTime` in any public API here.

**A wrapper header must not differ from the library it wraps only by case.**
`source/SvgLib.h` was called `NanoSVG.h` for about an hour. On a case-insensitive
filesystem — which is to say on a Mac — `#include "nanosvg.h"` from a file called
`NanoSVG.h` earlier in the search path resolves to **itself**; the include guard
stops the recursion, the types come from wherever the next include happens to
find them, and clang says only `-Wnonportable-include-path` in a wall of other
output. On Linux and in CI the same source resolves differently.

**nanosvg has no `<text>` support whatever** — not a stub, not a warning: there is
no `"text"` string anywhere in the parser. An SVG whose content is live text
parses perfectly, reports no error, and draws an empty frame. `<use>`, `<image>`,
`<clipPath>`, `<mask>`, `<pattern>` and `<filter>` go the same way.
`ScanUnsupported` counts them off the source text and the note says so, because
"the plugin does nothing" and "your file is mostly live text" are otherwise the
same picture. **This is the plugin's biggest user-facing limitation and it must
stay loud in the README, the OFX description and the log.**

**A stroke vanishes rather than thinning.** nanosvg's rasteriser skips the stroke
pass entirely when `strokeWidth * scale <= 0.01`. So hairlines on a drawing being
zoomed out do not fade, they disappear — and because different shapes have
different widths, they disappear *one at a time*, which reads as the file being
corrupt rather than as a resolution limit. `Min Stroke px` is the floor that
prevents it and it defaults to about one device pixel.

**Comparing the GPU against the CPU must happen in PREMULTIPLIED space.** The
shader's last line premultiplies (FFGL composites premultiplied); `ComposeFrame`
produces straight alpha. Bringing the GPU frame *down* by dividing by alpha
amplifies an 8-bit rounding difference by 255 on pixels whose alpha is one or two
levels — the first version of `burintest --gpu` reported the two as 255 levels apart
while their alpha channels agreed to the bit. Bringing the CPU frame *up* instead
gives a worst-case difference of **0**.

**The mirror is one function long, and that is not an accident.** Every other
plugin in the fleet builds its picture in a fragment shader and has to carry a
second implementation for OFX. This one builds its picture with a CPU rasteriser
in *both* builds, so Document, Style, Reveal, Motion, Raster, Compose, Settings
and Controls are linked straight from source by the OFX target. What is mirrored
is the tail of `ComposeFrame` against the tail of the fragment shader — five
lines, marked `//= mirrored` in both. `burintest --gpu` is the test.

**The fragment shader inverts the transform per pixel instead of transforming a
quad.** A quad would be faster and is not what happens, because the CPU
compositor has no rasteriser and must walk output pixels asking where each came
from. Making the shader do the same thing means the two are the same *algorithm*
rather than the same intent — a quad version would agree everywhere except at the
sub-pixel level, which is exactly where a mirror test has to be sensitive.

**Style runs before Reveal, and the order is load-bearing.** Reveal works through
the dash array, so it has to know whether a stroke survived. `Draw: Fill Only`
removes it, and the reveal then correctly falls back to driving the fill fade
alone — a staggered build-in. That reads right only if the stroke is already gone
by the time Reveal looks.

**Shapes outside the isolate window take no slot in the stagger order.** Isolating
the last ten shapes of a hundred-shape drawing must not leave them waiting for
ninety invisible ones to draw first.

### Inherited from the SDK

The fleet's usual list applies and is not repeated in full. The ones that
actually shaped code here:

- **`SetParamInfo`'s clamp applies to `FF_TYPE_STANDARD` only.** `First Shape`
  and `Shape Count` are real `FF_TYPE_INTEGER` parameters with real ranges, for
  the same reason flipbook's grid is: they count somebody else's file, and shape
  14 of a drawing is shape 14.
- **A TEXT parameter without a `SetTextParameter` override makes
  `FF_INSTANTIATE_GL` fail for the whole plugin** — the SDK sets every default on
  a fresh instance and deletes it if any set returns FF_FAIL, and the base method
  is a stub that does. Invisible to this harness, which calls the class directly.
- **Every `ffglex::Scoped*` binding CLEARS to 0 on scope exit rather than
  restoring**, so the render path uses plain `glUseProgram`/`glBindTexture` and
  puts the host's state back by hand.
- **`GetMaxGLTexCoords` is a free function in `FFGLLib.h`**, not in `ffglex`.
- **The host PUSHES time and tempo** into protected members via `SetTime` and
  `SetBeatInfo`; there are no getters.
- **Hosts disagree about whether `SetTime` is seconds or milliseconds.** The scale
  is inferred once from the size of the first forward step.

---

## Checking your work

`tools/verify.sh` runs the lot, including the two release-time traps that only
bite after a tag. The measurements that matter check different things:

- **`--crisp`** is the plugin's reason to exist, with its own control. Above.
- **`--ladder`** checks `SnapScale(s) >= s` across four thousand scales rather
  than at a handful of points, because the failure it guards against is an
  off-by-a-hair in the epsilon before the `ceil` — which shows up at the rungs
  and nowhere else. 1.0 is a rung, and an un-zoomed drawing sits on it.
- **`--rebuilds`** is the claim about frequency. Zoom, pan and still.
- **`--reveal`** checks the stagger arithmetic with nothing rendered, then
  measures the drawn length off the picture at every tenth, then checks that the
  two special-cased ends are exact — that p = 1 is the *same frame* as no reveal
  at all, which is what makes the joins right.
- **`--style`** renders the same document three ways and requires the unfilled
  shape to vanish under Fill Only and survive under Stroke Only. That is the
  check that says the separation is the file's own paint rather than something
  approximated from the picture.
- **`--gpu`** is the mirror, and drives the shipped class in a real GL context.
- **`--presets`** applies every preset through the real dropdown and samples
  **across that preset's own cycle** — not at t = 0, because a preset that turns
  the write-on on is legitimately empty at the instant the clock starts, and not
  over a fixed number of seconds, because the rates in the table span from a
  twelfth of a cycle a second down to a thirtieth. Both mistakes were made, and
  both reported a working preset as dead.
- **`sweep.py`** is the only thing that catches a dead control — a parameter
  declared, shown in the host, and never read. Its `CONTEXT` and `ENDS` tables
  are most of the file, and every entry is a false failure that happened: Fit and
  Fill are the same picture on a square frame, Position at both ends is the
  drawing entirely off screen *both* times, Phase spans exactly one cycle so its
  ends coincide, and Colour Spread rotates the hue of the default **white**,
  which has none.

The OFX build is smoke-tested with `ofxprobe` from
[resolume-ofx-bridge](https://github.com/stoatworks-labs/resolume-ofx-bridge).

**Host verification is Allan's, not an agent's.** Driving the Resolume GUI from a
session is unreliable.

**Both FFGL plugins load and render in Resolume Arena** — confirmed 2026-08-17,
after the v0.1.0 release. Be precise about what that does and does not settle:
it establishes that the bundles load, that the plugin registers, that `InitGL`
and `ProcessOpenGL` survive a real host's call sequence, and that a drawing
appears. It is **not** a sweep of the controls in a live host, and the questions
below were not part of it.

**The OpenFX build has still not been loaded into Resolve, Nuke or Natron.** It
builds, exports `OfxGetPlugin` and passes the signing checks in `verify.sh`,
which is exactly as far as flipbook's got before release too. A generator's
`render` in particular runs only in a real host — `ofxprobe` drives the Filter
context only.

Still worth checking, and unaffected by the Arena confirmation:

- **Whether Resolume honours `FF_TYPE_INTEGER` with a range** on First Shape and
  Shape Count. Structurally right, empirically unproven, same as flipbook — and
  a plugin that loads and draws would look identical either way, because the
  fallback is a working control with the wrong widget rather than a broken one.
  If it comes out as a 0..1 slider the fix is conversions in `Controls.cpp` and
  nothing else changes.
- **Whether the file picker offers `.svg`** and what it does with a path that no
  longer resolves. The plugin deliberately keeps the *previous* drawing on screen
  rather than blanking when a reload fails — a blank frame mid-show is worse than
  a stale one — so a broken path is quiet by design and the log is where it says
  so.
- **What a rebuild actually feels like mid-zoom on a heavy file.** `--cost` says
  14 ms of rasterise per frame at 1080p on the 464-shape example with the reveal
  running, and 45 ms at 4K. Those are honest numbers on this machine; the
  question is whether the twice-an-octave hitch during a continuous zoom is
  visible in a room.

---

## Things deliberately not done

- **No "take the incoming clip as the drawing"**, which is flipbook's most
  interesting effect-side mode. A clip is a raster. The whole reason this plugin
  exists is that its source is *not* one — there are paths in it, with fills and
  strokes and lengths — and a mode that fed it a bitmap would be a mode in which
  every interesting control did nothing.
- **No PDF, AI or EPS.** One format, read by one parser, with no external runtime
  dependency. AI is PDF underneath and a PDF vector extractor is a much larger
  surface with worse licence options.
- **No `.svgz`.** It is gzipped SVG and nanosvg has no inflate, so offering it in
  the picker would accept a file the plugin then silently fails to parse.
- **No gradient fill fade during a reveal.** A gradient keeps its alpha per stop
  rather than in the colour word, so fading one means snapshotting and restoring
  every stop of every gradient each frame. A gradient-filled shape appears when
  its turn starts. Flat fills fade.
- **Erase-from-the-start is not offered**, only retraction. nanosvg's dash walk
  always begins in the "on" state — `dashState` is initialised to 1 before the
  offset is consumed — so a two-entry dash can express "the first p of this path"
  and cannot express "the last p of it". Doing it anyway needs a three-entry
  pattern whose odd count doubles nanosvg's period and leaves a stub of stroke at
  the origin. Reverse order gets most of the same look honestly.
- **No mipmaps.** The raster is rebuilt whenever it would be minified past a
  rung, so the level the hardware would pick is the level that already exists.
- **No reload-on-change.** The drawing is parsed when the path changes and left
  alone. Watching the file would mean a stat per frame or a thread, for a
  workflow — editing an SVG with the plugin live — that is not how anyone uses
  this.
- **Rendering is not multi-threaded.** `nsvgRasterize` rasterises whole shapes
  into the full bitmap and does not band, so splitting it means either forking the
  library or compositing several passes. The measured cost is inside a 60 fps
  budget at 1080p, and Detail is the release valve above that.

Related: [flipbook](https://github.com/stoatworks-labs/flipbook) (the CMake,
harness, Diag and preset patterns came from there),
[downpour](https://github.com/stoatworks-labs/downpour),
[orrery](https://github.com/stoatworks-labs/orrery).

## Factory presets

`source/Presets.h` is one table of named looks in the host-facing parameter
space, and it drives **both** builds — the FFGL constructor and the OFX describe
each read it, so a preset cannot drift between Resolume and Resolve. Element 0 of
the dropdown is always **Custom**, which is not in the table: it means "the
sliders are the truth".

`Settings.cpp` is what makes that guarantee real rather than nominal. The reading
of the controls — every curve, every option decode — is shared, so the two builds
cannot disagree about what a number in the table means.

**What a preset covers here is the unusual part.** It never touches the Document
group — not the file, and not Bounds, which decides whether the drawing is framed
by its artboard or by its ink. Those describe the operator's own material, and a
preset that reached into them would re-crop a logo its designer framed
deliberately. It also leaves alone the Isolate range (shape 14 of one drawing has
nothing to do with shape 14 of another), Sync, and Mix.
