#include "Compose.h"

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

uint8_t ToByte( float v )
{
	return static_cast< uint8_t >( Clamp01( v ) * 255.0f + 0.5f );
}
} // namespace

void Frame::Resize( int w, int h )
{
	width  = std::max( w, 0 );
	height = std::max( h, 0 );
	rgba.assign( static_cast< size_t >( width ) * static_cast< size_t >( height ) * 4u, 0 );
}

void SampleRaster( const std::vector< uint8_t >& pixels, const RasterPlacement& placement,
                   float docX, float docY, float outRGBA[ 4 ] )
{
	outRGBA[ 0 ] = outRGBA[ 1 ] = outRGBA[ 2 ] = outRGBA[ 3 ] = 0.0f;

	if( placement.width <= 0 || placement.height <= 0 || pixels.empty() )
		return;

	// Outside the covered rectangle is TRANSPARENT, not the nearest texel.
	//
	// This is not the usual choice and it is not interchangeable with clamping.
	// The cover is the visible window intersected with the drawing's own
	// content box, so it is routinely *smaller* than the frame — point the
	// plugin at a mark in the corner of a large artboard and the raster covers
	// the mark and nothing else. Clamping there would take the raster's edge
	// texel, which is lit, and smear it across every pixel of the frame outside
	// the cover: the drawing would appear to be a full-frame wash of its own
	// border colour. Raster.cpp pads the content clip by a few texels so there
	// is a transparent border to meet this at, and the two together make the
	// join continuous.
	if( docX < placement.cover.minx || docX > placement.cover.maxx ||
	    docY < placement.cover.miny || docY > placement.cover.maxy )
		return;

	// Document units -> raster pixels. The raster's origin is the cover
	// rectangle's top-left corner, at the snapped scale.
	const float px = ( docX - placement.cover.minx ) * placement.scale;
	const float py = ( docY - placement.cover.miny ) * placement.scale;

	// Texel centres are at half-integers, so a sample at pixel centre p reads
	// texel floor(p). Subtracting the half here rather than adding it later is
	// what puts the interpolation weights on the right pair of texels; getting
	// it wrong shifts the whole picture half a pixel, which is invisible until
	// it is compared against the CPU-composited OFX build and they disagree by
	// exactly that.
	const float fx = px - 0.5f;
	const float fy = py - 0.5f;

	// The half-texel clamp. Outside the covered rectangle is transparent black,
	// and a sample that reached it would draw a dark fringe down the edge of
	// the window -- in the middle of the picture, and moving as the pan moves.
	const float maxX = static_cast< float >( placement.width ) - 1.0f;
	const float maxY = static_cast< float >( placement.height ) - 1.0f;

	const float cx = std::min( std::max( fx, 0.0f ), maxX );
	const float cy = std::min( std::max( fy, 0.0f ), maxY );

	const int x0 = static_cast< int >( std::floor( cx ) );
	const int y0 = static_cast< int >( std::floor( cy ) );
	const int x1 = std::min( x0 + 1, placement.width - 1 );
	const int y1 = std::min( y0 + 1, placement.height - 1 );

	const float tx = cx - static_cast< float >( x0 );
	const float ty = cy - static_cast< float >( y0 );

	const size_t stride = static_cast< size_t >( placement.width ) * 4u;
	const uint8_t* p00  = &pixels[ static_cast< size_t >( y0 ) * stride + static_cast< size_t >( x0 ) * 4u ];
	const uint8_t* p10  = &pixels[ static_cast< size_t >( y0 ) * stride + static_cast< size_t >( x1 ) * 4u ];
	const uint8_t* p01  = &pixels[ static_cast< size_t >( y1 ) * stride + static_cast< size_t >( x0 ) * 4u ];
	const uint8_t* p11  = &pixels[ static_cast< size_t >( y1 ) * stride + static_cast< size_t >( x1 ) * 4u ];

	for( int c = 0; c < 4; ++c )
	{
		const float a = static_cast< float >( p00[ c ] ) + ( static_cast< float >( p10[ c ] ) - static_cast< float >( p00[ c ] ) ) * tx;
		const float b = static_cast< float >( p01[ c ] ) + ( static_cast< float >( p11[ c ] ) - static_cast< float >( p01[ c ] ) ) * tx;
		outRGBA[ c ]  = ( a + ( b - a ) * ty ) * ( 1.0f / 255.0f );
	}
}

