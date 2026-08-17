#include "Settings.h"

#include <cmath>

namespace burin
{
namespace
{
float Clamp01( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}
} // namespace

void DefaultParams( float* params )
{
	for( int i = 0; i < PT_COUNT_ALL; ++i )
		params[ i ] = 0.0f;

	params[ PT_DETAIL ]       = 0.5f; // 1.0x
	params[ PT_STROKE_SCALE ] = 0.5f; // 1.0x
	params[ PT_STROKE_MIN ]   = 0.25f;// one device pixel
	params[ PT_COL_R ]        = 1.0f;
	params[ PT_COL_G ]        = 1.0f;
	params[ PT_COL_B ]        = 1.0f;
	params[ PT_COL_SPREAD ]   = 0.5f;// no walk
	params[ PT_REVEAL ]       = 1.0f;// finished, so a fresh instance draws
	params[ PT_REVEAL_FILL ]  = 0.33f;
	params[ PT_RATE ]         = 0.55f;
	params[ PT_ZOOM ]         = 0.5f;// 1:1
	params[ PT_POS_X ]        = 0.5f;
	params[ PT_POS_Y ]        = 0.5f;
	params[ PT_ROTATE ]       = 0.5f;
	params[ PT_SPIN ]         = 0.5f;
	params[ PT_TINT_R ]       = 1.0f;
	params[ PT_TINT_G ]       = 1.0f;
	params[ PT_TINT_B ]       = 1.0f;
	params[ PT_OPACITY ]      = 1.0f;
	params[ PT_MIX ]          = 1.0f;
}

MotionSettings MotionFromParams( const float* params )
{
	MotionSettings m;
	m.sync     = static_cast< SyncMode >( Option( params[ PT_SYNC ], static_cast< int >( SyncMode::Count ) ) );
	m.rate     = RateFromParam( params[ PT_RATE ] );
	m.phase    = params[ PT_PHASE ];
	m.wave     = static_cast< Wave >( Option( params[ PT_WAVE ], static_cast< int >( Wave::Count ) ) );
	m.zoom     = ZoomFromParam( params[ PT_ZOOM ] );
	m.zoomMove = ZoomMoveFromParam( params[ PT_ZOOM_MOVE ] );
	m.panX     = PanFromParam( params[ PT_POS_X ] );
	m.panY     = PanFromParam( params[ PT_POS_Y ] );
	m.driftX   = DriftFromParam( params[ PT_DRIFT_X ] );
	m.driftY   = DriftFromParam( params[ PT_DRIFT_Y ] );
	m.rotate   = RotateFromParam( params[ PT_ROTATE ] );
	m.spin     = SpinFromParam( params[ PT_SPIN ] );
	return m;
}

RasterRequest RequestFromParams( const float* params, int width, int height, double cycles )
{
	RasterRequest r;
	r.frameWidth  = width > 1 ? width : 1;
	r.frameHeight = height > 1 ? height : 1;

	r.bounds = static_cast< Bounds >( Option( params[ PT_BOUNDS ], static_cast< int >( Bounds::Count ) ) );
	r.fit    = static_cast< Fit >( Option( params[ PT_FIT ], static_cast< int >( Fit::Count ) ) );
	r.detail = DetailFromParam( params[ PT_DETAIL ] );

	r.style.draw         = static_cast< Draw >( Option( params[ PT_DRAW ], static_cast< int >( Draw::Count ) ) );
	r.style.strokeScale  = StrokeScaleFromParam( params[ PT_STROKE_SCALE ] );
	r.style.strokeMinPx  = StrokeMinFromParam( params[ PT_STROKE_MIN ] );
	r.style.recolour     = static_cast< Recolour >( Option( params[ PT_RECOLOUR ], static_cast< int >( Recolour::Count ) ) );
	r.style.colourR      = Clamp01( params[ PT_COL_R ] );
	r.style.colourG      = Clamp01( params[ PT_COL_G ] );
	r.style.colourB      = Clamp01( params[ PT_COL_B ] );
	r.style.colourSpread = ColourSpreadFromParam( params[ PT_COL_SPREAD ] );
	r.style.firstShape   = static_cast< int >( std::lround( params[ PT_FIRST ] ) );
	r.style.shapeCount   = static_cast< int >( std::lround( params[ PT_COUNT ] ) );

	const RevealMode mode = static_cast< RevealMode >( Option( params[ PT_REVEAL_MODE ], static_cast< int >( RevealMode::Count ) ) );
	r.reveal.mode         = mode;
	r.reveal.stagger      = RevealStaggerFromParam( params[ PT_REVEAL_STAGGER ] );
	r.reveal.order        = static_cast< RevealOrder >( Option( params[ PT_REVEAL_ORDER ], static_cast< int >( RevealOrder::Count ) ) );
	r.reveal.fillWindow   = RevealFillFromParam( params[ PT_REVEAL_FILL ] );

	const MotionSettings m = MotionFromParams( params );
	r.motion               = SolveMotion( m, cycles );

	// Hold takes the Progress slider literally; Draw On and Retract ride the
	// same clock the motion does, so a reveal cued to a bar lands on the bar.
	// The fractional part is what makes it loop -- a reveal that ran once and
	// stopped would need a state machine, and this needs none.
	if( mode == RevealMode::Hold || m.sync == SyncMode::Manual )
		r.reveal.progress = RevealFromParam( params[ PT_REVEAL ] );
	else if( mode == RevealMode::None )
		r.reveal.progress = 1.0f;
	else
		r.reveal.progress = static_cast< float >( cycles - std::floor( cycles ) );

	// Hold is not a separate code path downstream -- it is Draw On with a
	// hand-driven clock -- so it is translated here and Reveal.cpp never sees
	// it.
	if( mode == RevealMode::Hold )
		r.reveal.mode = RevealMode::On;

	return r;
}

ComposeSettings ComposeFromParams( const float* params, bool isEffect )
{
	ComposeSettings c;
	c.tintR       = Clamp01( params[ PT_TINT_R ] );
	c.tintG       = Clamp01( params[ PT_TINT_G ] );
	c.tintB       = Clamp01( params[ PT_TINT_B ] );
	c.opacity     = Clamp01( params[ PT_OPACITY ] );
	c.backR       = Clamp01( params[ PT_BACK_R ] );
	c.backG       = Clamp01( params[ PT_BACK_G ] );
	c.backB       = Clamp01( params[ PT_BACK_B ] );
	c.backOpacity = Clamp01( params[ PT_BACK_OPACITY ] );
	c.mix         = isEffect ? Clamp01( params[ PT_MIX ] ) : 1.0f;
	return c;
}

} // namespace burin
