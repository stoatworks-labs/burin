#pragma once

#include "Controls.h"
#include "Document.h"
#include "Motion.h"
#include "Reveal.h"
#include "Style.h"

#include <cstdint>
#include <string>
#include <vector>

namespace burin
{
/**
    The raster: the drawing, built at the size it will actually be seen at.

    This is the file the plugin exists for. Everything else decides *what* to
    draw; this decides at what resolution, over what part of the document, and
    — the part that makes it viable at all — how seldom.

    ## The whole idea

    A bitmap zoomed eight times is eight times blurrier. A vector zoomed eight
    times is not, but only if somebody re-rasterises it, and re-rasterising a
    drawing of a thousand paths on every frame of a zoom is not something a VJ
    rig has the budget for. So:

    **The scale is snapped to a ladder, and the raster is rebuilt only when the
    rung changes.** The rungs are half an octave apart. Between rebuilds the GPU
    scales the existing raster, and because it never has to cover more than half
    an octave the picture never drifts far from the resolution it was built for.

    **The ladder always snaps UP, never to the nearest rung.** This is the
    difference between a plugin that is sharp and one that is usually sharp.
    Snapping to the nearest would mean the GPU sometimes *magnifying* the raster
    — by up to 19% at the worst point of each rung, which is plainly visible on
    a hard edge and is exactly the artefact the plugin exists to avoid. Snapping
    up means the raster is always at least as fine as the screen needs and the
    GPU only ever *minifies*, which costs sharpness nothing. The price is up to
    twice the pixel area, and Detail is the control that buys it back.

    **The raster covers the visible window, not the document.** This is what
    keeps a deep zoom bounded. Zooming to 50x on an A0 drawing at 8192 px a side
    would need a raster of two thirds of a gigapixel if the whole document had
    to be covered; covering only what is on screen, plus a margin, it is the
    same cost at 50x as at 1x. The margin is what makes panning cheap — a pan
    that stays inside it is free, and only one that leaves it rebuilds.

    ## What that costs, honestly

    **A zoom that runs continuously rebuilds twice an octave.** On a heavy
    drawing that is a visible hitch, twice per octave, and Detail is the release
    valve.

    **The write-on rebuilds every frame.** Its whole purpose is that the pixels
    change, so there is nothing to cache; a reveal running on a thousand-path
    drawing is a thousand-path rasterise per frame. Measured rather than
    guessed: `burintest --cost` reports it for the file you are pointing at.

    **Rotation is resampled, not re-rasterised.** nanosvg takes a scale and a
    translation and no angle, and rotating the geometry instead would mean
    rewriting every control point every frame. So the raster is axis-aligned in
    document space and the GPU turns it, which is a bilinear resample and
    therefore slightly soft. It is mitigated rather than ignored: when the angle
    is not a multiple of a quarter turn the ladder takes **one extra rung**, so
    the resample has half an octave of headroom to lose. See `SnapScale`.

    ## Straight alpha, all the way through

    nanosvg's rasteriser composites premultiplied internally and calls
    `nsvg__unpremultiplyAlpha` on the way out, so what arrives here is straight
    alpha — which is what the rest of the fleet wants and what the shader
    premultiplies on its last line. Nothing in this path multiplies by alpha.
*/

/// A 2x3 affine map, in SVG's `[a b c d e f]` order:
///
///     x' = a*x + c*y + e
///     y' = b*x + d*y + f
///
/// Document space has **y pointing down**, like SVG, and so does frame space —
/// the single flip into OpenGL's y-up happens in the vertex shader and nowhere
/// else. Carrying SVG's convention through every CPU stage means no
/// intermediate step has to remember which way up it is.
struct Transform2D
{
	float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

	void Apply( float x, float y, float& outX, float& outY ) const
	{
		outX = a * x + c * y + e;
		outY = b * x + d * y + f;
	}

	/// The inverse, or the identity if this map is degenerate. Degenerate
	/// happens for real — Zoom at the bottom of its travel on a Stretch fit of
	/// a zero-height box — and an inverse full of infinities propagates into
	/// the cover rectangle and asks for a raster of 2^31 pixels.
	Transform2D Inverse() const;
};

/// What the raster is being asked for.
struct RasterRequest
{
	int frameWidth  = 1920;
	int frameHeight = 1080;

