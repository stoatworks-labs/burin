#include "Shaders.h"

namespace burin
{
const char* const kVertexShader = R"(#version 410 core

// A full-screen triangle straight out of gl_VertexID. Nothing is bound and
// nothing is uploaded; a VAO still has to exist because a core profile insists
// on one, but it stays empty.
//
// One triangle and not two: a quad rasterises its shared diagonal twice, and
// the fragment shader here does real work per pixel.
void main()
{
	vec2 p = vec2( ( gl_VertexID << 1 ) & 2, gl_VertexID & 2 );
	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );
}
)";

std::string FragmentShader( bool isEffect )
{
	std::string source = R"(#version 410 core

out vec4 FragColour;

uniform sampler2D Raster;

// The inverse map, frame pixels -> document units, as SVG's [a b c d e f].
uniform vec2 InvA;// a, b
uniform vec2 InvC;// c, d
uniform vec2 InvE;// e, f

uniform vec2 FrameSize;

// Where the raster sits in document space, and how big it is in texels.
uniform vec2 CoverMin;
uniform vec2 CoverMax;
uniform vec2 RasterSize;
uniform float RasterScale;

uniform vec3 Tint;
uniform float Opacity;
uniform vec3 Background;
uniform float BackgroundOpacity;
)";

	if( isEffect )
		source += R"(
uniform sampler2D Input;
uniform vec2 InputMaxUV;
uniform float Mix;
)";

	source += R"(
void main()
{
	// The ONE flip. Everything on the CPU has y pointing down, like SVG;
	// OpenGL's window origin is bottom-left. Nothing else in this repo has to
	// know which way up it is.
	vec2 frame = vec2( gl_FragCoord.x, FrameSize.y - gl_FragCoord.y );

	// Frame pixels -> document units. Deliberately the same per-pixel inverse
	// the CPU compositor does, so the two are the same algorithm rather than
	// two ways of expressing the same intent. See Shaders.h.
	vec2 doc = InvA * frame.x + InvC * frame.y + InvE;

	vec4 sampled = vec4( 0.0 );

	// Outside the covered rectangle is TRANSPARENT, not the clamped edge texel.
	// The cover is the visible window intersected with the drawing's content
	// box, so it is routinely smaller than the frame -- and clamping there
	// would smear the raster's lit border across everything outside it.
	if( all( greaterThanEqual( doc, CoverMin ) ) && all( lessThanEqual( doc, CoverMax ) ) )
	{
		vec2 texel = ( doc - CoverMin ) * RasterScale;
		sampled = texture( Raster, texel / RasterSize );
	}

	//= mirrored in Compose.cpp -- from here to the end of main()
	vec3 rgb = sampled.rgb * Tint;
	float a  = sampled.a * Opacity;

	// Straight alpha throughout, so this is the over-operator written out
	// rather than a lerp. Premultiplying earlier would make the tint act on
	// rgb * a and darken every soft edge in the drawing.
	float ba = clamp( BackgroundOpacity, 0.0, 1.0 );
	if( ba > 0.0 )
	{
		float outA = a + ba * ( 1.0 - a );
		if( outA > 0.0 )
			rgb = ( rgb * a + Background * ba * ( 1.0 - a ) ) / outA;
		a = outA;
	}
)";

	if( isEffect )
		source += R"(
	// MaxUV is the fraction of the input texture the host actually drew into.
	// The rest is undrawn padding, and a fetch that reached it would take a
	// column of black down one edge of the frame.
	vec4 clip = texture( Input, ( frame / FrameSize ) * InputMaxUV );

	// The host hands over premultiplied; the arithmetic below is straight, so
	// it is un-multiplied on the way in and multiplied again at the end.
	float ia = clip.a;
	vec3 irgb = ia > 0.0 ? clip.rgb / ia : vec3( 0.0 );

	float overA = a * clamp( Mix, 0.0, 1.0 );
	float outA  = overA + ia * ( 1.0 - overA );
	if( outA > 0.0 )
		rgb = ( rgb * overA + irgb * ia * ( 1.0 - overA ) ) / outA;
	a = outA;
)";

	source += R"(
	// The premultiply, on the last line, because FFGL composites premultiplied
	// and nothing before this point should have been scaled by alpha.
	FragColour = vec4( rgb * a, a );
}
)";

	return source;
}

} // namespace burin
