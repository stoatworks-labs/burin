/**
    burintest — the offline harness.

    Where this plugin is verified rather than previewed. Everything here drives
    the **real** classes: the same Document, Style, Reveal, Motion, Raster and
    Compose that the shipped bundle links, with no test doubles anywhere.

    The central claim of the plugin — "zooming in stays sharp, because the
    drawing is re-rasterised at the size it is being seen at" — is not asserted
    here, it is **measured**. `--crisp` renders a hard edge at six zoom levels,
    measures the width of its transition in device pixels, and requires it to
    stay put. It then does the same measurement against a raster deliberately
    built once at 1x and scaled up, which is what the plugin would be if the
    ladder were removed, and requires *that* to blow up. A test that cannot fail
    proves nothing, so the control is part of the test rather than a note.
*/

#include "Compose.h"
#include "Controls.h"
#include "Document.h"
#include "Motion.h"
#include "Raster.h"
#include "Burin.h"
#include "Reveal.h"
#include "Style.h"

#include <OpenGL/CGLCurrent.h>
#include <OpenGL/CGLTypes.h>
#include <OpenGL/OpenGL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <zlib.h>

using namespace burin;

namespace
{
int g_failures = 0;

void Check( bool ok, const std::string& what )
{
	std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++g_failures;
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool WritePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );// bit depth
	header.push_back( 6 );// colour type: RGBA
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Synthetic documents, so the tests do not depend on anyone's artwork.
//---------------------------------------------------------------------------

/// A half-plane with its edge exactly at the document centre. Zooming about the
/// centre leaves that edge at the centre of the frame at every zoom, which is
/// what lets one measurement be compared across six of them.
const char* kEdgeSvg =
	"<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
	"<rect x='50' y='0' width='50' height='100' fill='#000000'/>"
	"</svg>";

/// Two shapes, one filled only, one stroked only, plus one with both — the
/// document `--style` needs to tell the three Draw modes apart on.
const char* kStyleSvg =
	"<svg xmlns='http://www.w3.org/2000/svg' width='120' height='40'>"
	"<rect x='5'  y='5' width='30' height='30' fill='#ff0000'/>"
	"<rect x='45' y='5' width='30' height='30' fill='none' stroke='#00ff00' stroke-width='6'/>"
	"<rect x='85' y='5' width='30' height='30' fill='#0000ff' stroke='#ffffff' stroke-width='6'/>"
	"</svg>";

/// One long straight line, so a reveal's drawn length can be measured directly
/// off the raster rather than inferred.
const char* kLineSvg =
	"<svg xmlns='http://www.w3.org/2000/svg' width='100' height='20'>"
	"<path d='M 0 10 L 100 10' stroke='#000000' stroke-width='4' fill='none'/>"
	"</svg>";

//---------------------------------------------------------------------------
// Measurement
//---------------------------------------------------------------------------

/// Width, in pixels, of the alpha transition along row `y` of `frame`, measured
/// between the 10% and 90% crossings. The number that says whether an edge is
/// sharp: a correctly resolved edge is about one pixel whatever the zoom, and a
/// magnified bitmap's grows in proportion to the magnification.
float EdgeWidth( const Frame& frame, int y )
{
	if( !frame.Valid() || y < 0 || y >= frame.height )
		return -1.0f;

	const uint8_t* row = &frame.rgba[ static_cast< size_t >( y ) * frame.width * 4 ];

	auto alphaAt = [ & ]( int x ) { return row[ static_cast< size_t >( x ) * 4 + 3 ] / 255.0f; };

	// Walk out from the centre to find the transition, so a document with more
	// than one edge in it does not have its answer decided by whichever came
	// first from the left.
	int centre = frame.width / 2;

	int lo = -1, hi = -1;
	for( int x = 1; x < frame.width; ++x )
	{
		const float a = alphaAt( x );
		const float p = alphaAt( x - 1 );
		if( ( p < 0.1f && a >= 0.1f ) || ( p >= 0.1f && a < 0.1f ) )
			if( lo < 0 || std::abs( x - centre ) < std::abs( lo - centre ) )
				lo = x;
	}
	for( int x = 1; x < frame.width; ++x )
	{
		const float a = alphaAt( x );
		const float p = alphaAt( x - 1 );
		if( ( p < 0.9f && a >= 0.9f ) || ( p >= 0.9f && a < 0.9f ) )
			if( hi < 0 || std::abs( x - centre ) < std::abs( hi - centre ) )
				hi = x;
	}

	if( lo < 0 || hi < 0 )
		return -1.0f;

	return static_cast< float >( std::abs( hi - lo ) ) + 1.0f;
}

/// Fraction of `frame` that is not transparent. The measurement `--style` and
/// `--reveal` both work from: how much ink is on the page.
float InkFraction( const Frame& frame )
{
	if( !frame.Valid() )
		return 0.0f;
	size_t lit = 0;
	const size_t n = static_cast< size_t >( frame.width ) * frame.height;
	for( size_t i = 0; i < n; ++i )
		if( frame.rgba[ i * 4 + 3 ] > 8 )
			++lit;
	return static_cast< float >( lit ) / static_cast< float >( n );
}

/// Rightmost lit column, as a fraction of the frame width. How far a write-on
/// has got, measured off the picture.
float InkReach( const Frame& frame )
{
	if( !frame.Valid() )
		return 0.0f;
	int furthest = -1;
	for( int y = 0; y < frame.height; ++y )
		for( int x = frame.width - 1; x > furthest; --x )
			if( frame.rgba[ ( static_cast< size_t >( y ) * frame.width + x ) * 4 + 3 ] > 8 )
			{
				furthest = x;
				break;
			}
	return ( furthest < 0 ) ? 0.0f : static_cast< float >( furthest + 1 ) / static_cast< float >( frame.width );
}

//---------------------------------------------------------------------------
// A render, end to end, with no GL involved.
//---------------------------------------------------------------------------
struct Setup
{
	RasterRequest request;
	ComposeSettings compose;
};

Setup DefaultSetup( int w, int h )
{
	Setup s;
	s.request.frameWidth  = w;
	s.request.frameHeight = h;
	s.request.bounds      = Bounds::Viewport;
	s.request.fit         = Fit::Fit;
	s.request.detail      = 1.0f;
	s.request.reveal.mode = RevealMode::None;
	return s;
}

void Render( const Document& doc, Rasteriser& rast, RevealPlan& plan,
             const Setup& setup, Frame& out )
{
	out.Resize( setup.request.frameWidth, setup.request.frameHeight );

	const Rect box = ( setup.request.bounds == Bounds::Content ) ? doc.Content() : doc.Viewport();
	float baseScale = 1.0f;
	const Transform2D fwd = BuildTransform( setup.request, box, baseScale );

	rast.Build( doc, setup.request, plan );
	ComposeFrame( out, rast.Pixels(), rast.Placement(), fwd.Inverse(), setup.compose, nullptr );
}

//---------------------------------------------------------------------------
// Tests
//---------------------------------------------------------------------------

int TestLadder()
{
	std::printf( "ladder\n" );

	// The property the whole crispness claim rests on. Checked densely rather
	// than at a handful of points, because the failure it guards against is an
	// off-by-a-hair in the epsilon before the ceil, which would show up at the
	// rungs and nowhere else.
	bool alwaysUp = true;
	float worst   = 1.0f;
	for( int i = -2000; i <= 2000; ++i )
	{
		const float s       = std::exp2( static_cast< float >( i ) / 200.0f );
		const float snapped = SnapScale( s );
		if( snapped < s )
			alwaysUp = false;
		worst = std::max( worst, snapped / s );
	}
	Check( alwaysUp, "SnapScale never returns less than it was given" );

	// Half-octave rungs, so the worst overshoot is sqrt(2). Anything larger
	// means the ladder is coarser than documented and the memory cost with it.
	Check( worst <= 1.4143f, "worst-case overshoot is at most sqrt(2) (got " + std::to_string( worst ) + ")" );

	// 1.0 is a rung, and an un-zoomed drawing sits on it. If the epsilon before
	// the ceil is wrong this returns 1.41 and the most ordinary configuration
	// there is silently costs twice the pixels.
	Check( std::fabs( SnapScale( 1.0f ) - 1.0f ) < 1.0e-5f, "exactly 1.0 stays on its rung, not the next one" );
	Check( std::fabs( SnapScale( 2.0f ) - 2.0f ) < 1.0e-5f, "exactly 2.0 stays on its rung" );
	Check( std::fabs( SnapScale( 0.25f ) - 0.25f ) < 1.0e-5f, "exactly 0.25 stays on its rung" );

	// An extra rung is half an octave more.
	Check( std::fabs( SnapScale( 1.0f, 1 ) - std::sqrt( 2.0f ) ) < 1.0e-4f, "one extra rung is sqrt(2)" );

	// Degenerate input must not propagate. A zoom slider at the bottom of a
	// Stretch fit really can produce a zero here.
	Check( SnapScale( 0.0f ) > 0.0f, "a scale of zero is survived" );
	Check( std::isfinite( SnapScale( std::nanf( "" ) ) ), "a NaN scale is survived" );

	return 0;
}

int TestCrisp( bool writeImages )
{
	std::printf( "crisp — the central claim, measured\n" );

	Document doc;
	if( !doc.LoadText( kEdgeSvg, "edge.svg" ) )
	{
		Check( false, "built-in edge document loads" );
		return 1;
	}

	const int kFrame           = 512;
	const float zooms[]        = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
	const int kZoomCount       = static_cast< int >( sizeof( zooms ) / sizeof( zooms[ 0 ] ) );

	// --- the plugin as it ships -------------------------------------------
	float liveWidths[ kZoomCount ];
	{
		Rasteriser rast;
		RevealPlan plan;
		for( int i = 0; i < kZoomCount; ++i )
		{
			Setup s              = DefaultSetup( kFrame, kFrame );
			s.request.motion.zoom = zooms[ i ];

			Frame frame;
			Render( doc, rast, plan, s, frame );
			liveWidths[ i ] = EdgeWidth( frame, kFrame / 2 );

			if( writeImages )
			{
				char path[ 256 ];
				std::snprintf( path, sizeof( path ), "/tmp/burintest-crisp-live-%02dx.png", static_cast< int >( zooms[ i ] ) );
				WritePng( path, frame.width, frame.height, frame.rgba );
			}
		}
	}

	// --- the control: one raster built at 1x, then magnified ---------------
	// This is what the plugin would be without the ladder, and it is here so
	// that "the edge stayed sharp" is a result rather than a restatement of
	// what the code was written to do.
	float frozenWidths[ kZoomCount ];
	{
		Rasteriser rast;
		RevealPlan plan;

		Setup base              = DefaultSetup( kFrame, kFrame );
		base.request.motion.zoom = 1.0f;
		rast.Build( doc, base.request, plan );

		const RasterPlacement frozen = rast.Placement();
		const std::vector< uint8_t > frozenPixels = rast.Pixels();

		for( int i = 0; i < kZoomCount; ++i )
		{
			Setup s              = DefaultSetup( kFrame, kFrame );
			s.request.motion.zoom = zooms[ i ];

			float unusedBase      = 1.0f;
			const Transform2D fwd = BuildTransform( s.request, doc.Viewport(), unusedBase );

			Frame frame;
			frame.Resize( kFrame, kFrame );
			ComposeFrame( frame, frozenPixels, frozen, fwd.Inverse(), s.compose, nullptr );
			frozenWidths[ i ] = EdgeWidth( frame, kFrame / 2 );

			if( writeImages )
			{
				char path[ 256 ];
				std::snprintf( path, sizeof( path ), "/tmp/burintest-crisp-frozen-%02dx.png", static_cast< int >( zooms[ i ] ) );
				WritePng( path, frame.width, frame.height, frame.rgba );
			}
		}
	}

	std::printf( "    zoom   re-rasterised   frozen at 1x\n" );
	for( int i = 0; i < kZoomCount; ++i )
		std::printf( "    %4.0fx   %8.1f px   %8.1f px\n", zooms[ i ], liveWidths[ i ], frozenWidths[ i ] );

	bool liveFlat = true;
	for( int i = 0; i < kZoomCount; ++i )
		if( !( liveWidths[ i ] > 0.0f && liveWidths[ i ] <= 3.0f ) )
			liveFlat = false;

	Check( liveFlat, "re-rasterised edge stays under 3 px at every zoom to 32x" );

	// The control has to actually blow up, or the test above is measuring
	// nothing. At 32x a raster built for 1x has each of its pixels covering 32,
	// so the transition should be tens of pixels wide.
	//
	// The `> 0` is not defensive padding: EdgeWidth returns -1 when it finds no
	// transition at all, and -1 > 8 * -1 is true. An earlier version of this
	// check reported a comfortable pass while every single frame was blank.
	const float lastLive   = liveWidths[ kZoomCount - 1 ];
	const float lastFrozen = frozenWidths[ kZoomCount - 1 ];
	Check( lastLive > 0.0f && lastFrozen > 0.0f && lastFrozen > 8.0f * lastLive,
	       "control: an un-rebuilt raster is at least 8x softer at 32x (got " +
	           std::to_string( lastFrozen ) + " px vs " + std::to_string( lastLive ) + " px)" );

	return 0;
}

int TestRebuilds()
{
	std::printf( "rebuilds — the ladder has to be seldom as well as sharp\n" );

	Document doc;
	doc.LoadText( kStyleSvg, "style.svg" );

	// A zoom sweep of six octaves. Half-octave rungs means twelve rebuilds and
	// change; anything near the frame count means the cache is not working.
	{
		Rasteriser rast;
		RevealPlan plan;
		rast.ResetCounters();

		const int frames = 600;
		for( int i = 0; i < frames; ++i )
		{
			Setup s = DefaultSetup( 960, 540 );
			s.request.motion.zoom = std::exp2( -3.0f + 6.0f * static_cast< float >( i ) / ( frames - 1 ) );
			Frame f;
			Render( doc, rast, plan, s, f );
		}

		const uint64_t n = rast.Rebuilds();
		std::printf( "    600 frames across six octaves of zoom: %llu rebuilds\n", ( unsigned long long )n );
		Check( n <= 40, "a six-octave zoom rebuilds at most 40 times in 600 frames" );
		Check( n >= 12, "and at least once per rung, so the ladder is actually being climbed" );
	}

	// A pan at a fixed zoom, inside the margin. The margin is an eighth, so a
	// pan of a quarter of a frame should cost a handful of rebuilds, not 300.
	{
		Rasteriser rast;
		RevealPlan plan;
		rast.ResetCounters();

		const int frames = 300;
		for( int i = 0; i < frames; ++i )
		{
			Setup s = DefaultSetup( 960, 540 );
			s.request.motion.zoom = 4.0f;
			s.request.motion.panX = 0.25f * std::sin( static_cast< float >( i ) * 0.02f );
			Frame f;
			Render( doc, rast, plan, s, f );
		}

		const uint64_t n = rast.Rebuilds();
		std::printf( "    300 frames of pan at 4x: %llu rebuilds\n", ( unsigned long long )n );
		Check( n <= 30, "a pan inside the margin is mostly free" );
	}

	// A still frame must not rebuild at all after the first.
	{
		Rasteriser rast;
		RevealPlan plan;
		Setup s = DefaultSetup( 960, 540 );
		Frame f;
		Render( doc, rast, plan, s, f );
		rast.ResetCounters();
		for( int i = 0; i < 100; ++i )
			Render( doc, rast, plan, s, f );
		Check( rast.Rebuilds() == 0, "a still frame never rebuilds" );
	}

	return 0;
}

int TestStyle()
{
	std::printf( "style — fill and stroke are separated exactly\n" );

	Document doc;
	doc.LoadText( kStyleSvg, "style.svg" );
	Check( doc.ShapeCount() == 3, "the style document has three shapes" );

	Rasteriser rast;
	RevealPlan plan;

	auto inkFor = [ & ]( Draw draw ) {
		Setup s            = DefaultSetup( 480, 160 );
		s.request.style.draw = draw;
		Frame f;
		Render( doc, rast, plan, s, f );
		return InkFraction( f );
	};

	const float both   = inkFor( Draw::Both );
	const float fill   = inkFor( Draw::FillOnly );
	const float stroke = inkFor( Draw::StrokeOnly );

	std::printf( "    ink: both %.4f  fill-only %.4f  stroke-only %.4f\n", both, fill, stroke );

	Check( both > fill && both > stroke, "Both draws more than either half alone" );
	Check( fill > 0.0f && stroke > 0.0f, "neither half is empty" );

	// The middle shape has no fill at all, so Fill Only must lose it entirely
	// while Stroke Only keeps it. This is the check that says the separation is
	// the file's own paint rather than something approximated from the picture.
	{
		Setup s              = DefaultSetup( 480, 160 );
		s.request.style.draw = Draw::FillOnly;
		s.request.style.firstShape = 1;
		s.request.style.shapeCount = 1;
		Frame f;
		Render( doc, rast, plan, s, f );
		Check( InkFraction( f ) == 0.0f, "the unfilled shape renders nothing under Fill Only" );
	}
	{
		Setup s              = DefaultSetup( 480, 160 );
		s.request.style.draw = Draw::StrokeOnly;
		s.request.style.firstShape = 1;
		s.request.style.shapeCount = 1;
		Frame f;
		Render( doc, rast, plan, s, f );
		Check( InkFraction( f ) > 0.0f, "and its stroke survives under Stroke Only" );
	}

	// Isolate has to actually isolate.
	{
		Setup s                    = DefaultSetup( 480, 160 );
		s.request.style.firstShape = 0;
		s.request.style.shapeCount = 1;
		Frame f;
		Render( doc, rast, plan, s, f );
		const float one = InkFraction( f );
		Check( one > 0.0f && one < both, "isolating one shape draws less than all three" );
	}

	return 0;
}

int TestReveal()
{
	std::printf( "reveal — the write-on, against Reveal.cpp\n" );

	// --- the arithmetic, with nothing rendered ----------------------------
	Check( ShapeProgress( 0, 1, 0.5f, 0.0f ) == 0.5f, "one shape, no stagger: progress passes through" );
	Check( ShapeProgress( 0, 4, 0.5f, 0.0f ) == 0.5f, "no stagger: every slot the same" );
	Check( ShapeProgress( 3, 4, 0.5f, 0.0f ) == 0.5f, "no stagger: last slot too" );

	// At a stagger of one, slot k finishes exactly as slot k+1 starts.
	{
		const int n = 4;
		const float span = 1.0f + ( n - 1 ) * 1.0f;// 4 units
		// slot 0 finishes at T = 1, which is p = 1/4
		Check( std::fabs( ShapeProgress( 0, n, 1.0f / span, 1.0f ) - 1.0f ) < 1e-5f, "stagger 1: slot 0 done at p = 1/4" );
		Check( ShapeProgress( 1, n, 1.0f / span, 1.0f ) <= 1e-5f, "stagger 1: slot 1 not started at p = 1/4" );
		Check( std::fabs( ShapeProgress( 3, n, 1.0f, 1.0f ) - 1.0f ) < 1e-5f, "stagger 1: last slot done at p = 1" );
	}

	// Everything is finished at p = 1 whatever the stagger, which is the
	// property an operator cueing a reveal against a bar actually relies on.
	for( float stagger : { 0.0f, 0.5f, 1.0f, 2.5f, 4.0f } )
		Check( std::fabs( ShapeProgress( 7, 8, 1.0f, stagger ) - 1.0f ) < 1e-5f,
		       "everything finished at p = 1 with stagger " + std::to_string( stagger ) );

	// --- measured off the picture -----------------------------------------
	Document doc;
	doc.LoadText( kLineSvg, "line.svg" );

	Rasteriser rast;
	RevealPlan plan;

	float lastReach = -1.0f;
	bool monotonic  = true;
	for( int i = 0; i <= 10; ++i )
	{
		const float p = static_cast< float >( i ) / 10.0f;

		Setup s                  = DefaultSetup( 400, 80 );
		s.request.fit            = Fit::Stretch;
		s.request.reveal.mode    = RevealMode::On;
		s.request.reveal.progress = p;

		Frame f;
		Render( doc, rast, plan, s, f );
		const float reach = InkReach( f );

		if( reach < lastReach - 0.02f )
			monotonic = false;
		lastReach = reach;

		// The drawn length should track the parameter. Tolerance is wide
		// because the flattening this measures against is not the flattening
		// the length was computed with — see Document.h.
		if( i > 0 && i < 10 )
			Check( std::fabs( reach - p ) < 0.06f,
			       "at p = " + std::to_string( p ) + " the line reaches " + std::to_string( reach ) );
	}

	Check( monotonic, "the drawn length never goes backwards as p rises" );

	// The two ends are special-cased, and they are the ones that must be exact.
	{
		Setup s                   = DefaultSetup( 400, 80 );
		s.request.fit             = Fit::Stretch;
		s.request.reveal.mode     = RevealMode::On;
		s.request.reveal.progress = 0.0f;
		Frame f;
		Render( doc, rast, plan, s, f );
		Check( InkFraction( f ) == 0.0f, "p = 0 draws nothing at all" );
	}
	{
		Setup s                   = DefaultSetup( 400, 80 );
		s.request.fit             = Fit::Stretch;
		s.request.reveal.mode     = RevealMode::On;
		s.request.reveal.progress = 1.0f;
		Frame f;
		Render( doc, rast, plan, s, f );
		const float full = InkReach( f );

		Setup off                 = DefaultSetup( 400, 80 );
		off.request.fit           = Fit::Stretch;
		off.request.reveal.mode   = RevealMode::None;
		Frame g;
		Render( doc, rast, plan, off, g );

		Check( std::fabs( full - InkReach( g ) ) < 1e-6f, "p = 1 is the same picture as no reveal at all" );
	}

	return 0;
}

int TestMotion()
{
	std::printf( "motion\n" );

	// Waveforms are all -1..1 and all pass through zero at phase zero, so
	// changing the Wave control does not move the drawing.
	for( int w = 0; w < static_cast< int >( Wave::Count ); ++w )
	{
		const Wave wave = static_cast< Wave >( w );
		float lo = 1.0f, hi = -1.0f;
		for( int i = 0; i < 1000; ++i )
		{
			const float v = WaveValue( wave, static_cast< double >( i ) / 1000.0 );
			lo            = std::min( lo, v );
			hi            = std::max( hi, v );
		}
		Check( lo >= -1.0001f && hi <= 1.0001f, "wave " + std::to_string( w ) + " stays inside -1..1" );
	}

	Check( std::fabs( WaveValue( Wave::Sine, 0.0 ) ) < 1e-5f, "sine starts at zero" );
	Check( std::fabs( WaveValue( Wave::Triangle, 0.0 ) ) < 1e-5f, "triangle starts at zero" );
	Check( std::fabs( WaveValue( Wave::Ramp, 0.5 ) ) < 1e-5f, "ramp is zero at its midpoint" );

	// Negative cycles must not flip the sawtooth into the wrong half — this is
	// what Frac() is for, and fmod would get it wrong.
	Check( WaveValue( Wave::Ramp, -0.25 ) > 0.0f, "a negative clock still ramps the same way" );

	// The bar recovery is continuous across a bar line.
	{
		const float bpm = 120.0f;// one bar is two seconds
		const double a  = MotionClock( 1.999, bpm, 0.9995f, SyncMode::Bar, 1.0f, 0.0f );
		const double b  = MotionClock( 2.001, bpm, 0.0005f, SyncMode::Bar, 1.0f, 0.0f );
		Check( std::fabs( b - a ) < 0.01, "bar sync is continuous across the bar line" );
	}

	// Manual ignores the clock entirely.
	Check( MotionClock( 999.0, 120.0f, 0.5f, SyncMode::Manual, 3.0f, 0.25f ) == 0.25,
	       "Manual reads the phase slider and nothing else" );

	// Spin accumulates rather than wrapping — a rotation rate that snapped back
	// each cycle would be a stutter, not a spin.
	{
		MotionSettings m;
		m.spin = 1.0f;
		Check( std::fabs( SolveMotion( m, 3.0 ).rotate - 3.0f * 6.28318f ) < 0.01f, "spin accumulates across cycles" );
	}

	// Zoom moves in octaves, symmetrically.
	{
		MotionSettings m;
		m.zoom     = 1.0f;
		m.zoomMove = 1.0f;
		m.wave     = Wave::Ramp;
		const float lo = SolveMotion( m, 0.0 ).zoom;  // ramp at phase 0 is -1
		const float hi = SolveMotion( m, 0.5 ).zoom;  // ramp at phase 0.5 is 0
		Check( std::fabs( lo - 0.5f ) < 1e-4f, "one octave down is half size" );
		Check( std::fabs( hi - 1.0f ) < 1e-4f, "the centre of the travel is the base zoom" );
	}

	return 0;
}

int TestDoc( const std::string& path )
{
	Document doc;
	const bool ok = path.empty() ? doc.LoadText( kStyleSvg, "built-in" ) : doc.LoadFile( path );

	std::printf( "%s\n", doc.Note().c_str() );
	if( !ok )
		return 1;

	std::printf( "  viewport  %g %g -> %g %g\n", doc.Viewport().minx, doc.Viewport().miny, doc.Viewport().maxx, doc.Viewport().maxy );
	std::printf( "  content   %g %g -> %g %g\n", doc.Content().minx, doc.Content().miny, doc.Content().maxx, doc.Content().maxy );
	std::printf( "  %-4s %-10s %-6s %-6s %s\n", "#", "length", "fill", "stroke", "bounds" );
	int i = 0;
	for( const ShapeInfo& s : doc.Shapes() )
	{
		std::printf( "  %-4d %-10.2f %-6s %-6s %.1f %.1f %.1f %.1f\n", i++, s.length,
		             s.fill.type == NSVG_PAINT_NONE ? "-" : ( s.fill.type == NSVG_PAINT_COLOR ? "flat" : "grad" ),
		             s.stroke.type == NSVG_PAINT_NONE ? "-" : ( s.stroke.type == NSVG_PAINT_COLOR ? "flat" : "grad" ),
		             s.bounds[ 0 ], s.bounds[ 1 ], s.bounds[ 2 ], s.bounds[ 3 ] );
	}
	return 0;
}

int TestCost( const std::string& path )
{
	std::printf( "cost — what a rebuild actually costs on this file\n" );

	Document doc;
	if( !( path.empty() ? doc.LoadText( kStyleSvg, "built-in" ) : doc.LoadFile( path ) ) )
	{
		std::printf( "  %s\n", doc.Note().c_str() );
		return 1;
	}
	std::printf( "  %s\n", doc.Note().c_str() );

	std::printf( "    %-12s %10s %10s   %s\n", "frame", "rasterise", "composite", "raster size" );

	for( int frameW : { 1280, 1920, 3840 } )
	{
		Rasteriser rast;
		RevealPlan plan;
		Setup s = DefaultSetup( frameW, frameW * 9 / 16 );

		const Rect box  = doc.Viewport();
		float baseScale = 1.0f;

		// The two halves are timed SEPARATELY, because only one of them is a
		// cost the shipped FFGL plugin pays. The rasterise happens on the CPU
		// in both builds; the composite is a fragment shader in Resolume and is
		// only on the CPU here and in the OpenFX build. Reporting the sum would
		// overstate the Resolume figure by roughly a third and send somebody
		// optimising the wrong half.
		const int n = 20;
		double rasterMs = 0.0, composeMs = 0.0;

		for( int i = 0; i < n; ++i )
		{
			// A moving reveal, which is the worst case the plugin actually has:
			// its pixels change every frame, so there is nothing to cache.
			s.request.reveal.mode     = RevealMode::On;
			s.request.reveal.progress = static_cast< float >( i ) / n;

			const Transform2D fwd = BuildTransform( s.request, box, baseScale );

			const auto t0 = std::chrono::steady_clock::now();
			rast.Build( doc, s.request, plan );
			const auto t1 = std::chrono::steady_clock::now();

			Frame f;
			f.Resize( s.request.frameWidth, s.request.frameHeight );
			ComposeFrame( f, rast.Pixels(), rast.Placement(), fwd.Inverse(), s.compose, nullptr );
			const auto t2 = std::chrono::steady_clock::now();

			rasterMs += std::chrono::duration< double, std::milli >( t1 - t0 ).count();
			composeMs += std::chrono::duration< double, std::milli >( t2 - t1 ).count();
		}

		char frame[ 32 ];
		std::snprintf( frame, sizeof( frame ), "%dx%d", frameW, frameW * 9 / 16 );
		std::printf( "    %-12s %7.2f ms %7.2f ms   %dx%d\n", frame, rasterMs / n, composeMs / n,
		             rast.Placement().width, rast.Placement().height );
	}

	std::printf( "  The rasterise column is what Resolume pays; the composite is a shader there.\n"
	             "  It is only paid on a REBUILD — a still frame pays nothing. See Raster.h.\n" );
	return 0;
}

int TestRender( const std::string& path, const std::string& out, float zoom, float progress )
{
	Document doc;
	if( !( path.empty() ? doc.LoadText( kStyleSvg, "built-in" ) : doc.LoadFile( path ) ) )
	{
		std::printf( "%s\n", doc.Note().c_str() );
		return 1;
	}

	Rasteriser rast;
	RevealPlan plan;
	Setup s               = DefaultSetup( 1280, 720 );
	s.request.motion.zoom = zoom;
	if( progress >= 0.0f )
	{
		s.request.reveal.mode     = RevealMode::On;
		s.request.reveal.progress = progress;
	}

	Frame f;
	Render( doc, rast, plan, s, f );

	if( !WritePng( out, f.width, f.height, f.rgba ) )
	{
		std::printf( "could not write %s\n", out.c_str() );
		return 1;
	}
	std::printf( "%s: %s  (raster %dx%d at %.3f px/unit)\n", out.c_str(), doc.Note().c_str(),
	             rast.Placement().width, rast.Placement().height, rast.Placement().scale );
	return 0;
}

//---------------------------------------------------------------------------
// GL plumbing. Everything below drives the REAL plugin class through the real
// FFGL call sequence in a headless core-profile context.
//---------------------------------------------------------------------------
CGLContextObj CreateContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target MakeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

/// Render one frame through the plugin and read it back **top row first**, to
/// match `Frame`. GL hands over bottom-up; this is the one place that flips.
void RenderPlugin( BurinPlugin& plugin, const Target& target, double seconds, Frame& out )
{
	FFGLViewportStruct viewport{};
	viewport.x      = 0;
	viewport.y      = 0;
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	plugin.SetSecondsForTest( seconds );
	plugin.InitGL( &viewport );

	ProcessOpenGLStruct pgl{};
	pgl.numInputTextures = 0;
	pgl.inputTextures    = nullptr;
	pgl.HostFBO          = target.fbo;
	plugin.ProcessOpenGL( &pgl );

	std::vector< unsigned char > bottomUp( static_cast< size_t >( target.width ) * target.height * 4 );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );

