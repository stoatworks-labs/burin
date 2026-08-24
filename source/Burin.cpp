#include "Burin.h"

#include "Diag.h"
#include "Settings.h"
#include "Shaders.h"

#include <string>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace burin
{
namespace
{
/// Extensions offered by the file parameter. One, because there is one format
/// this reads. `.svgz` is deliberately absent: it is gzipped SVG, nanosvg has
/// no inflate, and offering it would mean a picker that accepts a file the
/// plugin then silently fails to parse.
const char* const kExtensions[] = { "svg" };

float Clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, to calibrate the host's against. Steady rather than system, so
/// nothing here moves if the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}
} // namespace

BurinPlugin::BurinPlugin( bool isEffect ) :
	isEffect_( isEffect )
{
	diag::init();

	if( isEffect )
		SetMinInputs( 1 ), SetMaxInputs( 1 );
	else
		SetMinInputs( 0 ), SetMaxInputs( 0 );

	//-----------------------------------------------------------------------
	// Document
	//-----------------------------------------------------------------------
	{
		std::vector< std::string > extensions;
		for( const char* e : kExtensions )
			extensions.emplace_back( e );
		SetFileParamInfo( PT_FILE, "Drawing", extensions, "" );
	}

	SetOptionParamInfo( PT_BOUNDS, "Bounds", static_cast< int >( Bounds::Count ), 0 );
	SetParamElementInfo( PT_BOUNDS, 0, "Viewport", 0.0f );
	SetParamElementInfo( PT_BOUNDS, 1, "Content", 1.0f );

	SetOptionParamInfo( PT_FIT, "Fit", static_cast< int >( Fit::Count ), 0 );
	SetParamElementInfo( PT_FIT, 0, "Fit", 0.0f );
	SetParamElementInfo( PT_FIT, 1, "Fill", 1.0f );
	SetParamElementInfo( PT_FIT, 2, "Stretch", 2.0f );

	SetParamInfof( PT_DETAIL, "Detail", FF_TYPE_STANDARD );

	//-----------------------------------------------------------------------
	// Style
	//-----------------------------------------------------------------------
	SetOptionParamInfo( PT_DRAW, "Draw", static_cast< int >( Draw::Count ), 0 );
	SetParamElementInfo( PT_DRAW, 0, "Fill + Stroke", 0.0f );
	SetParamElementInfo( PT_DRAW, 1, "Fill Only", 1.0f );
	SetParamElementInfo( PT_DRAW, 2, "Stroke Only", 2.0f );

	SetParamInfof( PT_STROKE_SCALE, "Stroke Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_STROKE_MIN, "Min Stroke px", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_RECOLOUR, "Recolour", static_cast< int >( Recolour::Count ), 0 );
	SetParamElementInfo( PT_RECOLOUR, 0, "Off", 0.0f );
	SetParamElementInfo( PT_RECOLOUR, 1, "Fill", 1.0f );
	SetParamElementInfo( PT_RECOLOUR, 2, "Stroke", 2.0f );
	SetParamElementInfo( PT_RECOLOUR, 3, "Both", 3.0f );

	SetParamInfof( PT_COL_R, "Colour", FF_TYPE_RED );
	SetParamInfof( PT_COL_G, "Colour_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_COL_B, "Colour_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_COL_SPREAD, "Colour Spread", FF_TYPE_STANDARD );

	//-----------------------------------------------------------------------
	// Isolate. Real integers, because these count somebody else's file: shape
	// 14 of a drawing is shape 14, and a 0..1 slider that lands on 13 or 15
	// either side of it makes the control useless. SetParamInfo's clamp applies
	// to FF_TYPE_STANDARD only -- see Controls.h.
	//-----------------------------------------------------------------------
	SetParamInfof( PT_FIRST, "First Shape", FF_TYPE_INTEGER );
	SetParamRange( PT_FIRST, 0.0f, static_cast< float >( kMaxShapes - 1 ) );
	SetParamInfof( PT_COUNT, "Shape Count", FF_TYPE_INTEGER );
	SetParamRange( PT_COUNT, 0.0f, static_cast< float >( kMaxShapes ) );

	//-----------------------------------------------------------------------
	// Reveal
	//-----------------------------------------------------------------------
	SetOptionParamInfo( PT_REVEAL_MODE, "Reveal", static_cast< int >( RevealMode::Count ), 0 );
	SetParamElementInfo( PT_REVEAL_MODE, 0, "Off", 0.0f );
	SetParamElementInfo( PT_REVEAL_MODE, 1, "Draw On", 1.0f );
	SetParamElementInfo( PT_REVEAL_MODE, 2, "Retract", 2.0f );
	SetParamElementInfo( PT_REVEAL_MODE, 3, "Hold", 3.0f );

	SetParamInfof( PT_REVEAL, "Progress", FF_TYPE_STANDARD );
	SetParamInfof( PT_REVEAL_STAGGER, "Stagger", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_REVEAL_ORDER, "Order", static_cast< int >( RevealOrder::Count ), 0 );
	SetParamElementInfo( PT_REVEAL_ORDER, 0, "Document", 0.0f );
	SetParamElementInfo( PT_REVEAL_ORDER, 1, "Reverse", 1.0f );
	SetParamElementInfo( PT_REVEAL_ORDER, 2, "Longest First", 2.0f );
	SetParamElementInfo( PT_REVEAL_ORDER, 3, "Shortest First", 3.0f );
	SetParamElementInfo( PT_REVEAL_ORDER, 4, "Scatter", 4.0f );

	SetParamInfof( PT_REVEAL_FILL, "Fill Fade", FF_TYPE_STANDARD );

	//-----------------------------------------------------------------------
	// Motion
	//-----------------------------------------------------------------------
	SetOptionParamInfo( PT_SYNC, "Sync", static_cast< int >( SyncMode::Count ), 0 );
	SetParamElementInfo( PT_SYNC, 0, "Free", 0.0f );
	SetParamElementInfo( PT_SYNC, 1, "Beat", 1.0f );
	SetParamElementInfo( PT_SYNC, 2, "Bar", 2.0f );
	SetParamElementInfo( PT_SYNC, 3, "Manual", 3.0f );

	SetParamInfof( PT_RATE, "Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_PHASE, "Phase", FF_TYPE_STANDARD );
	SetParamInfof( PT_RESET, "Reset", FF_TYPE_EVENT );

	SetOptionParamInfo( PT_WAVE, "Wave", static_cast< int >( Wave::Count ), 0 );
	SetParamElementInfo( PT_WAVE, 0, "Sine", 0.0f );
	SetParamElementInfo( PT_WAVE, 1, "Triangle", 1.0f );
	SetParamElementInfo( PT_WAVE, 2, "Ramp", 2.0f );
	SetParamElementInfo( PT_WAVE, 3, "Pulse", 3.0f );

	SetParamInfof( PT_ZOOM, "Zoom", FF_TYPE_STANDARD );
	SetParamInfof( PT_ZOOM_MOVE, "Zoom Move", FF_TYPE_STANDARD );
	SetParamInfof( PT_POS_X, "Position X", FF_TYPE_XPOS );
	SetParamInfof( PT_POS_Y, "Position Y", FF_TYPE_YPOS );
	SetParamInfof( PT_DRIFT_X, "Drift X", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIFT_Y, "Drift Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROTATE, "Rotate", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPIN, "Spin", FF_TYPE_STANDARD );

	//-----------------------------------------------------------------------
	// Colour
	//-----------------------------------------------------------------------
	SetParamInfof( PT_TINT_R, "Tint", FF_TYPE_RED );
	SetParamInfof( PT_TINT_G, "Tint_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_TINT_B, "Tint_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_BACK_R, "Background", FF_TYPE_RED );
	SetParamInfof( PT_BACK_G, "Background_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_BACK_B, "Background_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_BACK_OPACITY, "Background Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//-----------------------------------------------------------------------
	// Preset
	//-----------------------------------------------------------------------
	SetOptionParamInfo( PT_PRESET, "Preset", presets::kOptionCount, 0 );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kPresetCount; ++i )
		SetParamElementInfo( PT_PRESET, i + 1, presets::kPresets[ i ].name, static_cast< float >( i + 1 ) );

	//-----------------------------------------------------------------------
	// Defaults. Every one of these is in host-facing 0..1 space; the physical
	// values come out of Controls.cpp.
	//-----------------------------------------------------------------------
	DefaultParams( params_ );

	// Groups. SetParamGroup collapses RUNS of same-group parameters, so the
	// declaration order above is what holds these together -- reordering an id
	// silently splits a group in two.
	for( unsigned int id = PT_FILE; id <= PT_DETAIL; ++id )
		SetParamGroup( id, "Document" );
	for( unsigned int id = PT_DRAW; id <= PT_COL_SPREAD; ++id )
		SetParamGroup( id, "Style" );
	for( unsigned int id = PT_FIRST; id <= PT_COUNT; ++id )
		SetParamGroup( id, "Isolate" );
	for( unsigned int id = PT_REVEAL_MODE; id <= PT_REVEAL_FILL; ++id )
		SetParamGroup( id, "Reveal" );
	for( unsigned int id = PT_SYNC; id <= PT_SPIN; ++id )
		SetParamGroup( id, "Motion" );
	for( unsigned int id = PT_TINT_R; id <= PT_BACK_OPACITY; ++id )
		SetParamGroup( id, "Colour" );
	SetParamGroup( PT_MIX, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT_ALL; ++id )
		SetParamGroup( id, "About" );
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------
FFResult BurinPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT_ALL )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_RESET )
	{
		// An event parameter arrives as a pulse. The clock base is taken on the
		// next render rather than here, because `hostSeconds_` is only current
		// on the render thread.
		if( value > 0.5f )
			resetPending_ = true;
		params_[ index ] = value;
		return FF_SUCCESS;
	}

	if( index == PT_PRESET )
	{
		const int choice = Option( value, presets::kOptionCount );
		params_[ index ]  = static_cast< float >( choice );
		if( choice > 0 )
			ApplyPreset( choice - 1 );
		return FF_SUCCESS;
	}

	params_[ index ] = value;

	// Editing anything a preset covers drops the dropdown back to Custom.
	// Judged by comparing values rather than by the change reason, so a host
	// echoing our own writes back at us cannot un-set the preset it just
	// applied.
	if( params_[ PT_PRESET ] > 0.5f )
	{
		const int chosen = Option( params_[ PT_PRESET ], presets::kOptionCount ) - 1;
		if( chosen >= 0 && chosen < presets::kPresetCount )
		{
			static const unsigned int kCovered[ presets::kParamCount ] = {
				PT_FIT, PT_DETAIL, PT_DRAW, PT_STROKE_SCALE, PT_STROKE_MIN,
				PT_RECOLOUR, PT_COL_R, PT_COL_G, PT_COL_B, PT_COL_SPREAD,
				PT_REVEAL_MODE, PT_REVEAL, PT_REVEAL_STAGGER, PT_REVEAL_ORDER, PT_REVEAL_FILL,
				PT_RATE, PT_WAVE, PT_ZOOM, PT_ZOOM_MOVE, PT_DRIFT_X, PT_DRIFT_Y,
				PT_ROTATE, PT_SPIN, PT_TINT_R, PT_TINT_G, PT_TINT_B, PT_OPACITY,
				PT_BACK_R, PT_BACK_G, PT_BACK_B, PT_BACK_OPACITY
			};

			for( int i = 0; i < presets::kParamCount; ++i )
			{
				if( kCovered[ i ] != index )
					continue;
				if( std::fabs( params_[ index ] - presets::kPresets[ chosen ].values[ i ] ) > 1.0e-4f )
					params_[ PT_PRESET ] = 0.0f;
				break;
			}
		}
	}

	return FF_SUCCESS;
}

float BurinPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT_ALL ? params_[ index ] : 0.0f;
}

FFResult BurinPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// Display-only, and it MUST still succeed -- see the declaration.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	if( index != PT_FILE )
		return FF_FAIL;

	std::lock_guard< std::mutex > lock( textMutex_ );
	filePath_     = value != nullptr ? value : "";
	contentDirty_ = true;
	return FF_SUCCESS;
}

char* BurinPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call. Answered before the
		// lock below -- it shares no state with the file path.
		static const std::string aboutLine = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutLine.c_str() );
	}

	std::lock_guard< std::mutex > lock( textMutex_ );

	textReturn_[ 0 ] = '\0';
	if( index == PT_FILE )
	{
		const size_t length = std::min( filePath_.size(), sizeof( textReturn_ ) - 1 );
		std::memcpy( textReturn_, filePath_.data(), length );
		textReturn_[ length ] = '\0';
	}
	return textReturn_;
}

void BurinPlugin::ApplyPreset( int presetIndex )
{
	if( presetIndex < 0 || presetIndex >= presets::kPresetCount )
		return;

	static const unsigned int kCovered[ presets::kParamCount ] = {
		PT_FIT, PT_DETAIL, PT_DRAW, PT_STROKE_SCALE, PT_STROKE_MIN,
		PT_RECOLOUR, PT_COL_R, PT_COL_G, PT_COL_B, PT_COL_SPREAD,
		PT_REVEAL_MODE, PT_REVEAL, PT_REVEAL_STAGGER, PT_REVEAL_ORDER, PT_REVEAL_FILL,
		PT_RATE, PT_WAVE, PT_ZOOM, PT_ZOOM_MOVE, PT_DRIFT_X, PT_DRIFT_Y,
		PT_ROTATE, PT_SPIN, PT_TINT_R, PT_TINT_G, PT_TINT_B, PT_OPACITY,
		PT_BACK_R, PT_BACK_G, PT_BACK_B, PT_BACK_OPACITY
	};

	const presets::Preset& preset = presets::kPresets[ presetIndex ];

	for( int i = 0; i < presets::kParamCount; ++i )
	{
		const unsigned int id = kCovered[ i ];
		if( std::fabs( params_[ id ] - preset.values[ i ] ) <= 1.0e-6f )
			continue;

		params_[ id ] = preset.values[ i ];

		// Tell the host its slider is stale. A host that ignores this still
		// renders the preset correctly and merely shows the old knob position.
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

//---------------------------------------------------------------------------
// Content
//---------------------------------------------------------------------------
void BurinPlugin::ReloadDocument()
{
	std::string path;
	{
		std::lock_guard< std::mutex > lock( textMutex_ );
		path = filePath_;
	}

	contentDirty_ = false;

	if( path == loadedPath_ )
		return;
	loadedPath_ = path;

	raster_.Invalidate();
	plan_.Invalidate();

	if( path.empty() )
	{
		document_ = Document();
		return;
	}

	Document fresh;
	const bool ok = fresh.LoadFile( path );

	if( ok )
		diag::info( fresh.Note() );
	else
		diag::error( fresh.Note() );

	// A file that would not load leaves the *previous* drawing on screen rather
	// than blanking. A composition recalled on a machine where the path does
	// not resolve is the common case, and a blank frame mid-show is worse than
	// a stale one -- the log says which happened.
	if( ok )
		document_ = std::move( fresh );
}

bool BurinPlugin::LoadDocumentString( const std::string& text, const std::string& name )
{
	raster_.Invalidate();
	plan_.Invalidate();
	contentDirty_ = false;
	loadedPath_   = name;

	Document fresh;
	const bool ok = fresh.LoadText( text, name );
	if( ok )
		document_ = std::move( fresh );
	return ok;
}

//---------------------------------------------------------------------------
// Clock
//---------------------------------------------------------------------------
void BurinPlugin::UpdateClock( double hostTime )
{
	if( forcedSeconds_ )
		return;

	// FFGL never says what unit SetTime arrives in, and hosts disagree:
	// Resolume sends MILLISECONDS (measured live at 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. Reading it raw is a thousand times fast
	// on the one host that matters and exactly right on the one that gets
	// tested, which is how it stays hidden.
	//
	// This used to guess the unit from the magnitude of a single frame delta
	// and then lock. That had three holes: a delta between 0.5 and 2.0 decided
	// nothing, a burst of sub-0.5 ms frames at load -- a thumbnail render on a
	// quick GPU -- locked it to "seconds" for the rest of the session, and
	// while undecided it assumed seconds, which is precisely the millisecond
	// host's wrong answer.
	//
	// So measure instead of guessing. steady_clock says how much real time
	// passed, the host says how much host time passed, and the ratio names the
	// unit outright. Nothing plausible sits between 1 and 1000, so both bands
	// are wide and a frame fitting neither simply does not vote.
	const double wallNow = wallSeconds();
	if( wallStart_ < 0.0 )
		wallStart_ = wallNow;

	// Never read `hostTime` before the host has set it: CFFGLPlugin's
	// constructor initialises bpm and barPhase and leaves hostTime
	// uninitialised, so until SetTime lands it is whatever was in that memory.
	const double raw = hostTimeSeen_ ? hostTime : -1.0;

	if( clockScale_ == 0.0 && raw >= 0.0 && lastRawTime_ >= 0.0 && lastWallTime_ >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime_;
		const double wallDelta = wallNow - lastWallTime_;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes_;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes_;

			// Several frames rather than one, so a single odd frame -- the
			// first after a seek, say -- cannot decide it on its own.
			if( secondsVotes_ >= kClockVotes || millisVotes_ >= kClockVotes )
			{
				clockScale_ = millisVotes_ > secondsVotes_ ? 0.001 : 1.0;
				diag::info( std::string( "host clock is " )
				            + ( clockScale_ == 0.001 ? "milliseconds" : "seconds" )
				            + ", scale=" + std::to_string( clockScale_ ) );
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime_ = raw;
	lastWallTime_ = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime at
	// all -- run on the real clock. Wrong in origin but right in rate, where
	// assuming seconds would be a thousand times fast on Resolume.
	hostSeconds_ = ( raw >= 0.0 && clockScale_ != 0.0 ) ? raw * clockScale_ : wallNow - wallStart_;
}

FFResult BurinPlugin::SetTime( double time )
{
	hostTimeSeen_ = true;
	return CFFGLPlugin::SetTime( time );
}

void BurinPlugin::SetClockScaleForTest( double scale )
{
	clockScale_ = scale;
}

void BurinPlugin::TickClockForTest()
{
	UpdateClock( hostTime );
	UpdateMotionAnchor();
}

double BurinPlugin::ClockScaleForTest() const
{
	return clockScale_;
}

double BurinPlugin::HostSecondsForTest() const
{
	return hostSeconds_;
}

void BurinPlugin::SetSecondsForTest( double seconds )
{
	forcedSeconds_ = true;
	hostSeconds_   = seconds;
	UpdateMotionAnchor();
}

double BurinPlugin::CyclesForTest() const
{
	return CurrentCycles();
}

//---------------------------------------------------------------------------
void BurinPlugin::UpdateMotionAnchor()
{
	const MotionSettings m = MotionFromParams( params_ );
	const double elapsed   = hostSeconds_ - clockBase_;

	// Beat and Bar are meant to jump -- they re-lock to the transport, which is
	// the point of them, and Manual ignores Rate entirely. Keep the anchor
	// following the clock through all three so that returning to Free resumes
	// rather than leaps.
	if( m.sync != SyncMode::Free )
	{
		anchorSeconds_ = elapsed;
		anchorRate_    = m.rate;
		return;
	}

	// First frame: leave the anchor at elapsed zero, zero cycles. That makes the
	// expression in BuildRequest identical to MotionClock's Free branch for as
	// long as nobody touches Rate, which is what keeps every rendered-frame test
	// and tools/sweep.py measuring the same thing they measured before.
	if( anchorRate_ < 0.0f )
	{
		anchorRate_ = m.rate;
		return;
	}

	if( m.rate != anchorRate_ )
	{
		// Once per rate change, not once per frame: this carries the exact cycle
		// count forward rather than integrating it, so a long session cannot
		// accumulate rounding into a drift. Frame rate still cannot affect where
		// the drawing sits.
		cycleAnchor_ += ( elapsed - anchorSeconds_ ) * static_cast< double >( anchorRate_ );
		anchorSeconds_ = elapsed;
		anchorRate_    = m.rate;
	}
}

//---------------------------------------------------------------------------
// Reading the controls
//---------------------------------------------------------------------------
RasterRequest BurinPlugin::BuildRequest( int width, int height ) const
{
	// The clock is the ONLY part of reading the controls that differs between
	// the two builds -- FFGL has a host clock and a tempo, OFX has a frame
	// number and no tempo at all -- so it is solved here and everything else is
	// shared with the OpenFX build through Settings.cpp. See Settings.h for why
	// that matters more than it looks like it should.
	return RequestFromParams( params_, width, height, CurrentCycles() );
}

//---------------------------------------------------------------------------
/// Where the motion has got to, in cycles.
///
/// Free is anchored so that moving Rate changes the pace without moving the
/// drawing -- see UpdateMotionAnchor. Until the operator has touched Rate this
/// is exactly `elapsed * rate`, which is what MotionClock's Free branch
/// returns, because the anchor starts at elapsed zero with zero cycles. Every
/// other mode is the shared pure function, unchanged.
//---------------------------------------------------------------------------
double BurinPlugin::CurrentCycles() const
{
	const MotionSettings m = MotionFromParams( params_ );
	const double elapsed   = hostSeconds_ - clockBase_;

	return m.sync == SyncMode::Free
	       ? cycleAnchor_ + ( elapsed - anchorSeconds_ ) * static_cast< double >( m.rate )
	       : MotionClock( elapsed, bpm_, barPhase_, m.sync, m.rate, m.phase );
}

ComposeSettings BurinPlugin::BuildCompose() const
{
	return ComposeFromParams( params_, isEffect_ );
}

//---------------------------------------------------------------------------
// GL
//---------------------------------------------------------------------------
FFResult BurinPlugin::InitGL( const FFGLViewportStruct* vp )
{
	// Idempotent, and it has to be: the harness calls it every frame and a host
	// is entitled to call it twice. Recompiling the shader and generating a
	// fresh VAO and texture each time would leak three GL objects per frame.
	if( glReady_ )
		return FF_SUCCESS;

	if( !shader_.Compile( kVertexShader, FragmentShader( isEffect_ ).c_str() ) )
	{
		// A shader that will not compile surfaces at runtime as "the plugin
		// does nothing", with nothing anywhere saying why. The GL strings go in
		// next to the log because a shader that builds on one machine and not
		// another is a driver answer, not a source answer.
		const char* vendor   = reinterpret_cast< const char* >( glGetString( GL_VENDOR ) );
		const char* renderer = reinterpret_cast< const char* >( glGetString( GL_RENDERER ) );
		const char* version  = reinterpret_cast< const char* >( glGetString( GL_VERSION ) );
		diag::error( std::string( "shader would not compile on " ) +
		             ( vendor ? vendor : "?" ) + " / " + ( renderer ? renderer : "?" ) +
		             " / " + ( version ? version : "?" ) );
		return FF_FAIL;
	}

	// A core profile refuses to draw without a VAO bound, even when the vertex
	// shader reads nothing but gl_VertexID and the VAO stays empty.
	glGenVertexArrays( 1, &vao_ );
	glGenTextures( 1, &texture_ );

	glBindTexture( GL_TEXTURE_2D, texture_ );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	// No mipmaps, deliberately. The raster is rebuilt whenever it would be
	// minified past a rung, so the level the hardware would pick is the level
	// that already exists -- and generating a chain on every rebuild would cost
	// a third again in bandwidth for something never sampled.

	glReady_     = true;
	uploadDirty_ = true;

	if( vp != nullptr )
		glViewport( 0, 0, vp->width, vp->height );

	return FF_SUCCESS;
}

bool BurinPlugin::UploadRaster()
{
	const RasterPlacement& p = raster_.Placement();
	if( p.width <= 0 || p.height <= 0 || raster_.Pixels().empty() )
		return false;

	glBindTexture( GL_TEXTURE_2D, texture_ );

	// A resize needs a fresh allocation; a same-size rebuild -- which is what
	// every frame of a write-on is -- only needs the pixels. glTexSubImage2D
	// avoids reallocating the whole texture sixty times a second.
	if( p.width != textureW_ || p.height != textureH_ )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, p.width, p.height, 0,
		              GL_RGBA, GL_UNSIGNED_BYTE, raster_.Pixels().data() );
		textureW_ = p.width;
		textureH_ = p.height;
	}
	else
	{
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, p.width, p.height,
		                 GL_RGBA, GL_UNSIGNED_BYTE, raster_.Pixels().data() );
	}

	glBindTexture( GL_TEXTURE_2D, 0 );
	return true;
}

FFResult BurinPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( !glReady_ )
		return FF_FAIL;

	// The host's own viewport, captured before anything can change it. The SDK's
	// scoped bindings restore the framebuffer and NOT the viewport.
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, viewport );
	const int frameW = std::max( viewport[ 2 ], 1 );
	const int frameH = std::max( viewport[ 3 ], 1 );

	// The host PUSHES these into protected members of CFFGLPlugin -- SetTime
	// and SetBeatInfo are virtuals it calls, not getters we call -- so they are
	// read here rather than queried. `bpm` is whatever the host last said and
	// is zero in a host that never says; MotionClock falls back to 120.
	UpdateClock( hostTime );
	bpm_      = bpm;
	barPhase_ = barPhase;

	if( resetPending_ )
	{
		clockBase_    = hostSeconds_;
		resetPending_ = false;

		// Starting again means starting again: a carried-forward cycle count
		// would survive the reset and leave the drawing wherever it had got to.
		cycleAnchor_   = 0.0;
		anchorSeconds_ = 0.0;
	}

	UpdateMotionAnchor();

	if( contentDirty_ )
		ReloadDocument();

	const RasterRequest request  = BuildRequest( frameW, frameH );
	const ComposeSettings compose = BuildCompose();

	if( raster_.Build( document_, request, plan_ ) )
		uploadDirty_ = true;

	if( uploadDirty_ )
		uploadDirty_ = !UploadRaster();

	const RasterPlacement& placement = raster_.Placement();

	const Rect box = ( request.bounds == Bounds::Content ) ? document_.Content() : document_.Viewport();
	float baseScale = 1.0f;
	const Transform2D inv = BuildTransform( request, box, baseScale ).Inverse();

	//-----------------------------------------------------------------------
	// Draw. Plain glUseProgram and glBindTexture rather than the SDK's scoped
	// bindings: every ffglex::Scoped* CLEARS to zero on scope exit instead of
	// restoring, so using them here would unbind the host's state rather than
	// putting it back.
	//-----------------------------------------------------------------------
	glUseProgram( shader_.GetGLID() );
	glBindVertexArray( vao_ );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, texture_ );
	shader_.Set( "Raster", 0 );

	shader_.Set( "InvA", inv.a, inv.b );
	shader_.Set( "InvC", inv.c, inv.d );
	shader_.Set( "InvE", inv.e, inv.f );
	shader_.Set( "FrameSize", static_cast< float >( frameW ), static_cast< float >( frameH ) );

	shader_.Set( "CoverMin", placement.cover.minx, placement.cover.miny );
	shader_.Set( "CoverMax", placement.cover.maxx, placement.cover.maxy );
	shader_.Set( "RasterSize", static_cast< float >( std::max( placement.width, 1 ) ),
	             static_cast< float >( std::max( placement.height, 1 ) ) );
	shader_.Set( "RasterScale", placement.scale );

	shader_.Set( "Tint", compose.tintR, compose.tintG, compose.tintB );
	shader_.Set( "Opacity", compose.opacity );
	shader_.Set( "Background", compose.backR, compose.backG, compose.backB );
	shader_.Set( "BackgroundOpacity", compose.backOpacity );

	if( isEffect_ && pGL != nullptr && pGL->numInputTextures > 0 && pGL->inputTextures[ 0 ] != nullptr )
	{
		const FFGLTextureStruct& in = *pGL->inputTextures[ 0 ];
		// A free function in FFGLLib.h, not in ffglex. MaxUV is the fraction of
		// the input texture the host actually drew into; the rest is undrawn
		// padding, and a fetch that reached it would take a column of black
		// down one edge of the frame.
		const FFGLTexCoords max = GetMaxGLTexCoords( in );

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, in.Handle );
		shader_.Set( "Input", 1 );
		shader_.Set( "InputMaxUV", max.s, max.t );
		shader_.Set( "Mix", compose.mix );
	}

	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );// premultiplied, as FFGL wants

	glDrawArrays( GL_TRIANGLES, 0, 3 );

	// Put the state back by hand, since nothing scoped did it for us.
	glBindVertexArray( 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glUseProgram( 0 );

	return FF_SUCCESS;
}

FFResult BurinPlugin::DeInitGL()
{
	shader_.FreeGLResources();

	if( vao_ != 0 )
	{
		glDeleteVertexArrays( 1, &vao_ );
		vao_ = 0;
	}
	if( texture_ != 0 )
	{
		glDeleteTextures( 1, &texture_ );
		texture_ = 0;
	}

	textureW_ = textureH_ = 0;
	glReady_  = false;
	return FF_SUCCESS;
}

} // namespace burin