void ComposeFrame( Frame& frame, const std::vector< uint8_t >& pixels,
                   const RasterPlacement& placement, const Transform2D& inverse,
                   const ComposeSettings& settings, const Frame* input )
{
	if( !frame.Valid() )
		return;

	const bool haveInput = ( input != nullptr && input->Valid() &&
	                         input->width == frame.width && input->height == frame.height );

	for( int y = 0; y < frame.height; ++y )
	{
		for( int x = 0; x < frame.width; ++x )
		{
			// Pixel centre, not corner. The transform maps a continuous plane,
			// and sampling at the corner offsets the whole picture by half a
			// pixel against the GPU, which rasterises to centres.
			float docX = 0.0f, docY = 0.0f;
			inverse.Apply( static_cast< float >( x ) + 0.5f, static_cast< float >( y ) + 0.5f, docX, docY );

			float sample[ 4 ];
			SampleRaster( pixels, placement, docX, docY, sample );

			//= mirrored in Shaders.cpp -- from here to the end of the loop
			float r = sample[ 0 ] * settings.tintR;
			float g = sample[ 1 ] * settings.tintG;
			float b = sample[ 2 ] * settings.tintB;
			float a = sample[ 3 ] * settings.opacity;

			// Background under the drawing. Straight alpha throughout, so this
			// is the standard over-operator written out rather than a lerp.
			const float ba = Clamp01( settings.backOpacity );
			if( ba > 0.0f )
			{
				const float outA = a + ba * ( 1.0f - a );
				if( outA > 0.0f )
				{
					r = ( r * a + settings.backR * ba * ( 1.0f - a ) ) / outA;
					g = ( g * a + settings.backG * ba * ( 1.0f - a ) ) / outA;
					b = ( b * a + settings.backB * ba * ( 1.0f - a ) ) / outA;
				}
				a = outA;
			}

			if( haveInput )
			{
				// `Frame` is straight alpha by definition, so there is nothing
				// to undo here. The shader's twin of this block starts by
				// un-multiplying, because FFGL hands the clip over
				// premultiplied — that difference is a conversion at the
				// boundary, not a difference in the arithmetic, and both sides
				// run the over-operator on straight values.
				const size_t i  = ( static_cast< size_t >( y ) * input->width + static_cast< size_t >( x ) ) * 4u;
				const float ir  = input->rgba[ i + 0 ] * ( 1.0f / 255.0f );
				const float ig  = input->rgba[ i + 1 ] * ( 1.0f / 255.0f );
				const float ib  = input->rgba[ i + 2 ] * ( 1.0f / 255.0f );
				const float ia  = input->rgba[ i + 3 ] * ( 1.0f / 255.0f );
				const float mix = Clamp01( settings.mix );

				// Over the clip, then crossfaded by Mix -- so Mix at 0 is the
				// clip untouched and at 1 is the drawing composited over it.
				const float overA = a * mix;
				const float outA  = overA + ia * ( 1.0f - overA );
				if( outA > 0.0f )
				{
					r = ( r * overA + ir * ia * ( 1.0f - overA ) ) / outA;
					g = ( g * overA + ig * ia * ( 1.0f - overA ) ) / outA;
					b = ( b * overA + ib * ia * ( 1.0f - overA ) ) / outA;
				}
				a = outA;
			}

			const size_t o        = ( static_cast< size_t >( y ) * frame.width + static_cast< size_t >( x ) ) * 4u;
			frame.rgba[ o + 0 ]   = ToByte( r );
			frame.rgba[ o + 1 ]   = ToByte( g );
			frame.rgba[ o + 2 ]   = ToByte( b );
			frame.rgba[ o + 3 ]   = ToByte( a );
		}
	}
}

} // namespace burin