	out.Resize( target.width, target.height );
	const size_t stride = static_cast< size_t >( target.width ) * 4;
	for( int y = 0; y < target.height; ++y )
		std::memcpy( out.rgba.data() + static_cast< size_t >( y ) * stride,
		             bottomUp.data() + static_cast< size_t >( target.height - 1 - y ) * stride, stride );
}

/// The GPU writes PREMULTIPLIED — that is what FFGL composites and what the
/// shader's last line produces — while `ComposeFrame` produces straight alpha.
/// So the two have to be brought into one space before they can be compared,
/// and this brings the CPU's up rather than bringing the GPU's down.
///
/// That direction is the whole point. Un-premultiplying the GPU frame divides
/// by alpha, and at the edge of a drawing there are pixels whose alpha is one
/// or two levels out of 255: dividing by 0.004 multiplies an 8-bit rounding
/// difference by 255 and reports a disagreement of the full range on a pixel
/// nobody can see. The first version of this test did exactly that and claimed
/// the shader and the compositor were 255 levels apart while their alpha
/// channels agreed to the bit.
///
/// Premultiplied is also the honest space to compare in: it is what is actually
/// displayed, and a difference that vanishes when multiplied by alpha is a
/// difference in a colour that is not being shown.
void Premultiply( Frame& frame )
{
	const size_t n = static_cast< size_t >( frame.width ) * frame.height;
	for( size_t i = 0; i < n; ++i )
	{
		const float a = frame.rgba[ i * 4 + 3 ] / 255.0f;
		for( int c = 0; c < 3; ++c )
			frame.rgba[ i * 4 + c ] =
				static_cast< uint8_t >( ( frame.rgba[ i * 4 + c ] / 255.0f ) * a * 255.0f + 0.5f );
	}
}

