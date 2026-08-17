#include "Motion.h"

#include <algorithm>
#include <cmath>

namespace burin
{
namespace
{
constexpr double kPi  = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

/// Fractional part, always in 0..1 including for negative input. `fmod` keeps
/// the sign, which would put a negative rate's sawtooth in -1..0 and make the
/// Ramp waveform jump the wrong way when Spin was reversed.
double Frac( double x )
{
	return x - std::floor( x );
}
} // namespace

double MotionClock( double seconds, float bpm, float barPhase, SyncMode mode,
                    float rate, float manualPhase )
{
	if( mode == SyncMode::Manual )
	{
		// Exactly one cycle across the travel. Unlike flipbook's frame clock
		// there is no "one short of the end" correction here: a cycle is
		// continuous and 1.0 is the same position as 0.0, so the top of the
		// slider landing back at the start is correct rather than a wrap.
		return std::min( std::max( static_cast< double >( manualPhase ), 0.0 ), 1.0 );
	}

	const double perUnit = static_cast< double >( rate );

	if( mode == SyncMode::Free )
		return seconds * perUnit;

	const double tempo      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
	const double barSeconds = 240.0 / tempo;// four beats to the bar
	const double estimate   = seconds / barSeconds;
	const double within     = std::min( std::max( static_cast< double >( barPhase ), 0.0 ), 1.0 );

	const double bars = within + std::round( estimate - within );

	return ( mode == SyncMode::Beat ? bars * 4.0 : bars ) * perUnit;
}

float WaveValue( Wave wave, double cycles )
{
	const double t = Frac( cycles );

	switch( wave )
	{
	case Wave::Sine:
		return static_cast< float >( std::sin( t * kTau ) );

	case Wave::Triangle:
		// Starts at 0, up to 1 at a quarter, down through 0 at a half to -1 at
		// three quarters. Phase-aligned with the sine so switching between them
		// does not jump.
		return static_cast< float >( t < 0.25 ? t * 4.0
		                             : t < 0.75 ? 2.0 - t * 4.0
		                                        : t * 4.0 - 4.0 );

	case Wave::Ramp:
		// Sawtooth over -1..1 rather than 0..1, so that the centre of the
		// travel is the same place it is for every other waveform and changing
		// the control does not also shift the drawing.
		return static_cast< float >( t * 2.0 - 1.0 );

	case Wave::Pulse:
	default:
		return t < 0.5 ? 1.0f : -1.0f;
	}
}

MotionState SolveMotion( const MotionSettings& settings, double cycles )
{
	MotionState out;

	const float wx = WaveValue( settings.wave, cycles );

	// A quarter cycle of separation turns a pair of drifts into an ellipse
	// rather than a diagonal. See the header.
	const float wy = WaveValue( settings.wave, cycles + 0.25 );

	// Zoom moves in octaves, not in multiples. An excursion of one octave means
	// half size to double size, which is symmetrical on screen; the same
	// excursion expressed additively would be a small move down and a large one
	// up, and would cross zero and invert the drawing before it got anywhere.
	out.zoom = settings.zoom * std::pow( 2.0f, settings.zoomMove * wx );

	out.panX = settings.panX + settings.driftX * wx;
	out.panY = settings.panY + settings.driftY * wy;

	// Spin is a rate and always ramps -- see the header. Note that it reads
	// `cycles` whole rather than its fractional part, so the rotation
	// accumulates across cycles instead of snapping back at each one.
	out.rotate = settings.rotate + static_cast< float >( settings.spin * cycles * kTau );

	return out;
}

} // namespace burin
