#pragma once

#include "Controls.h"
#include "Style.h"

#include <vector>

namespace rasterizer
{
class Document;

/**
    The write-on: strokes drawn in along their own length.

    ## How it is done, and the one thing that must never happen

    nanosvg has no notion of a partly-drawn path, but it does implement stroke
    dashing — so a path drawn `p` of the way along is a two-entry dash pattern
    of `[L*p, L*(1-p)]`, on then off, where `L` is the path's length from
    `Document`. The rasteriser walks the flattened polyline, strokes the "on"
    run and skips the "off" run, and what comes out is a pen part way through.

    **A zero-length dash entry hangs the host.** In `nsvg__flattenShapeStroke`
    the loop over the polyline is `for (j = 1; j < r->npoints2; )` and the `j++`
    lives **only in the else branch** — the branch taken when the current
    segment fits inside the remaining dash. If `dashLen` is zero, every segment
    overflows it, so the other branch runs forever: it splits the segment, flips
    the dash state, takes the next entry, and if that entry is also zero it does
    the same thing again without ever advancing `j`. Two zero entries is an
    infinite loop with a `nsvg__addPathPoint` in it, which is to say it is
    Resolume hanging and growing until it is killed.

    So the ends of the range are **special-cased rather than expressed as
    dashes**, which is also what makes them exact:

    - at `p <= 0` the stroke is switched off (`stroke.type = NSVG_PAINT_NONE`)
      rather than dashed to nothing;
    - at `p >= 1` the dash is switched off (`strokeDashCount = 0`) and the
      stroke draws whole;
    - in between, both entries are clamped to a positive floor.

    The `p >= 1` case is not only a safety measure. Dashing changes the picture:
    each dash run is stroked as its own open subpath, so line **joins become
    caps**. A finished drawing that was still nominally dashed would differ from
    the same drawing with the reveal switched off, at every corner. Handing the
    dash back at the end is what makes "reveal complete" and "no reveal" the
    same frame.

    ## Per-shape timing

    With `N` shapes and a stagger of `s` — measured in units of one shape's own
    draw time — the whole run lasts `1 + (N-1)*s` of those units. Shape in slot
    `k` starts at `k*s` and finishes one unit later, so its own progress is
    `clamp(T - k*s)` where `T = p * (1 + (N-1)*s)`.

    At `s = 0` every shape draws together and the run lasts one unit. At `s = 1`
    each shape starts exactly as the one before it finishes. Both fall out of
    the same expression rather than being modes.

    ## The fill cannot be drawn on

    A fill is an area, not a length: there is nothing to run along. So it fades
    instead, over the last `fillWindow` of its shape's own reveal — line first,
    colour following it in, which is the order somebody drawing would work in.

    **Gradient fills do not fade.** The fade is applied to the alpha byte of the
    fill colour, and a gradient keeps its alpha per stop rather than in that
    word. Fading one would mean snapshotting and restoring every stop of every
    gradient each frame, for a case that a logo being drawn on almost never
    hits. A gradient-filled shape appears when its turn starts.
*/
struct RevealSettings
{
	RevealMode mode = RevealMode::None;

	/// 0..1 across the whole run, all shapes and their stagger included.
	float progress = 1.0f;

	/// Per-shape offset, in units of one shape's own draw time.
	float stagger = 0.0f;

	RevealOrder order = RevealOrder::Document;

	/// Fraction of a shape's own reveal over which its fill fades up. Zero
	/// means the fill is simply present for the whole of that shape's turn.
	float fillWindow = 0.33f;
};

/// The slot each shape occupies in the stagger order.
///
/// Cached rather than recomputed, because three of the five orders are sorts
/// and the clock advances every frame while the order does not. `Rebuild` is
/// called only when the document, the isolate window or the order changes;
/// `Slot()` is what the per-frame path asks.
class RevealPlan
{
public:
	/// Rebuild if any input differs from the cached one. Returns true if the
	/// table was actually rebuilt, which the harness asserts on.
	bool Update( const Document& doc, const StyleSettings& style, RevealOrder order );

	/// Slot of shape `i`, or -1 if it is outside the isolate window.
	///
	/// Shapes outside the window take **no slot at all** rather than an unused
	/// one. Isolating the last ten shapes of a hundred-shape drawing must not
	/// leave them waiting for ninety invisible ones to finish drawing first.
	int Slot( int shapeIndex ) const;

	int Count() const { return count_; }

	void Invalidate() { valid_ = false; }

private:
	std::vector< int > slot_;///< index -> slot, -1 outside the window
	int first_             = 0;
	int count_             = 0;
	int documentShapes_    = -1;
	RevealOrder order_     = RevealOrder::Document;
	bool valid_            = false;
};

/// Progress of one shape, 0..1, given its slot in the run.
///
/// Split out and pure so the harness can check the stagger arithmetic without
/// rendering anything -- and so the OFX build and the web demo have one
/// definition to agree with rather than a second implementation to drift from.
float ShapeProgress( int slot, int slotCount, float globalProgress, float stagger );

/// Write the reveal into the document's live shapes.
///
/// Call after `ApplyStyle()`: this reads whether a stroke survived, and with
/// `Draw::FillOnly` there is nothing to dash, so the reveal drives the fill
/// fade alone and becomes a staggered build-in.
void ApplyReveal( const Document& doc, const RevealPlan& plan, const RevealSettings& reveal );

} // namespace rasterizer