int TestGpu( bool writeImages )
{
	std::printf( "gpu — the shipped plugin class, in a real GL context\n" );

	CGLContextObj context = CreateContext();
	if( context == nullptr )
	{
		Check( false, "a core-profile context could be created" );
		return 1;
	}

	BurinPlugin plugin( false );
	if( !plugin.LoadDocumentString( kStyleSvg, "style.svg" ) )
	{
		Check( false, "the built-in document loads into the plugin" );
		return 1;
	}

	Target target = MakeTarget( 480, 160 );

	Frame gpu;
	RenderPlugin( plugin, target, 0.0, gpu );
	Check( InkFraction( gpu ) > 0.0f, "the plugin draws something at all" );

	if( writeImages )
		WritePng( "/tmp/burintest-gpu.png", gpu.width, gpu.height, gpu.rgba );

	//----------------------------------------------------------------------
	// The mirror. Shaders.cpp and Compose.cpp are the only duplicated
	// arithmetic in the repo, and this is the only thing that catches them
	// drifting apart.
	//----------------------------------------------------------------------
	Frame cpu;
	{
		const Document& doc          = plugin.GetDocument();
		const RasterRequest request  = plugin.BuildRequest( target.width, target.height );
		const ComposeSettings settings = plugin.BuildCompose();

		Rasteriser rast;
		RevealPlan plan;
		rast.Build( doc, request, plan );

		const Rect box  = ( request.bounds == Bounds::Content ) ? doc.Content() : doc.Viewport();
		float baseScale = 1.0f;
		const Transform2D inv = BuildTransform( request, box, baseScale ).Inverse();

		cpu.Resize( target.width, target.height );
		ComposeFrame( cpu, rast.Pixels(), rast.Placement(), inv, settings, nullptr );
		Premultiply( cpu );
	}

	if( writeImages )
		WritePng( "/tmp/burintest-cpu.png", cpu.width, cpu.height, cpu.rgba );

	double worst = 0.0, total = 0.0;
	const size_t n = static_cast< size_t >( gpu.width ) * gpu.height;
	for( size_t i = 0; i < n; ++i )
		for( int c = 0; c < 4; ++c )
		{
			const double d = std::fabs( static_cast< double >( gpu.rgba[ i * 4 + c ] ) -
			                            static_cast< double >( cpu.rgba[ i * 4 + c ] ) );
			worst = std::max( worst, d );
			total += d;
		}

	const double mean = total / static_cast< double >( n * 4 );
	std::printf( "    GPU vs CPU composite: worst %.1f levels, mean %.3f\n", worst, mean );

	// Two 8-bit levels. The two paths do the same arithmetic in the same order,
	// but one does it in 32-bit float on a GPU and the other on a CPU, and the
	// bilinear fetch is the hardware's on one side and ours on the other.
	Check( worst <= 2.0, "GPU and CPU composites agree to within 2 levels" );
	Check( mean <= 0.1, "and agree almost exactly on average" );

	//----------------------------------------------------------------------
	// InitGL is called every frame by this harness and a host may call it
	// twice. If it is not idempotent it leaks a shader, a VAO and a texture
	// per frame -- inside Resolume.
	//----------------------------------------------------------------------
	{
		GLint before = 0, after = 0;
		glGetIntegerv( GL_MAX_TEXTURE_SIZE, &before );// touch GL so the driver settles
		for( int i = 0; i < 50; ++i )
		{
			Frame f;
			RenderPlugin( plugin, target, static_cast< double >( i ) * 0.1, f );
		}
		glGetIntegerv( GL_MAX_TEXTURE_SIZE, &after );
		Check( glGetError() == GL_NO_ERROR, "fifty frames leave no GL error behind" );
	}

	plugin.DeInitGL();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}

