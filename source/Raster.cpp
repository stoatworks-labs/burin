#include "Raster.h"

#include "Diag.h"
#include "SvgLib.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace burin
{
namespace
{
/// Rungs per octave on the scale ladder. Two means half-octave steps: the
/// raster is between 1.0 and 1.41 times as fine as the frame needs, so the
/// worst case is 2x the pixel area of an exact fit.
///
/// Four rungs would halve that waste and double the rebuild rate, which is the
/// wrong trade — the rebuild is the expensive event and the memory is not.
constexpr int kRungsPerOctave = 2;

/// How far past the visible rectangle the raster reaches, as a fraction of the
/// visible size on each side. A pan that stays inside this is free.
///
/// An eighth is about a third of a second of travel at a drift rate anyone
/// would use on a background element, which is the number that matters: it is
/// not chosen to make panning free, which is impossible, but to make the
/// rebuild rate during a pan comparable to the rebuild rate during a zoom.
constexpr float kCoverMargin = 0.125f;

/// Texels of transparent border kept outside the drawing's content box. Three
/// rather than one because the cap loop below can halve the scale after this
/// pad has been sized, and a pad that survives two halvings is still a pad.
constexpr float kCoverPadTexels = 3.0f;

float Clamp( float v, float lo, float hi )
{
	return v < lo ? lo : ( v > hi ? hi : v );
}

/// True when the angle is close enough to a multiple of a quarter turn that the
/// GPU resample is a straight axis swap and costs no sharpness.
bool IsAxisAligned( float radians )
{
	constexpr float kHalfPi = 1.57079632679489661923f;
	const float turns       = radians / kHalfPi;
	return std::fabs( turns - std::round( turns ) ) < 1.0e-3f;
}

bool RectContains( const Rect& outer, const Rect& inner )
{
	return outer.minx <= inner.minx && outer.miny <= inner.miny &&
	       outer.maxx >= inner.maxx && outer.maxy >= inner.maxy;
}
} // namespace

Transform2D Transform2D::Inverse() const
{
	const float det = a * d - b * c;

	// A degenerate map is reachable from the controls -- a Stretch fit of a box
	// with no height, or a zoom small enough to underflow -- and an inverse
	// full of infinities does not stay contained: it propagates into the cover
	// rectangle, which becomes the size of the float range, which asks for a
	// raster of two billion pixels a side. Returning the identity gives a wrong
	// picture instead of a hang, and the wrong picture is one nobody can reach
	// without dragging a slider to a place where there was nothing to see.
	if( std::fabs( det ) < 1.0e-12f )
		return Transform2D{};

	const float inv = 1.0f / det;

	Transform2D r;
	r.a = d * inv;
	r.b = -b * inv;
	r.c = -c * inv;
	r.d = a * inv;
	r.e = ( c * f - d * e ) * inv;
	r.f = ( b * e - a * f ) * inv;
	return r;
}

float SnapScale( float scale, int extraRungs )
{
	if( !( scale > 0.0f ) || !std::isfinite( scale ) )
		return 1.0f;

	const float rungs   = std::log2( scale ) * static_cast< float >( kRungsPerOctave );
	const float snapped = std::ceil( rungs - 1.0e-4f ) + static_cast< float >( extraRungs );

	// The epsilon before the ceil is not cosmetic. A scale that lands exactly
	// on a rung -- which is the common case, because 1.0 is a rung and an
	// un-zoomed drawing at native size sits on it -- would otherwise be pushed
	// to the next rung by the accumulated error of the log and the multiply,
	// doubling the pixel area of the most ordinary configuration there is.
	return std::exp2( snapped / static_cast< float >( kRungsPerOctave ) );
}

Transform2D BuildTransform( const RasterRequest& request, const Rect& box, float& baseScaleOut )
{
	const float frameW = static_cast< float >( std::max( request.frameWidth, 1 ) );
	const float frameH = static_cast< float >( std::max( request.frameHeight, 1 ) );

	const float boxW = std::max( box.Width(), 1.0e-6f );
	const float boxH = std::max( box.Height(), 1.0e-6f );

	const float sx = frameW / boxW;
	const float sy = frameH / boxH;

	// The uniform scale the drawing is fitted at, before Zoom. Stretch has no
	// single answer, so it takes the larger of the two and leaves the residual
	// to the non-uniform part below -- which means the axis that is squashed is
	// downsampled rather than the axis that is stretched being magnified.
	float baseScale = 1.0f;
	switch( request.fit )
	{
	case Fit::Fit: baseScale = std::min( sx, sy ); break;
	case Fit::Fill: baseScale = std::max( sx, sy ); break;
	case Fit::Stretch:
	default: baseScale = std::max( sx, sy ); break;
	}

	baseScaleOut = baseScale;

	const float zoom = std::max( request.motion.zoom, 1.0e-6f );

	// Non-uniform residual. Identity for everything but Stretch, where it
	// squashes the axis that the uniform scale over-covered.
	float aspectX = 1.0f, aspectY = 1.0f;
	if( request.fit == Fit::Stretch )
	{
		aspectX = sx / baseScale;
		aspectY = sy / baseScale;
	}

	const float scaleX = baseScale * zoom * aspectX;
	const float scaleY = baseScale * zoom * aspectY;

	const float cosT = std::cos( request.motion.rotate );
	const float sinT = std::sin( request.motion.rotate );

	// Pan is in fractions of the FITTED box, measured before Zoom, so that a
	// pan of half a unit is half a screen whatever the zoom is. Measured after
	// Zoom it would be half of the zoomed drawing, and a nudge at 8x would
	// throw the frame clean off the artwork.
	const float panX = request.motion.panX * boxW * baseScale;
	const float panY = request.motion.panY * boxH * baseScale;

	const float cx = ( box.minx + box.maxx ) * 0.5f;
	const float cy = ( box.miny + box.maxy ) * 0.5f;

	// T(frameCentre + pan) . R(theta) . S(scale) . T(-boxCentre)
	//
	// Pan is applied AFTER the rotation, in screen space. Applied before it,
	// dragging the X pan on a drawing rotated 30 degrees would move the picture
	// diagonally, which is not what the operator watching it thinks the control
	// does.
	Transform2D t;
	t.a = cosT * scaleX;
	t.b = sinT * scaleX;
	t.c = -sinT * scaleY;
	t.d = cosT * scaleY;
	t.e = frameW * 0.5f + panX - ( t.a * cx + t.c * cy );
	t.f = frameH * 0.5f + panY - ( t.b * cx + t.d * cy );

	return t;
}

Rect VisibleRect( const Transform2D& inverse, int frameWidth, int frameHeight, float margin )
{
	const float w = static_cast< float >( std::max( frameWidth, 1 ) );
	const float h = static_cast< float >( std::max( frameHeight, 1 ) );

	const float cornerX[ 4 ] = { 0.0f, w, 0.0f, w };
	const float cornerY[ 4 ] = { 0.0f, 0.0f, h, h };

	Rect r;
	for( int i = 0; i < 4; ++i )
	{
		float dx = 0.0f, dy = 0.0f;
		inverse.Apply( cornerX[ i ], cornerY[ i ], dx, dy );

		if( i == 0 )
		{
			r.minx = r.maxx = dx;
			r.miny = r.maxy = dy;
		}
		else
		{
			r.minx = std::min( r.minx, dx );
			r.miny = std::min( r.miny, dy );
			r.maxx = std::max( r.maxx, dx );
			r.maxy = std::max( r.maxy, dy );
		}
	}

	const float mx = r.Width() * margin;
	const float my = r.Height() * margin;
	r.minx -= mx;
	r.maxx += mx;
	r.miny -= my;
	r.maxy += my;

	return r;
}

bool Rasteriser::Key::operator==( const Key& other ) const
{
	// std::memcmp over the settings structs rather than a field list. Both are
	// plain aggregates of floats and enums with no padding worth worrying about
	// -- and, more to the point, a field-by-field comparison is a list that has
	// to be extended every time a control is added, silently returning "same"
	// for the one that was forgotten. The failure mode of forgetting here is a
	// control that appears completely dead, because the cache keeps handing
	// back the raster from before it was moved.
	return document == other.document &&
	       scale == other.scale &&
	       std::memcmp( &style, &other.style, sizeof( style ) ) == 0 &&
	       std::memcmp( &reveal, &other.reveal, sizeof( reveal ) ) == 0;
}

Rasteriser::Rasteriser()
{
	raster_ = nsvgCreateRasterizer();
}

Rasteriser::~Rasteriser()
{
	if( raster_ != nullptr )
		nsvgDeleteRasterizer( raster_ );
}

void Rasteriser::Invalidate()
{
	valid_ = false;
	key_   = Key{};
}

bool Rasteriser::Build( const Document& document, const RasterRequest& request, RevealPlan& plan )
{
	capped_    = false;
	capFactor_ = 1.0f;

	if( !document.Valid() || raster_ == nullptr )
	{
		valid_ = false;
		pixels_.clear();
		placement_ = RasterPlacement{};
		return false;
	}

	const Rect box = ( request.bounds == Bounds::Content ) ? document.Content() : document.Viewport();

	float baseScale       = 1.0f;
	const Transform2D fwd = BuildTransform( request, box, baseScale );
	const Transform2D inv = fwd.Inverse();

	// The device scale the frame actually needs, before snapping. Taken from
	// the transform rather than recomputed, so the two cannot disagree: the
	// length of the transformed unit-x vector IS the pixels-per-document-unit
	// this frame will draw at, whatever the fit and rotation did to get there.
	const float deviceScale = std::sqrt( std::fabs( fwd.a * fwd.d - fwd.b * fwd.c ) );

	// Rotation is resampled rather than re-rasterised, so it gets an extra rung
	// of resolution to lose. Not applied at right angles, where the resample is
	// an axis swap and loses nothing.
	const int extraRungs = IsAxisAligned( request.motion.rotate ) ? 0 : 1;

	float scale = SnapScale( deviceScale * std::max( request.detail, 0.01f ), extraRungs );

	// What has to be covered: the visible window, clipped to where ink can be.
	// Clipping to the content box and not the viewport is deliberate — a
	// drawing whose geometry overflows its declared viewport still has to be
	// rasterised where the geometry is, or the overflow is cropped away by the
	// cache rather than by anything the operator chose.
	// TWO rectangles, and the difference between them is the whole reason
	// panning is affordable.
	//
	// `need` is what is actually on screen this frame. `want` is that plus a
	// margin, and is what gets built. The cache is then tested against `need`,
	// so a pan is free until it has travelled the whole margin — whereas
	// testing against `want` would mean the cached cover never contained the
	// next frame's request, because that request already had the margin added
	// to it. The margin has to be slack in what is *built*, not in what is
	// *demanded*, and the two are only distinguishable if the code holds both.
	Rect need = VisibleRect( inv, request.frameWidth, request.frameHeight, 0.0f );
	Rect want = VisibleRect( inv, request.frameWidth, request.frameHeight, kCoverMargin );

	// The content box is expanded by a few texels before it clips anything.
	//
	// That padding is what guarantees the raster has a transparent border
	// wherever the clip was made by the drawing's own extent, which is what
	// Compose's "outside the cover is transparent" rule needs in order to be
	// continuous. Without it the ink runs right to the last texel of the
	// raster, and the boundary between the clamped edge inside and the
	// transparent outside is a hard step — a one-pixel bright rim around the
	// whole drawing, and it moves as the zoom crosses a rung.
	const float pad = kCoverPadTexels / std::max( scale, 1.0e-6f );

	const Rect ink = document.Content();
	const Rect clip{ ink.minx - pad, ink.miny - pad, ink.maxx + pad, ink.maxy + pad };

	auto clipTo = []( Rect& r, const Rect& c ) {
		r.minx = std::max( r.minx, c.minx );
		r.miny = std::max( r.miny, c.miny );
		r.maxx = std::min( r.maxx, c.maxx );
		r.maxy = std::min( r.maxy, c.maxy );
	};
	clipTo( need, clip );
	clipTo( want, clip );

	if( !want.Valid() )
	{
		// Nothing on screen. Not an error -- it is what panning off the edge of
		// a drawing looks like -- so the cache is dropped and an empty
		// placement returned rather than the last raster being left to draw in
		// a place it no longer belongs.
		valid_ = false;
		pixels_.clear();
		placement_       = RasterPlacement{};
		placement_.scale = scale;
		return true;
	}

	// Cap. The raster is bounded by what is on screen rather than by the
	// document, so this is reachable only on a genuinely enormous frame or a
	// Detail of 2 on a 4K output -- but when it is reached, quietly losing
	// detail is much better than glTexImage2D failing and the frame going
	// black, which is what crossing GL_MAX_TEXTURE_SIZE actually does.
	for( int guard = 0; guard < 32; ++guard )
	{
		const float pxW = want.Width() * scale;
		const float pxH = want.Height() * scale;
		if( pxW <= static_cast< float >( kMaxRasterPx ) && pxH <= static_cast< float >( kMaxRasterPx ) )
			break;

		scale *= 0.5f;
		capped_ = true;
		capFactor_ *= 0.5f;
	}

	// Does the cache already answer this?
	Key key;
	key.document = &document;
	key.scale    = scale;
	key.style    = request.style;
	key.reveal   = request.reveal;

	// Tested against `need`, not `want` — see above.
	if( valid_ && key == key_ && RectContains( placement_.cover, need ) )
		return false;

	// Rebuild. The cover is the wanted rectangle, not the intersection with
	// what was cached: growing it to include both would ratchet upward until
	// the raster covered the whole document, which is the bound this design
	// exists to avoid.
	const int w = std::max( 1, static_cast< int >( std::ceil( want.Width() * scale ) ) );
	const int h = std::max( 1, static_cast< int >( std::ceil( want.Height() * scale ) ) );

	pixels_.assign( static_cast< size_t >( w ) * static_cast< size_t >( h ) * 4u, 0 );

	// The three stages that decide what the pixels are, in the order they have
	// to run: the file's own state, then the style, then the reveal on top of
	// what the style left.
	document.ResetShapes();
	ApplyStyle( document, request.style, scale );
	plan.Update( document, request.style, request.reveal.order );
	ApplyReveal( document, plan, request.reveal );

	// tx and ty are applied AFTER the scale inside nanosvg, so they are in
	// device pixels and this is where the cover rectangle's origin goes.
	nsvgRasterize( raster_, document.Image(),
	               -want.minx * scale, -want.miny * scale, scale,
	               pixels_.data(), w, h, w * 4 );

	placement_.cover  = want;
	placement_.scale  = scale;
	placement_.width  = w;
	placement_.height = h;

	key_   = key;
	valid_ = true;
	++rebuilds_;

	if( capped_ )
		diag::warn( "raster capped at " + std::to_string( kMaxRasterPx ) +
		            " px; detail reduced by " + std::to_string( capFactor_ ) );

	return true;
}

} // namespace burin