	Bounds bounds = Bounds::Viewport;
	Fit fit       = Fit::Fit;

	MotionState motion;
	float detail = 1.0f;

	StyleSettings style;
	RevealSettings reveal;
};

/// Where a built raster sits, and at what resolution.
struct RasterPlacement
{
	Rect cover;        ///< the document-space rectangle the raster covers
	float scale = 1.0f;///< device pixels per document unit, after snapping
	int width   = 0;
	int height  = 0;
};

/// Snap a device scale up onto the ladder.
///
/// `rungsPerOctave` is 2 — half-octave steps. `extraRungs` is how many further
/// rungs to take, which is where the rotation headroom comes from.
///
/// Always rounds **up**: see the class comment. The consequence worth stating
/// is that `SnapScale(s) >= s` for every s, which is the property the whole
/// crispness claim rests on and which `burintest --ladder` asserts directly.
float SnapScale( float scale, int extraRungs = 0 );

/// The map from document units to frame pixels for this request.
///
/// Split out and pure because three different things need to agree about it:
/// the rasteriser (to work out what to cover), the vertex shader (to place the
/// quad) and the OFX build (to sample per pixel). One definition, three
/// consumers, no mirror.
Transform2D BuildTransform( const RasterRequest& request, const Rect& box, float& baseScaleOut );

/// The document-space rectangle visible in the frame, expanded by `margin` as a
/// fraction of its own size.
///
/// Under rotation this is the axis-aligned bound of the four transformed frame
/// corners, which is larger than the rotated rectangle it contains — up to 41%
/// larger at 45 degrees. That waste is real and is the reason the rotation
/// headroom is one rung rather than two.
Rect VisibleRect( const Transform2D& inverse, int frameWidth, int frameHeight, float margin );

/// Builds and caches the raster.
///
/// Not thread-safe and does not need to be: one instance lives on one plugin
/// instance and is only ever touched from the render call.
class Rasteriser
{
public:
	Rasteriser();
	~Rasteriser();

	Rasteriser( const Rasteriser& )            = delete;
	Rasteriser& operator=( const Rasteriser& ) = delete;

	/// Build the raster for this frame, reusing the cached one where possible.
	///
	/// Returns true if the pixels changed and the caller must re-upload. That
	/// is the only thing the caller needs to know: the placement is available
	/// either way, because a cached raster still has to be *drawn* somewhere
	/// and the transform moves every frame even when the pixels do not.
	bool Build( const Document& document, const RasterRequest& request, RevealPlan& plan );

	const RasterPlacement& Placement() const { return placement_; }
	const std::vector< uint8_t >& Pixels() const { return pixels_; }

	/// True when the last Build had to reduce detail to stay inside
	/// `kMaxRasterPx`, and by how much. Surfaced so Diag can say so once rather
	/// than the operator wondering why a deep zoom went soft.
	bool Capped() const { return capped_; }
	float CapFactor() const { return capFactor_; }

	/// Rebuild counter, for the harness. The ladder's whole claim is that this
	/// number stays small while the zoom moves, and the only way to check a
	/// claim about how *often* something happens is to count it.
	uint64_t Rebuilds() const { return rebuilds_; }
	void ResetCounters() { rebuilds_ = 0; }

	/// Drop the cache. Called when the document changes underneath us.
	void Invalidate();

private:
	/// Everything that, if it changes, means the pixels are wrong. Compared
	/// whole rather than field by field, so adding a setting to one of the
	/// structs cannot silently fail to invalidate the cache.
	struct Key
	{
		const Document* document = nullptr;
		float scale              = 0.0f;
		StyleSettings style;
		RevealSettings reveal;

		bool operator==( const Key& other ) const;
	};

	struct NSVGrasterizer* raster_ = nullptr;

	Key key_;
	RasterPlacement placement_;
	std::vector< uint8_t > pixels_;

	bool valid_       = false;
	bool capped_      = false;
	float capFactor_  = 1.0f;
	uint64_t rebuilds_ = 0;
};

} // namespace burin