int TestPresets()
{
	std::printf( "presets — every one applied through the real dropdown\n" );

	CGLContextObj context = CreateContext();
	if( context == nullptr )
	{
		Check( false, "a core-profile context could be created" );
		return 1;
	}

	Target target = MakeTarget( 480, 160 );

	for( int i = 0; i < presets::kPresetCount; ++i )
	{
		BurinPlugin plugin( false );
		plugin.LoadDocumentString( kStyleSvg, "style.svg" );

		// Through SetFloatParameter, exactly as a host would, so the preset
		// machinery is what is under test rather than the table.
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( i + 1 ) );

		// Sampled ACROSS a cycle rather than at t = 0.
		//
		// A preset that turns the write-on on is legitimately empty at the
		// instant the clock starts — that is what the beginning of a draw-on
		// looks like — so requiring ink at t = 0 would be asserting something
		// false about a correct animation. It is still worth catching a preset
		// that draws nothing *ever*, which is one wrong value away at all times
		// and looks exactly like a broken plugin, so the requirement is that
		// most of the cycle has something on screen.
		// A cycle of THIS preset, not a fixed number of seconds. The rates in
		// the table span from about a twelfth of a cycle a second down to a
		// thirtieth, so a fixed window covers a full cycle of the fast presets
		// and a third of one of the slow ones — and a reveal a third of the way
		// through its first shape has, correctly, drawn nothing yet. A fixed
		// four-second window reported "Rough to Fine" as a dead preset for
		// exactly that reason.
		const double cycleSeconds = 1.0 / RateFromParam( presets::kPresets[ i ].values[ presets::kRate ] );

		const int kSamples = 8;
		int drew           = 0;
		float peak         = 0.0f;
		for( int s = 0; s < kSamples; ++s )
		{
			Frame f;
			RenderPlugin( plugin, target, cycleSeconds * static_cast< double >( s ) / kSamples, f );
			const float ink = InkFraction( f );
			peak            = std::max( peak, ink );
			if( ink > 0.001f )
				++drew;
		}

		std::printf( "    %-14s peak ink %.4f, drew in %d of %d samples\n",
		             presets::kPresets[ i ].name, peak, drew, kSamples );

		Check( drew >= kSamples / 2,
		       std::string( "preset '" ) + presets::kPresets[ i ].name + "' draws for most of a cycle" );

		// And picking one must leave the dropdown reading that preset rather
		// than flipping straight back to Custom, which is what happens if the
		// "editing a covered parameter" rule is too eager.
		Check( std::lround( plugin.GetFloatParameter( PT_PRESET ) ) == i + 1,
		       std::string( "preset '" ) + presets::kPresets[ i ].name + "' stays selected" );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}

