#include "Controls.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>

namespace rasterizer
{
namespace
{
float Clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// Map 0..1 onto lo..hi geometrically. The reason so many of these are
/// exponential rather than linear is always the same: a ratio is what the eye
/// reads. Half size and double size should be the same distance either side of
/// the detent, and on a linear control they are not.
float Exp( float v, float lo, float hi )
{
	return lo * std::pow( hi / lo, Clamp01( v ) );
}

/// 0..1 onto -1..1 with the centre of the slider at exactly zero, raised to
/// `power` to bunch the fine settings around it. Odd powers keep the sign.
float Signed( float v, float power )
{
	const float x = Clamp01( v ) * 2.0f - 1.0f;
	const float m = std::pow( std::fabs( x ), power );
	return x < 0.0f ? -m : m;
}
} // namespace

int Option( float value, int count )
{
	if( count <= 0 )
		return 0;
	const int i = static_cast< int >( std::lround( value ) );
	return std::min( std::max( i, 0 ), count - 1 );
}

float ZoomFromParam( float value )
{
	// Six octaves, 1/8..8, so 1.0 falls exactly on 0.5.
	return Exp( value, 0.125f, 8.0f );
}

float ZoomMoveFromParam( float value )
{
	const float v = Clamp01( value );
	return 3.0f * v * v;
}

float PanFromParam( float value )
{
	return Clamp01( value ) * 2.0f - 1.0f;
}

float DriftFromParam( float value )
{
	const float v = Clamp01( value );
	return v * v;
}

float RotateFromParam( float value )
{
	return ( Clamp01( value ) * 2.0f - 1.0f ) * 3.14159265358979323846f;
}

float SpinFromParam( float value )
{
	// Cubed: a tenth of a turn per cycle is a background element drifting, and
	// on a linear -4..4 control it would be a pixel and a half of travel.
	return Signed( value, 3.0f ) * 4.0f;
}

float RateFromParam( float value )
{
	return Exp( value, 0.01f, 4.0f );
}

float StrokeScaleFromParam( float value )
{
	// Squared with 1.0 at the centre: 0.5 * 2^2 == 2 would put 1.0 at 0.5 only
	// by accident, so it is written as an explicit two-piece curve instead.
	const float v = Clamp01( value );
	if( v <= 0.5f )
	{
		const float t = v * 2.0f;// 0..1 over the lower half
		return t * t;            // 0 .. 1
	}
	const float t = ( v - 0.5f ) * 2.0f;// 0..1 over the upper half
	return 1.0f + t * t * 7.0f;         // 1 .. 8
}

float StrokeMinFromParam( float value )
{
	return Clamp01( value ) * 4.0f;
}

float ColourSpreadFromParam( float value )
{
	return Clamp01( value ) * 2.0f - 1.0f;
}

float RevealFromParam( float value )
{
	return Clamp01( value );
}

float RevealStaggerFromParam( float value )
{
	const float v = Clamp01( value );
	// Dead zone below a twentieth of the travel, so "every shape together" is
	// reachable by dragging to the bottom rather than by luck.
	if( v < 0.05f )
		return 0.0f;
	const float t = ( v - 0.05f ) / 0.95f;
	return t * t * 4.0f;
}

float RevealFillFromParam( float value )
{
	return Clamp01( value );
}

float DetailFromParam( float value )
{
	// 0.25..2 exponential does not put 1.0 at 0.5 -- the geometric middle of
	// that range is 0.707 -- so it is two pieces, like the stroke scale.
	const float v = Clamp01( value );
	if( v <= 0.5f )
		return Exp( v * 2.0f, 0.25f, 1.0f );
	return Exp( ( v - 0.5f ) * 2.0f, 1.0f, 2.0f );
}

uint32_t ScatterKey( int shapeIndex, int shapeCount )
{
	// Mixed with the shape count so that the order for a 10-shape drawing is
	// not simply the first ten of the order for a 400-shape one. Stable for a
	// given (index, count), which is what keeps the order still while the clock
	// runs: an order re-rolled per frame would be a strobe, not an order.
	return Hash32( static_cast< uint32_t >( shapeIndex ) * 2654435761u ^ static_cast< uint32_t >( shapeCount ) );
}

} // namespace rasterizer
