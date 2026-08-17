#pragma once

#include "Controls.h"

namespace rasterizer
{
/**
    Where the drawing is, and how big, and which way up.

    ## The one idea, carried over

    **The frame is a pure function of (clock, parameters).** Nothing is
    advanced, nothing accumulates, no position is integrated. The fleet's usual
    invariant, and the usual reasons apply — the harness renders t = 3.25 cold,
    beat sync is a different clock rather than a second code path, and a drop to
    30 fps changes how smooth the motion is and not where it has got to.

    It matters more here than the frame rate alone suggests, because of what the
    rasteriser downstream does with the answer. The scale ladder in Raster.cpp
    rebuilds the picture when the zoom crosses a rung, and an integrated zoom
    would arrive at a slightly different value on every machine and every run —
    so the same show would rebuild at different moments, and a performance that
    was smooth in rehearsal could stutter in the room. Closed-form, the rung a
    given beat lands on is the same everywhere.

    ## Spin does not read the waveform

    Zoom Move and Drift are excursions: they go somewhere and come back, and the
    Wave control says how. Spin is a **rate** — turns per cycle — so it is
    always a continuous ramp, and applying a sine to it would produce a rotation
    that speeds up and slows down without ever going anywhere, which is what
    Rotate plus a drift already is.

    ## The two drift axes are a quarter cycle apart

    Drift X and Drift Y share one clock, and a shared phase would make every
    combination of the two a diagonal line. Offsetting Y by a quarter cycle
    makes it an ellipse instead — a drawing that wanders rather than one that
    slides back and forth on one axis. It costs nothing when only one of the two
    is set, because a quarter cycle of an amplitude of zero is still zero.
*/

/// Everything the motion is solved from. Held as physical units -- the
/// conversion from the host's 0..1 sliders has already happened in Controls.
struct MotionSettings
{
	SyncMode sync = SyncMode::Free;
	float rate    = 0.2f;///< cycles per second, beat or bar
	float phase   = 0.0f;///< 0..1, Manual only
	Wave wave     = Wave::Sine;

	float zoom     = 1.0f;///< multiplier on the fitted size
	float zoomMove = 0.0f;///< excursion in octaves either side of `zoom`

	float panX = 0.0f;///< -1..1, fractions of the fitted box
	float panY = 0.0f;
	float driftX = 0.0f;///< excursion, same units
	float driftY = 0.0f;

	float rotate = 0.0f;///< radians
	float spin   = 0.0f;///< turns per cycle
};

/// The answer: where the drawing sits this frame.
struct MotionState
{
	float zoom   = 1.0f;
	float panX   = 0.0f;
	float panY   = 0.0f;
	float rotate = 0.0f;///< radians
};

/// The motion clock, in **cycles** since the run began.
///
/// Free is seconds times the rate. Beat and Bar count beats or bars, recovered
/// statelessly from the tempo and the position-in-bar the host hands over --
/// the fleet's usual recovery: the clock estimates how many bars have passed,
/// `barPhase` gives the exact position inside this one, and the whole number
/// reconciling the two is `round( estimate - barPhase )`. Continuous across the
/// bar line, and exact while the estimate is within half a bar of the truth.
///
/// Manual ignores everything but `manualPhase`, and maps the slider across
/// exactly one cycle. It is the mode for keyframing against an edit, and the
/// only one where the OFX build behaves identically to the FFGL build -- an OFX
/// host has no tempo to offer.
double MotionClock( double seconds, float bpm, float barPhase, SyncMode mode,
                    float rate, float manualPhase );

/// A waveform sampled at `cycles`, always -1..1.
///
/// Ramp is a sawtooth over the same range rather than 0..1, so that switching
/// waveform does not also change where the middle of the travel is. Pulse is a
/// square wave, which is the one that reads on a beat.
float WaveValue( Wave wave, double cycles );

/// Solve the motion for a given clock reading.
///
/// Pure, and takes the clock rather than reading one, so the harness can ask
/// for any moment in any order and the OFX build can hand over a time in frames
/// converted to seconds without a second code path.
MotionState SolveMotion( const MotionSettings& settings, double cycles );

} // namespace rasterizer
