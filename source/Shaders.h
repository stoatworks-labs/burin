#pragma once

#include <string>

namespace burin
{
/**
    The GPU half — which is deliberately almost nothing.

    Most of the fleet puts its picture in the shader. This one cannot: the
    picture is made by a CPU rasteriser, and by the time the GPU sees it there
    is a texture and an affine map and no drawing left to do. So the fragment
    shader's whole job is to fetch a texel, tint it, put a background under it,
    mix it with the clip and premultiply.

    **Those five lines are the only mirrored arithmetic in the repo**, and their
    twin is the tail of `ComposeFrame` in Compose.cpp, marked there with a
    `//= mirrored` comment. `burintest --mirror` renders the same frame through
    both and compares them; it is the only thing that catches the two drifting.

    ## Why the fragment shader inverts the transform per pixel

    The obvious alternative is to transform a quad in the vertex shader — send
    four corners through the forward map and let the hardware interpolate the
    texture coordinates. It would be faster, and it is not what happens here,
    because the CPU compositor cannot do it that way: with no rasteriser of its
    own it has to walk output pixels and ask where each one came from.

    Making the shader do the same thing means the two implementations are the
    same *algorithm* and not merely the same intent. A quad-based version would
    agree with the CPU one everywhere except at the sub-pixel level, which is
    exactly where a mirror test has to be sensitive to be worth running — and
    the difference would be invisible until somebody compared a Resolume render
    against a Resolve one on a still and found them half a pixel apart.

    The cost is one 2x3 multiply per fragment, which against a texture fetch is
    not measurable.

    ## The one flip

    Document space and frame space both have **y pointing down**, all the way
    through the CPU. OpenGL's window origin is bottom-left. The single
    conversion is on the first line of `main()` and there is nowhere else in the
    repo that thinks about which way up anything is.
*/

/// The vertex shader. A full-screen triangle from `gl_VertexID` — no buffers,
/// no VAO contents, nothing to bind. A triangle rather than a quad because the
/// two-triangle version rasterises the diagonal twice.
extern const char* const kVertexShader;

/// Build the fragment shader. `isEffect` compiles in the input-clip sampling
/// and the Mix control; the source build has neither and would otherwise carry
/// a dead sampler that some drivers warn about and others optimise away
/// inconsistently, changing the uniform locations between the two builds.
std::string FragmentShader( bool isEffect );

} // namespace burin
