#include "Reveal.h"

#include "Document.h"
#include "Hash.h"

#include <algorithm>
#include <cmath>

namespace rasterizer
{
namespace
{
/// Below this much progress the stroke is switched off rather than dashed, and
/// above `1 - this` the dash is removed and the stroke drawn whole.
///
/// It is not a tolerance on a comparison, it is the width of the region where a
/// dash entry would be short enough to be worth not trusting. A thousandth of a
/// path is well under one device pixel at any zoom this plugin offers, so
/// nothing visible is lost at either end -- and what is bought is that no dash
/// entry is ever near zero. See the header for what happens when one is.
constexpr float kRevealEnds = 1.0e-3f;

/// Absolute floor on a dash entry, in document units, applied on top of the
/// proportional guard above. Belt and braces: `kRevealEnds` bounds the entries
/// in terms of the path's own length, and a path can be short. A hang inside
/// somebody else's process is bad enough to be worth two guards.
constexpr float kMinDashLen = 1.0e-4f;

float Clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

float Smoothstep( float t )
{
	t = Clamp01( t );
	return t * t * ( 3.0f - 2.0f * t );
}
} // namespace

float ShapeProgress( int slot, int slotCount, float globalProgress, float stagger )
{
	if( slot < 0 || slotCount <= 0 )
		return 0.0f;

	const float p = Clamp01( globalProgress );
	const float s = std::max( stagger, 0.0f );

	if( s <= 0.0f || slotCount == 1 )
		return p;

	// The run lasts one shape's draw time plus the stagger accumulated across
	// the others, so p is stretched over that whole span and each slot reads
	// its own unit-length window out of it.
	const float span = 1.0f + static_cast< float >( slotCount - 1 ) * s;
	const float T    = p * span;

	return Clamp01( T - static_cast< float >( slot ) * s );
}

bool RevealPlan::Update( const Document& doc, const StyleSettings& style, RevealOrder order )
{
	const int n = doc.ShapeCount();

	int first = 0, count = 0;
	IsolateRange( style, n, first, count );

	if( valid_ && documentShapes_ == n && first_ == first && count_ == count && order_ == order )
		return false;

	documentShapes_ = n;
	first_          = first;
	count_          = count;
	order_          = order;
	valid_          = true;

	slot_.assign( n < 0 ? 0 : n, -1 );
	if( count <= 0 )
		return true;

	// Build the list of shape indices inside the window, then order it. The
	// slot is the position in that ordered list, so shapes outside the window
	// never take a slot and never delay the ones inside it.
	std::vector< int > members;
	members.reserve( count );
	for( int i = first; i < first + count; ++i )
		members.push_back( i );

	const std::vector< ShapeInfo >& info = doc.Shapes();

	switch( order )
	{
	case RevealOrder::Document:
		break;

	case RevealOrder::Reverse:
		std::reverse( members.begin(), members.end() );
		break;

	case RevealOrder::Longest:
		// Stable, and tie-broken by document index by construction: std::stable_sort
		// keeps equal-length shapes in the order the file listed them, so a
		// drawing of identical marks reveals left to right as drawn rather than
		// in whatever order the sort happened to leave them.
		std::stable_sort( members.begin(), members.end(), [ &info ]( int a, int b ) {
			return info[ a ].length > info[ b ].length;
		} );
		break;

	case RevealOrder::Shortest:
		std::stable_sort( members.begin(), members.end(), [ &info ]( int a, int b ) {
			return info[ a ].length < info[ b ].length;
		} );
		break;

	case RevealOrder::Scatter:
	default:
		std::stable_sort( members.begin(), members.end(), [ count ]( int a, int b ) {
			return ScatterKey( a, count ) < ScatterKey( b, count );
		} );
		break;
	}

	for( int slot = 0; slot < static_cast< int >( members.size() ); ++slot )
		slot_[ members[ slot ] ] = slot;

	return true;
}

int RevealPlan::Slot( int shapeIndex ) const
{
	if( shapeIndex < 0 || shapeIndex >= static_cast< int >( slot_.size() ) )
		return -1;
	return slot_[ shapeIndex ];
}

void ApplyReveal( const Document& doc, const RevealPlan& plan, const RevealSettings& reveal )
{
	if( reveal.mode == RevealMode::None )
		return;

	const int n = doc.ShapeCount();
	if( n <= 0 )
		return;

	const int slots = plan.Count();

	for( int i = 0; i < n; ++i )
	{
		NSVGshape* s = doc.Shape( i );
		if( s == nullptr )
			continue;

		const int slot = plan.Slot( i );
		if( slot < 0 )
			continue;// outside the isolate window; Style has already hidden it

		float p = ShapeProgress( slot, slots, reveal.progress, reveal.stagger );

		// Off retracts: the drawn length runs back toward the pen's starting
		// point. See RevealMode in Controls.h for why it is not an erase from
		// the other end.
		if( reveal.mode == RevealMode::Off )
			p = 1.0f - p;

		const ShapeInfo& info = doc.Shapes()[ i ];

		// --- the stroke ------------------------------------------------------
		if( s->stroke.type != NSVG_PAINT_NONE )
		{
			if( p <= kRevealEnds || info.length <= 0.0f )
			{
				// Switched off rather than dashed to nothing. A dash entry of
				// zero is an infinite loop in nsvg__flattenShapeStroke -- see
				// the header.
				s->stroke.type = NSVG_PAINT_NONE;
			}
			else if( p >= 1.0f - kRevealEnds )
			{
				// Dash removed, not set to "all on". Dashing strokes each run
				// as its own open subpath, so leaving it on would turn every
				// line join in the finished drawing into a pair of caps.
				// Document::ResetShapes has already restored the file's own
				// dash pattern, so this is simply leaving it alone.
			}
			else
			{
				const float on  = std::max( info.length * p, kMinDashLen );
				const float off = std::max( info.length * ( 1.0f - p ), kMinDashLen );

				s->strokeDashArray[ 0 ] = on;
				s->strokeDashArray[ 1 ] = off;
				s->strokeDashCount      = 2;// two, so nanosvg does not double the period
				s->strokeDashOffset     = 0.0f;
			}
		}

		// --- the fill --------------------------------------------------------
		if( s->fill.type == NSVG_PAINT_COLOR )
		{
			float fillAlpha;
			if( reveal.fillWindow <= 0.0f )
			{
				// Present for the whole of this shape's turn, and absent before
				// it. Absent before it matters: a fill that ignored the stagger
				// would show the finished drawing already coloured in while the
				// lines were still arriving.
				fillAlpha = ( p > 0.0f ) ? 1.0f : 0.0f;
			}
			else
			{
				const float w = reveal.fillWindow;
				fillAlpha     = Smoothstep( ( p - ( 1.0f - w ) ) / w );
			}

			const float a = info.fillOpacity * fillAlpha;
			s->fill.color = ( s->fill.color & 0x00FFFFFFu ) |
			                ( static_cast< unsigned int >( Clamp01( a ) * 255.0f + 0.5f ) << 24 );
		}
		else if( s->fill.type != NSVG_PAINT_NONE && p <= 0.0f )
		{
			// A gradient fill cannot be faded through the colour word -- it
			// keeps its alpha per stop. It can at least be made to respect the
			// stagger rather than appearing before its turn.
			s->fill.type = NSVG_PAINT_NONE;
		}
	}
}

} // namespace rasterizer
