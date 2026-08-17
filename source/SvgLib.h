#pragma once

/**
    nanosvg, included in exactly one place.

    Both nanosvg headers are single-header libraries: the declarations come out
    plainly, and the *implementation* comes out only under
    `NANOSVG_IMPLEMENTATION` / `NANOSVGRAST_IMPLEMENTATION`. Defining either in
    more than one translation unit gives a page of duplicate-symbol errors at
    link time naming functions nobody in this repo wrote.

    So: everything that needs the types includes **this** header, which defines
    neither. `SvgLib.cpp` is the one file that defines both, and it contains
    nothing else.

    ## Why this file is not called NanoSVG.h

    It was, for about an hour. On a case-insensitive filesystem — which is to
    say on the Mac this is developed on — `#include "nanosvg.h"` from a file
    called `NanoSVG.h` in an earlier include directory resolves to **itself**,
    and the vendored library is never reached. It does not fail: the include
    guard stops the recursion, the types come from wherever the *next* include
    happened to find them, and clang mentions it only as a `-Wnonportable-
    include-path` warning in a wall of other output. On Linux and in CI, where
    the filesystem is case-sensitive, the same source resolves differently.

    A wrapper header must not differ from the library it wraps only by case.

    ## What nanosvg does not do, and why it is still the right library

    It ignores `<text>` entirely — not badly, *entirely*: there is no `"text"`
    string anywhere in the parser, so a `<text>` element contributes no shapes
    and no warning. It also ignores `<use>`, `<image>`, `<clipPath>`, `<mask>`,
    `<pattern>` and `<filter>`. `Document.cpp` scans the source for those and
    reports them, because "the plugin drew nothing" and "your file is mostly
    live text" are the same picture.

    Against that: nanosvg is the only option that hands over the **shape list**.
    A more complete renderer — LunaSVG, ThorVG — draws a correct picture into a
    canvas and gives you back a bitmap, and this plugin's whole reason to exist
    is the things you can only do with the paths still separated: draw fills
    without strokes, reveal a path along its own length, stagger one shape
    against the next, recolour a stroke without touching a fill. A finished
    bitmap cannot be asked any of those questions.

    Vendored at `external/nanosvg`, zlib licence, unmodified. Everything this
    plugin needs is achieved by writing into the parsed `NSVGshape` structs
    before each rasterise — `flags`, `fill.type`, `stroke.type`, `strokeWidth`,
    `strokeDashArray` — all of which `nsvgRasterize` reads at call time. Not
    forking it is deliberate: see AGENTS.md.
*/

// nanosvg.h uses `strncpy` into fixed-size id buffers, which MSVC's CRT
// deprecates in favour of its own non-portable `strncpy_s`. The library is
// vendored and unmodified, so the warning is silenced at the include rather
// than fixed at the source.
#ifdef _MSC_VER
	#pragma warning( push )
	#pragma warning( disable : 4996 )
#endif

#include "nanosvg.h"
#include "nanosvgrast.h"

#ifdef _MSC_VER
	#pragma warning( pop )
#endif