/// Render one frame through the REAL plugin class with `--set` values applied.
///
/// This is what sweep.py drives. It matters that it goes through
/// `SetFloatParameter` by parameter *name* rather than by writing a settings
/// struct: a control that is declared, appears in the host and is never read by
/// BuildRequest is exactly the failure this catches, and a struct-level test
/// would bypass the gap.
int RenderThroughPlugin( const std::string& svgPath, const std::string& out,
                         const std::vector< std::pair< std::string, float > >& sets,
                         double seconds, int width, int height )
{
	CGLContextObj context = CreateContext();
	if( context == nullptr )
	{
		std::printf( "no GL context\n" );
		return 1;
	}

	BurinPlugin plugin( false );

	if( svgPath.empty() )
	{
		plugin.LoadDocumentString( kStyleSvg, "built-in" );
	}
	else
	{
		// Through the text parameter, as a host would, so the file-parameter
		// path is exercised rather than bypassed.
		plugin.SetTextParameter( PT_FILE, svgPath.c_str() );
	}

	for( const auto& kv : sets )
	{
		bool found = false;
		for( unsigned int id = 0; id < plugin.GetNumParams(); ++id )
		{
			const char* name = plugin.GetParamName( id );
			if( name != nullptr && kv.first == name )
			{
				plugin.SetFloatParameter( id, kv.second );
				found = true;
				break;
			}
		}
		if( !found )
		{
			std::printf( "no such parameter: %s\n", kv.first.c_str() );
			return 1;
		}
	}

	Target target = MakeTarget( width, height );
	Frame frame;
	RenderPlugin( plugin, target, seconds, frame );

	const bool ok = WritePng( out, frame.width, frame.height, frame.rgba );

	plugin.DeInitGL();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	if( !ok )
	{
		std::printf( "could not write %s\n", out.c_str() );
		return 1;
	}
	return 0;
}

