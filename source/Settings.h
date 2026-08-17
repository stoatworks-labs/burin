#pragma once

#include "Compose.h"
#include "Controls.h"
#include "Motion.h"
#include "Raster.h"

namespace burin
{
/**
    The one reading of the controls, shared by both builds.

    Every parameter this plugin has arrives as a float in 0..1 (or an option
    index), and something has to turn that into the physical settings the
    renderer wants. The obvious place for it is the plugin class — and putting
    it there would mean writing it **twice**, once for FFGL and once for
    OpenFX, which is the single most likely place for the two builds to drift
    apart. A preset is a table of these same numbers, so a divergence here would
    make "Blueprint" a different look in Resolume than in Resolve while both
    read the same table and neither was obviously wrong.

    So it lives here, takes a bare array, and both hosts' glue calls it. The
    FFGL build's array is its own `params_`; the OFX build fills one from its
    param handles immediately before calling in. Neither carries a second
    opinion about what Zoom means.

    The clock is the one thing NOT handled here, because it is the one thing the
    two hosts genuinely differ on: FFGL has a host clock and a tempo, OFX has a
    frame number and a frame rate and no tempo at all. Each computes `cycles`
    its own way and passes the result in.
*/

/// The motion controls, before the clock is applied.
MotionSettings MotionFromParams( const float* params );

/// Everything the rasteriser needs, given a clock reading in cycles.
///
/// `cycles` is what `MotionClock` returned. It drives both the motion and — in
/// the running reveal modes — the write-on, which is what makes a reveal cued
/// against a bar land on the bar.
RasterRequest RequestFromParams( const float* params, int width, int height, double cycles );

/// The tint, background and mix. `isEffect` is what makes Mix apply; the source
/// build declares the parameter so a composition can move between the two, and
/// ignores it.
ComposeSettings ComposeFromParams( const float* params, bool isEffect );

/// Fill `params` with the plugin's defaults, in host-facing space.
///
/// Shared for the same reason as the rest of this file: the FFGL constructor
/// and the OFX `describeInContext` both need them, and a default that differed
/// between the builds would be a plugin that looked different on first drop in
/// each host.
void DefaultParams( float* params );

} // namespace burin
