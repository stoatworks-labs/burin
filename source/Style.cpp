#include "Style.h"

#include "Document.h"

#include <algorithm>
#include <cmath>

namespace burin
{
namespace
{
float Clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}
} // namespace

unsigned int PackABGR( float r, float g, float b, float a )
{
	const unsigned int R = static_cast< unsigned int >( Clamp01( r ) * 255.0f + 0.5f );
	const unsigned int G = static_cast< unsigned int >( Clamp01( g ) * 255.0f + 0.5f );
	const unsigned int B = static_cast< unsigned int >( Clamp01( b ) * 255.0f + 0.5f );
	const unsigned int A = static_cast< unsigned int >( Clamp01( a ) * 255.0f + 0.5f );
	return R | ( G << 8 ) | ( B << 16 ) | ( A << 24 );
}

void RotateHue( float& r, float& g, float& b, float turns )
{
	if( turns == 0.0f )
		return;

	const float mx = std::max( r, std::max( g, b ) );
	const float mn = std::min( r, std::min( g, b ) );
	const float d  = mx - mn;

	// A grey has no hue to rotate. Returning early rather than letting the
	// arithmetic run is not just an optimisation: the hue of a grey is 0/0, and
	// carrying that through would turn the drawing's blacks and whites into an
	// arbitrary colour the moment the spread left zero.
	if( d <= 0.0f )
		return;

	float h = 0.0f;
	if( mx == r )
		h = ( g - b ) / d;
	else if( mx == g )
		h = 2.0f + ( b - r ) / d;
	else
		h = 4.0f + ( r - g ) / d;

	h = h / 6.0f + turns;
	h -= std::floor( h );// wrap into 0..1

	const float s = mx > 0.0f ? d / mx : 0.0f;
	const float v = mx;

	const float i = std::floor( h * 6.0f );
	const float f = h * 6.0f - i;
	const float p = v * ( 1.0f - s );
	const float q = v * ( 1.0f - s * f );
	const float t = v * ( 1.0f - s * ( 1.0f - f ) );

	switch( static_cast< int >( i ) % 6 )
	{
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
}

void IsolateRange( const StyleSettings& style, int shapeCount, int& firstOut, int& countOut )
{
	if( shapeCount <= 0 )
	{
		firstOut = 0;
		countOut = 0;
		return;
	}

	const int first = std::min( std::max( style.firstShape, 0 ), shapeCount - 1 );

	// A count of zero or less means "to the end". The default has to be the
	// whole drawing: a default that selected nothing would render an empty
	// frame, which is indistinguishable from a file that failed to load and
	// would send every first-time user to the log for no reason.
	int count = ( style.shapeCount <= 0 ) ? shapeCount : style.shapeCount;
	count     = std::min( count, shapeCount - first );

	firstOut = first;
	countOut = count;
}

void ApplyStyle( const Document& doc, const StyleSettings& style, float deviceScale )
{
	const int n = doc.ShapeCount();
	if( n <= 0 )
		return;

	int first = 0, count = 0;
	IsolateRange( style, n, first, count );
	const int last = first + count;// exclusive

	// The colour walk runs across the ISOLATED range, not across the document.
	// The operator is looking at what is on screen: isolating ten shapes out of
	// four hundred and getting a fortieth of the colour wheel would read as the
	// spread control being broken.
	const float span = ( count > 1 ) ? static_cast< float >( count - 1 ) : 1.0f;

	for( int i = 0; i < n; ++i )
	{
		NSVGshape* s = doc.Shape( i );
		if( s == nullptr )
			continue;

		if( i < first || i >= last )
		{
			s->flags &= static_cast< unsigned char >( ~NSVG_FLAGS_VISIBLE );
			continue;
		}

		// --- which halves are painted -------------------------------------
		if( style.draw == Draw::FillOnly )
			s->stroke.type = NSVG_PAINT_NONE;
		else if( style.draw == Draw::StrokeOnly )
			s->fill.type = NSVG_PAINT_NONE;

		// --- stroke width --------------------------------------------------
		s->strokeWidth *= style.strokeScale;

		// The floor exists because of a real cliff in nanosvg's rasteriser: it
		// skips the stroke pass entirely when `strokeWidth * scale <= 0.01`. A
		// hairline on a drawing being zoomed out therefore does not fade, it
		// disappears — and because different shapes have different widths, they
		// disappear one at a time, which reads as the file being corrupt rather
		// than as a resolution limit.
		if( style.strokeMinPx > 0.0f && deviceScale > 0.0f && s->stroke.type != NSVG_PAINT_NONE )
		{
			const float minDoc = style.strokeMinPx / deviceScale;
			if( s->strokeWidth < minDoc )
				s->strokeWidth = minDoc;
		}

		// --- colour ---------------------------------------------------------
		if( style.recolour != Recolour::Off )
		{
			const float t     = ( count > 1 ) ? static_cast< float >( i - first ) / span : 0.0f;
			const float turns = style.colourSpread * t;

			float r = style.colourR, g = style.colourG, b = style.colourB;
			RotateHue( r, g, b, turns );

			const bool doFill   = ( style.recolour == Recolour::Fill || style.recolour == Recolour::Both );
			const bool doStroke = ( style.recolour == Recolour::Stroke || style.recolour == Recolour::Both );

			// Alpha is taken from the shape's own paint, so a mark the file made
			// half-transparent stays half-transparent. A gradient carries alpha
			// per stop rather than in the colour word, and flattening it can
			// only keep one number, so it is taken as opaque -- the shape
			// opacity is still there to bring it back down.
			if( doFill && s->fill.type != NSVG_PAINT_NONE )
			{
				const float a = ( s->fill.type == NSVG_PAINT_COLOR )
				                    ? static_cast< float >( ( s->fill.color >> 24 ) & 0xFF ) / 255.0f
				                    : 1.0f;
				// Setting `type` before `color` is not cosmetic: they share a
				// union with the gradient pointer, so this is the moment the
				// pointer stops being one. Document::Release() restores every
				// paint before nsvgDelete for exactly this reason.
				s->fill.type  = NSVG_PAINT_COLOR;
				s->fill.color = PackABGR( r, g, b, a );
			}

			if( doStroke && s->stroke.type != NSVG_PAINT_NONE )
			{
				const float a = ( s->stroke.type == NSVG_PAINT_COLOR )
				                    ? static_cast< float >( ( s->stroke.color >> 24 ) & 0xFF ) / 255.0f
				                    : 1.0f;
				s->stroke.type  = NSVG_PAINT_COLOR;
				s->stroke.color = PackABGR( r, g, b, a );
			}
		}
	}
}

} // namespace burin