//---------------------------------------------------------------------------
// --sequence
//---------------------------------------------------------------------------
//
// A cue sheet, so the project video is the real plugin being *operated* rather
// than a mock-up or a screen recording:
//
//     12.0        Draw=2               set at a time
//     4.0..9.0    Zoom=0.5..0.82       ramp between two times
//
// Times are seconds on the video's own clock, which is also the host clock
// handed to the plugin — so a cue at 12s is the frame you see at 12s.
//
// The clock is deliberately NOT pinned beyond that. The plugin free-runs off
// the host clock exactly as it does in Resolume, which is the only way footage
// can honestly show Rate, Wave and the reveal doing anything.
//
struct Cue
{
	double from = 0.0;
	double to   = 0.0;
	std::string name;
	float first  = 0.0f;
	float second = 0.0f;
	bool ramp    = false;
};

bool ParseCues( const std::string& path, std::vector< Cue >& cues )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
	{
		std::fprintf( stderr, "cannot open cue sheet %s\n", path.c_str() );
		return false;
	}

	char line[ 1024 ];
	int number = 0;
	while( std::fgets( line, sizeof( line ), file ) != nullptr )
	{
		++number;
		std::string text = line;

		const size_t hash = text.find( '#' );
		if( hash != std::string::npos )
			text = text.substr( 0, hash );

		const size_t firstReal = text.find_first_not_of( " \t\r\n" );
		if( firstReal == std::string::npos )
			continue;
		text = text.substr( firstReal );

		const size_t split = text.find_first_of( " \t" );
		if( split == std::string::npos )
			continue;

		const std::string when = text.substr( 0, split );
		std::string assignment = text.substr( split );

		const size_t assignStart = assignment.find_first_not_of( " \t" );
		if( assignStart == std::string::npos )
			continue;
		assignment = assignment.substr( assignStart );
		while( !assignment.empty() && std::strchr( " \t\r\n", assignment.back() ) != nullptr )
			assignment.pop_back();

		Cue cue;
		const size_t timeRange = when.find( ".." );
		if( timeRange != std::string::npos )
		{
			cue.from = std::strtod( when.substr( 0, timeRange ).c_str(), nullptr );
			cue.to   = std::strtod( when.substr( timeRange + 2 ).c_str(), nullptr );
			cue.ramp = true;
		}
		else
		{
			cue.from = cue.to = std::strtod( when.c_str(), nullptr );
		}

		const size_t equals = assignment.find( '=' );
		if( equals == std::string::npos )
		{
			std::fprintf( stderr, "%s:%d: expected Name=value\n", path.c_str(), number );
			std::fclose( file );
			return false;
		}

		cue.name                = assignment.substr( 0, equals );
		const std::string value = assignment.substr( equals + 1 );

		const size_t valueRange = value.find( ".." );
		if( cue.ramp && valueRange != std::string::npos )
		{
			cue.first  = std::strtof( value.substr( 0, valueRange ).c_str(), nullptr );
			cue.second = std::strtof( value.substr( valueRange + 2 ).c_str(), nullptr );
		}
		else
		{
			cue.first = cue.second = std::strtof( value.c_str(), nullptr );
			cue.ramp  = false;
		}

		cues.push_back( cue );
	}

	std::fclose( file );
	return true;
}

