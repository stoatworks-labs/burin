#pragma once

#include "Raster.h"

#include <cstdint>
#include <vector>

namespace rasterizer
{
/**
    Putting the raster on the screen.

    The FFGL build does this on the GPU — one textured quad, one fragment
    shader. The OpenFX build has no GPU to do it on, and the harness needs to
    measure the *final* picture rather than the intermediate raster. So the
    sampling and the composite live here, once, and `Shaders.cpp` carries the
    GLSL that mirrors them.

    **This is the only mirrored arithmetic in the repo**, and it is worth being
    precise about why there is any at all. The rest of the fleet mirrors a lot
    more, because the rest of the fleet generates its picture per pixel on the
    GPU and has to say the same thing twice. Here the picture is built by a CPU
    rasteriser for both builds — Document, Style, Reveal, Motion and Raster have
    exactly one implementation and the OFX target links the same objects. What
    is left over is the last step: fetch a texel, tint it, composite it. Those
    three lines are in `Compose.cpp` and in `Shaders.cpp`, and `rztest --mirror`
    is the test whose only job is catching them drifting apart.

    ## Bilinear, and clamped half a texel inside

    The GPU samples `GL_LINEAR` with `GL_CLAMP_TO_EDGE`, so this does too. The
    half-texel inset matters for the same reason it does in flipbook: a sample
    taken exactly at the raster's edge takes half its weight from outside, and
    outside a straight-alpha raster is transparent black — a one-pixel dark
    fringe along every edge of the covered rectangle. Since the covered
    rectangle is a *window* onto a larger drawing, that fringe would fall in the
    middle of the picture, and it moves as you pan.

    ## Straight alpha until the very end

    The raster arrives straight-alpha from nanosvg. Tint multiplies RGB, opacity
    multiplies alpha, and the premultiply is the last thing that happens, after
    the background has been composited under and the input mixed in. Doing it
    earlier would make the tint act on `rgb * a` and darken every soft edge.
*/

/// One RGBA8 frame, straight alpha, row 0 at the **top** — the same convention
/// the raster uses, and the one flip into OpenGL's y-up happens in the shader.
struct Frame
{
	int width  = 0;
	int height = 0;
	std::vector< uint8_t > rgba;

	void Resize( int w, int h );
	bool Valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

/// Everything the composite needs that is not the raster itself.
struct ComposeSettings
{
	float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
	float opacity = 1.0f;

	float backR = 0.0f, backG = 0.0f, backB = 0.0f;
	float backOpacity = 0.0f;

	/// Effect build only: how much of the result replaces the incoming clip.
	float mix = 1.0f;
};

/// Sample the raster at a point in **document** space, bilinear, clamped.
/// Returns straight-alpha RGBA in 0..1. Outside the covered rectangle it
/// returns transparent, which is correct: that part of the document was not
/// rasterised because it is not on screen.
void SampleRaster( const std::vector< uint8_t >& pixels, const RasterPlacement& placement,
                   float docX, float docY, float outRGBA[ 4 ] );

/// Composite the raster into `frame` through `inverse` (frame pixels ->
/// document units).
///
/// `input` may be null, which is the source plugin's case. When it is not, it
/// is the incoming clip in the same layout as `frame`, and `settings.mix`
/// crossfades.
void ComposeFrame( Frame& frame, const std::vector< uint8_t >& pixels,
                   const RasterPlacement& placement, const Transform2D& inverse,
                   const ComposeSettings& settings, const Frame* input );

} // namespace rasterizer
