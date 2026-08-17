#pragma once

#include "Controls.h"

namespace rasterizer
{
class Document;

/**
    Which parts of the drawing get painted, how wide, and in what colour.

    Everything here works by writing into the live `NSVGshape` structs before
    the rasterise, because that is the only place nanosvg looks. Nothing is
    additive and nothing accumulates: `Document::ResetShapes()` runs first,
    every frame, so this always starts from what the file said.

    ## Fill and stroke are exact here, and that is the point

    `nsvgRasterize` tests `shape->fill.type != NSVG_PAINT_NONE` and
    `shape->stroke.type != NSVG_PAINT_NONE` independently, in a paint-order loop
    that draws them as separate passes. So "stroke only" is not an edge detect,
    not a colour key and not an approximation — it is the drawing's own stroke,
    at the drawing's own widths and joins, with the fill pass simply not run.

    On artwork that was composed entirely as solids that gives you the wireframe
    of something whose author never drew one, which is the single most
    interesting thing this plugin does to a file it is pointed at.

    ## Order matters: Style runs before Reveal

    Reveal works through the stroke dash array, so it has to know whether there
    is a stroke to dash. `Draw::FillOnly` removes it, and the reveal then has
    nothing to run along — so it falls back to driving the fill fade alone,
    which turns the write-on into a staggered build-in. That is a documented
    consequence rather than a special case, but it only reads correctly if the
    stroke has already been switched off by the time Reveal looks.
*/
struct StyleSettings
{
	Draw draw = Draw::Both;

	/// Multiplier on the file's own stroke width, and a floor in device pixels.
	float strokeScale = 1.0f;
	float strokeMinPx = 0.0f;

	Recolour recolour = Recolour::Off;
	float colourR     = 1.0f;
	float colourG     = 1.0f;
	float colourB     = 1.0f;

	/// Hue turns walked across the isolated range. See ApplyStyle.
	float colourSpread = 0.0f;

	/// The isolate window, in document order. `count <= 0` means "to the end",
	/// which is what makes the default (0, 0) the whole drawing rather than
	/// nothing — a default that hid everything would look exactly like a file
	/// that failed to load.
	int firstShape = 0;
	int shapeCount = 0;
};

/// The half-open range of shape indices the isolate window selects, clamped
/// into the document. Split out from ApplyStyle because Reveal needs the same
/// answer — a shape outside the window must not take up a slot in the stagger
/// order, or isolating the last ten shapes of a drawing would leave them
/// waiting for ninety invisible ones to draw first.
void IsolateRange( const StyleSettings& style, int shapeCount, int& firstOut, int& countOut );

/// Write `style` into the document's live shapes.
///
/// `deviceScale` is pixels per document unit at the scale this frame will be
/// rasterised at, and is needed only by the stroke floor — a minimum expressed
/// in device pixels has to be converted back into the document's units before
/// nanosvg sees it.
///
/// Call after `Document::ResetShapes()` and before `ApplyReveal()`.
void ApplyStyle( const Document& doc, const StyleSettings& style, float deviceScale );

/// Pack 0..1 components into nanosvg's colour word.
///
/// nanosvg stores colour as **ABGR** — `NSVG_RGB(r,g,b)` is
/// `r | g<<8 | b<<16` — so red is the low byte. Writing the RGBA word every
/// other graphics API uses swaps red and blue, which on a monochrome test file
/// looks perfectly correct and is why this is a named function rather than a
/// shift written out at each of its call sites.
unsigned int PackABGR( float r, float g, float b, float a );

/// Rotate a colour's hue by `turns`, keeping saturation and value. Used by the
/// colour walk. Round-trips through HSV, so a fully desaturated colour is
/// unchanged by any rotation — grey has no hue to move.
void RotateHue( float& r, float& g, float& b, float turns );

} // namespace rasterizer