int RenderSequence( const std::string& directory, const std::string& cuePath,
                    const std::string& svgPath, int width, int height,
                    double seconds, double fps )
{
	std::vector< Cue > cues;
	if( !cuePath.empty() && !ParseCues( cuePath, cues ) )
		return 1;

	CGLContextObj context = CreateContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "no GL context\n" );
		return 1;
	}

	BurinPlugin plugin( false );
	if( svgPath.empty() )
		plugin.LoadDocumentString( kStyleSvg, "built-in" );
	else
		plugin.SetTextParameter( PT_FILE, svgPath.c_str() );

	// Every cue is checked against the real parameter list before a single
	// frame is rendered. A typo in a name would otherwise be a cue that
	// silently never fires, and the only symptom would be a video that is
	// subtly less interesting than the sheet says it is — discovered after the
	// whole render.
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < plugin.GetNumParams(); ++id )
	{
		const char* n = plugin.GetParamName( id );
		if( n != nullptr )
			byName[ n ] = id;
	}
	for( const Cue& cue : cues )
	{
		if( byName.find( cue.name ) == byName.end() )
		{
			std::fprintf( stderr, "cue names '%s', which is not a parameter\n", cue.name.c_str() );
			return 1;
		}
	}

	Target target   = MakeTarget( width, height );
	const int frames = static_cast< int >( seconds * fps + 0.5 );

	for( int frame = 0; frame < frames; ++frame )
	{
		const double now = static_cast< double >( frame ) / fps;

		// Applied in file order every frame rather than tracked as state, so a
		// later cue on the same parameter simply wins — which is what reading
		// the sheet top to bottom would lead you to expect.
		for( const Cue& cue : cues )
		{
			if( now < cue.from )
				continue;

			float value = cue.second;
			if( cue.ramp && now < cue.to && cue.to > cue.from )
			{
				const double t = ( now - cue.from ) / ( cue.to - cue.from );
				// Smoothstep rather than linear. A parameter that starts and
				// stops abruptly reads as a jump cut even when the value in
				// between is right.
				const double eased = t * t * ( 3.0 - 2.0 * t );
				value = static_cast< float >( cue.first + ( cue.second - cue.first ) * eased );
			}

			plugin.SetFloatParameter( byName.at( cue.name ), value );
		}

		Frame f;
		RenderPlugin( plugin, target, now, f );

		char path[ 1024 ];
		std::snprintf( path, sizeof( path ), "%s/frame%05d.png", directory.c_str(), frame );
		if( !WritePng( path, f.width, f.height, f.rgba ) )
		{
			std::fprintf( stderr, "could not write %s\n", path );
			return 1;
		}

		if( ( frame + 1 ) % 60 == 0 )
			std::printf( "  %d / %d frames\n", frame + 1, frames );
	}

	plugin.DeInitGL();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	std::printf( "%d frames -> %s\n", frames, directory.c_str() );
	return 0;
}

int ListParams()
{
	BurinPlugin plugin( false );
	std::printf( "%-4s %-20s %-10s %-10s %s\n", "id", "name", "type", "default", "range" );
	for( unsigned int id = 0; id < plugin.GetNumParams(); ++id )
	{
		const char* name        = plugin.GetParamName( id );
		const unsigned int type = plugin.GetParamType( id );
		const RangeStruct range = plugin.GetParamRange( id );
		std::printf( "%-4u %-20s %-10u %-10.4f %g..%g\n", id, name ? name : "?", type,
		             plugin.GetFloatParameter( id ), range.min, range.max );
	}
	return 0;
}

void Usage()
{
	std::printf(
		"burintest — the burin harness\n"
		"\n"
		"  --all                     every check below\n"
		"  --ladder                  the scale ladder's snap-up property\n"
		"  --crisp [--images]        THE test: edge sharpness across zoom, with a control\n"
		"  --rebuilds                that the cache is seldom as well as sharp\n"
		"  --style                   fill/stroke separation and isolate\n"
		"  --reveal                  write-on timing, against Reveal.cpp\n"
		"  --motion                  clock, waveforms, spin, zoom octaves\n"
		"  --gpu [--images]          the shipped class in real GL, and the CPU/GPU mirror\n"
		"  --presets                 every factory preset, applied through the dropdown\n"
		"\n"
		"  --list                    parameters, with types, defaults and ranges\n"
		"  --frame out.png           render through the real plugin class:\n"
		"                            [--file f.svg] [--set \"Zoom=0.8\"]... [--time s] [--size WxH]\n"
		"  --doc [file.svg]          what a document parses to\n"
		"  --cost [file.svg]         milliseconds per rebuild at three frame sizes\n"
		"  --render out.png [--file f.svg] [--zoom z] [--progress p]\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string file, out;
	float zoom     = 1.0f;
	float progress = -1.0f;
	bool images    = false;
	double seconds = 0.0;
	int width = 480, height = 270;
	std::vector< std::pair< std::string, float > > sets;
	std::string script;
	double duration = 24.0, fps = 30.0;

	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		if( a == "--file" && i + 1 < argc )
			file = argv[ ++i ];
		else if( a == "--zoom" && i + 1 < argc )
			zoom = std::strtof( argv[ ++i ], nullptr );
		else if( a == "--progress" && i + 1 < argc )
			progress = std::strtof( argv[ ++i ], nullptr );
		else if( a == "--time" && i + 1 < argc )
			seconds = std::strtod( argv[ ++i ], nullptr );
		else if( a == "--size" && i + 1 < argc )
		{
			const std::string s = argv[ ++i ];
			const size_t x      = s.find( 'x' );
			if( x != std::string::npos )
			{
				width  = std::atoi( s.substr( 0, x ).c_str() );
				height = std::atoi( s.substr( x + 1 ).c_str() );
			}
		}
		else if( a == "--set" && i + 1 < argc )
		{
			const std::string s = argv[ ++i ];
			const size_t eq     = s.find( '=' );
			if( eq != std::string::npos )
				sets.emplace_back( s.substr( 0, eq ), std::strtof( s.substr( eq + 1 ).c_str(), nullptr ) );
		}
		else if( a == "--script" && i + 1 < argc )
			script = argv[ ++i ];
		else if( a == "--seconds" && i + 1 < argc )
			duration = std::strtod( argv[ ++i ], nullptr );
		else if( a == "--fps" && i + 1 < argc )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( a == "--images" )
			images = true;
	}

	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];

		if( a == "--help" || a == "-h" )
		{
			Usage();
			return 0;
		}
		if( a == "--ladder" )
			TestLadder();
		else if( a == "--crisp" )
			TestCrisp( images );
		else if( a == "--rebuilds" )
			TestRebuilds();
		else if( a == "--style" )
			TestStyle();
		else if( a == "--reveal" )
			TestReveal();
		else if( a == "--motion" )
			TestMotion();
		else if( a == "--gpu" )
			TestGpu( images );
		else if( a == "--presets" )
			TestPresets();
		else if( a == "--list" )
			return ListParams();
		else if( a == "--doc" )
			return TestDoc( file );
		else if( a == "--cost" )
			return TestCost( file );
		else if( a == "--render" && i + 1 < argc )
			return TestRender( file, argv[ ++i ], zoom, progress );
		else if( a == "--frame" && i + 1 < argc )
			return RenderThroughPlugin( file, argv[ ++i ], sets, seconds, width, height );
		else if( a == "--sequence" && i + 1 < argc )
			return RenderSequence( argv[ ++i ], script, file, width, height, duration, fps );
		else if( a == "--all" )
		{
			TestLadder();
			TestCrisp( images );
			TestRebuilds();
			TestStyle();
			TestReveal();
			TestMotion();
			TestGpu( images );
			TestPresets();
		}
	}

	if( argc == 1 )
	{
		Usage();
		return 0;
	}

	if( g_failures > 0 )
		std::printf( "\n%d FAILED\n", g_failures );
	else
		std::printf( "\nall checks passed\n" );

	return g_failures > 0 ? 1 : 0;
}
