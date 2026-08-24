/// The OpenFX builds of Burin, for DaVinci Resolve, Nuke, Natron, Vegas
/// and other OFX hosts. Two plugins from this one file, as the FFGL side ships
/// two bundles: "Burin" is a generator, "Burin Over" draws over the
/// incoming clip.
///
/// ## What is shared, which here is nearly everything
///
/// This file is much thinner than the rest of the fleet's OFX builds, and that
/// is a consequence of the design rather than an accident. Every other plugin
/// in the fleet makes its picture in a fragment shader, so its OFX build has to
/// carry a second implementation of the whole per-pixel algorithm. This one
/// makes its picture with a CPU rasteriser in **both** builds — the GPU's only
/// job in Resolume is the final transform and composite — so `Document`,
/// `Style`, `Reveal`, `Motion`, `Raster`, `Compose`, `Settings` and `Controls`
/// are linked straight from source and are the same objects `burintest` measures.
///
/// `Settings.cpp` matters most of all: the reading of the controls is shared,
/// so "Blueprint" cannot mean one thing in Resolume and another in Resolve.
/// The FFGL build fills a `float[PT_COUNT_ALL]` from its own parameter array
/// and this one fills the same array from its OFX param handles, and both hand
/// it to the same function.
///
/// What is left to do here is: declare the parameters, fill that array, work
/// out the clock, and write the composited frame into the output image.
///
/// ## The differences that are not bugs
///
/// **Sync offers Free and Manual only.** OFX carries no tempo, so Beat and Bar
/// have no clock to lock to. Manual is the mode for keyframing Phase against
/// the edit, and it is the one mode that behaves identically in both builds.
///
/// **OFX hands render time in frames.** The clip's frame rate turns it into the
/// seconds `MotionClock` wants. A host that reports no frame rate gets 25,
/// which is wrong somewhere but is never zero.
///
/// **Rendering is not tiled.** `setSupportsTiles(false)` on both clips. The
/// raster is built for the whole frame's transform in one go and a tile would
/// either rebuild it per tile — many times the work — or need the untiled
/// bounds anyway. Tiles buy nothing here.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Compose.h"
#include "../Controls.h"
#include "../Diag.h"
#include "../Document.h"
#include "../Motion.h"
#include "../Presets.h"
#include "../Raster.h"
#include "../Reveal.h"
#include "../Settings.h"
#include "../Style.h"

using namespace burin;

namespace
{
const char* const kSourceIdentifier = "com.stoatworks.burin";
const char* const kOverIdentifier   = "com.stoatworks.burinover";

/// OFX parameter names. Kept in one table beside the ParamId they correspond to
/// so that filling the shared float array is a loop rather than forty lines
/// that can each be wrong individually.
struct ParamName
{
	unsigned int id;
	const char* name;
	const char* label;
};

const ParamName kParams[] = {
	{ PT_BOUNDS, "bounds", "Bounds" },
	{ PT_FIT, "fit", "Fit" },
	{ PT_DETAIL, "detail", "Detail" },
	{ PT_DRAW, "draw", "Draw" },
	{ PT_STROKE_SCALE, "strokeWidth", "Stroke Width" },
	{ PT_STROKE_MIN, "strokeMin", "Min Stroke px" },
	{ PT_RECOLOUR, "recolour", "Recolour" },
	{ PT_COL_SPREAD, "colourSpread", "Colour Spread" },
	{ PT_FIRST, "firstShape", "First Shape" },
	{ PT_COUNT, "shapeCount", "Shape Count" },
	{ PT_REVEAL_MODE, "revealMode", "Reveal" },
	{ PT_REVEAL, "reveal", "Progress" },
	{ PT_REVEAL_STAGGER, "revealStagger", "Stagger" },
	{ PT_REVEAL_ORDER, "revealOrder", "Order" },
	{ PT_REVEAL_FILL, "revealFill", "Fill Fade" },
	{ PT_SYNC, "sync", "Sync" },
	{ PT_RATE, "rate", "Rate" },
	{ PT_PHASE, "phase", "Phase" },
	{ PT_WAVE, "wave", "Wave" },
	{ PT_ZOOM, "zoom", "Zoom" },
	{ PT_ZOOM_MOVE, "zoomMove", "Zoom Move" },
	{ PT_POS_X, "posX", "Position X" },
	{ PT_POS_Y, "posY", "Position Y" },
	{ PT_DRIFT_X, "driftX", "Drift X" },
	{ PT_DRIFT_Y, "driftY", "Drift Y" },
	{ PT_ROTATE, "rotate", "Rotate" },
	{ PT_SPIN, "spin", "Spin" },
	{ PT_OPACITY, "opacity", "Opacity" },
	{ PT_BACK_OPACITY, "backOpacity", "Background Opacity" },
	{ PT_MIX, "mix", "Mix" },
};
constexpr int kParamNameCount = static_cast< int >( sizeof( kParams ) / sizeof( kParams[ 0 ] ) );

//---------------------------------------------------------------------------
// The parameters, described once for both plugins.
//---------------------------------------------------------------------------
OFX::DoubleParamDescriptor* MakeDouble( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name, const char* label, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabel( label );
	p->setDefault( def );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	if( page != nullptr )
		page->addChild( *p );
	return p;
}

OFX::ChoiceParamDescriptor* MakeChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name, const char* label,
                                        const std::vector< const char* >& options, int def )
{
	OFX::ChoiceParamDescriptor* p = desc.defineChoiceParam( name );
	p->setLabel( label );
	for( const char* o : options )
		p->appendOption( o );
	p->setDefault( def );
	if( page != nullptr )
		page->addChild( *p );
	return p;
}

void DescribeParams( OFX::ImageEffectDescriptor& desc, bool isEffect )
{
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	float defaults[ PT_COUNT_ALL ];
	DefaultParams( defaults );

	// --- Document ---------------------------------------------------------
	{
		OFX::StringParamDescriptor* file = desc.defineStringParam( "drawing" );
		file->setLabel( "Drawing" );
		file->setStringType( OFX::eStringTypeFilePath );
		file->setDefault( "" );
		page->addChild( *file );
	}

	MakeChoice( desc, page, "bounds", "Bounds", { "Viewport", "Content" }, 0 );
	MakeChoice( desc, page, "fit", "Fit", { "Fit", "Fill", "Stretch" }, 0 );
	MakeDouble( desc, page, "detail", "Detail", defaults[ PT_DETAIL ] );

	// --- Style ------------------------------------------------------------
	MakeChoice( desc, page, "draw", "Draw", { "Fill + Stroke", "Fill Only", "Stroke Only" }, 0 );
	MakeDouble( desc, page, "strokeWidth", "Stroke Width", defaults[ PT_STROKE_SCALE ] );
	MakeDouble( desc, page, "strokeMin", "Min Stroke px", defaults[ PT_STROKE_MIN ] );
	MakeChoice( desc, page, "recolour", "Recolour", { "Off", "Fill", "Stroke", "Both" }, 0 );

	{
		// A real RGB swatch rather than three sliders. FFGL has no colour
		// parameter type and has to spell it out as RED/GREEN/BLUE; OFX does,
		// and using it is what makes the plugin feel native in Resolve.
		OFX::RGBParamDescriptor* c = desc.defineRGBParam( "colour" );
		c->setLabel( "Colour" );
		c->setDefault( defaults[ PT_COL_R ], defaults[ PT_COL_G ], defaults[ PT_COL_B ] );
		page->addChild( *c );
	}
	MakeDouble( desc, page, "colourSpread", "Colour Spread", defaults[ PT_COL_SPREAD ] );

	// --- Isolate ----------------------------------------------------------
	{
		OFX::IntParamDescriptor* first = desc.defineIntParam( "firstShape" );
		first->setLabel( "First Shape" );
		first->setDefault( 0 );
		first->setRange( 0, kMaxShapes - 1 );
		first->setDisplayRange( 0, 256 );
		page->addChild( *first );

		OFX::IntParamDescriptor* count = desc.defineIntParam( "shapeCount" );
		count->setLabel( "Shape Count" );
		count->setDefault( 0 );
		count->setRange( 0, kMaxShapes );
		count->setDisplayRange( 0, 256 );
		page->addChild( *count );
	}

	// --- Reveal -----------------------------------------------------------
	MakeChoice( desc, page, "revealMode", "Reveal", { "Off", "Draw On", "Retract", "Hold" }, 0 );
	MakeDouble( desc, page, "reveal", "Progress", defaults[ PT_REVEAL ] );
	MakeDouble( desc, page, "revealStagger", "Stagger", defaults[ PT_REVEAL_STAGGER ] );
	MakeChoice( desc, page, "revealOrder", "Order",
	            { "Document", "Reverse", "Longest First", "Shortest First", "Scatter" }, 0 );
	MakeDouble( desc, page, "revealFill", "Fill Fade", defaults[ PT_REVEAL_FILL ] );

	// --- Motion -----------------------------------------------------------
	// Free and Manual only. An OFX host has no tempo to lock to, and offering
	// Beat and Bar would put two modes in the list that silently behave as Free.
	MakeChoice( desc, page, "sync", "Sync", { "Free", "Manual" }, 0 );
	MakeDouble( desc, page, "rate", "Rate", defaults[ PT_RATE ] );
	MakeDouble( desc, page, "phase", "Phase", defaults[ PT_PHASE ] );
	MakeChoice( desc, page, "wave", "Wave", { "Sine", "Triangle", "Ramp", "Pulse" }, 0 );
	MakeDouble( desc, page, "zoom", "Zoom", defaults[ PT_ZOOM ] );
	MakeDouble( desc, page, "zoomMove", "Zoom Move", defaults[ PT_ZOOM_MOVE ] );
	MakeDouble( desc, page, "posX", "Position X", defaults[ PT_POS_X ] );
	MakeDouble( desc, page, "posY", "Position Y", defaults[ PT_POS_Y ] );
	MakeDouble( desc, page, "driftX", "Drift X", defaults[ PT_DRIFT_X ] );
	MakeDouble( desc, page, "driftY", "Drift Y", defaults[ PT_DRIFT_Y ] );
	MakeDouble( desc, page, "rotate", "Rotate", defaults[ PT_ROTATE ] );
	MakeDouble( desc, page, "spin", "Spin", defaults[ PT_SPIN ] );

	// --- Colour -----------------------------------------------------------
	{
		OFX::RGBParamDescriptor* t = desc.defineRGBParam( "tint" );
		t->setLabel( "Tint" );
		t->setDefault( 1.0, 1.0, 1.0 );
		page->addChild( *t );
	}
	MakeDouble( desc, page, "opacity", "Opacity", defaults[ PT_OPACITY ] );

	{
		OFX::RGBParamDescriptor* b = desc.defineRGBParam( "background" );
		b->setLabel( "Background" );
		b->setDefault( 0.0, 0.0, 0.0 );
		page->addChild( *b );
	}
	MakeDouble( desc, page, "backOpacity", "Background Opacity", defaults[ PT_BACK_OPACITY ] );

	if( isEffect )
		MakeDouble( desc, page, "mix", "Mix", defaults[ PT_MIX ] );

	// --- Presets ----------------------------------------------------------
	{
		std::vector< const char* > options;
		options.push_back( "Custom" );
		for( int i = 0; i < presets::kPresetCount; ++i )
			options.push_back( presets::kPresets[ i ].name );
		MakeChoice( desc, page, "preset", "Preset", options, 0 );
	}

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

//---------------------------------------------------------------------------
// The plugin instance.
//---------------------------------------------------------------------------
class BurinOFXPlugin : public OFX::ImageEffect
{
public:
	BurinOFXPlugin( OfxImageEffectHandle handle, bool isEffect ) :
		OFX::ImageEffect( handle ),
		isEffect_( isEffect )
	{
		// Same log as the FFGL build, in the same place. The failures this
		// plugin has are host-independent -- a drawing that would not parse, a
		// file that is all live text, a raster capped for size -- and they look
		// identical from outside in Resolve and in Resolume.
		diag::init();

		dstClip_ = fetchClip( kOfxImageEffectOutputClipName );
		if( isEffect_ )
			srcClip_ = fetchClip( kOfxImageEffectSimpleSourceClipName );

		drawing_ = fetchStringParam( "drawing" );
		colour_  = fetchRGBParam( "colour" );
		tint_    = fetchRGBParam( "tint" );
		back_    = fetchRGBParam( "background" );
		first_   = fetchIntParam( "firstShape" );
		count_   = fetchIntParam( "shapeCount" );
		preset_  = fetchChoiceParam( "preset" );

		for( int i = 0; i < kParamNameCount; ++i )
		{
			const ParamName& p = kParams[ i ];
			if( p.id == PT_FIRST || p.id == PT_COUNT )
				continue;// integers, fetched above
			if( p.id == PT_MIX && !isEffect_ )
				continue;

			if( paramExists( p.name ) )
			{
				if( p.id == PT_BOUNDS || p.id == PT_FIT || p.id == PT_DRAW || p.id == PT_RECOLOUR ||
				    p.id == PT_REVEAL_MODE || p.id == PT_REVEAL_ORDER || p.id == PT_SYNC || p.id == PT_WAVE )
					choices_[ p.id ] = fetchChoiceParam( p.name );
				else
					doubles_[ p.id ] = fetchDoubleParam( p.name );
			}
		}
	}

	void render( const OFX::RenderArguments& args ) override;
	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& name ) override;

private:
	/// Fill the shared parameter array from the OFX param handles at `time`.
	/// Everything downstream then goes through `Settings.cpp`, the same as the
	/// FFGL build.
	void ReadParams( double time, float* params );

	bool paramExists( const char* name ) const
	{
		return OFX::ImageEffect::paramExists( name );
	}

	const bool isEffect_;

	OFX::Clip* dstClip_ = nullptr;
	OFX::Clip* srcClip_ = nullptr;

	OFX::StringParam* drawing_ = nullptr;
	OFX::RGBParam* colour_     = nullptr;
	OFX::RGBParam* tint_       = nullptr;
	OFX::RGBParam* back_       = nullptr;
	OFX::IntParam* first_      = nullptr;
	OFX::IntParam* count_      = nullptr;
	OFX::ChoiceParam* preset_  = nullptr;

	OFX::DoubleParam* doubles_[ PT_COUNT_ALL ] = { nullptr };
	OFX::ChoiceParam* choices_[ PT_COUNT_ALL ] = { nullptr };

	Document document_;
	std::string loadedPath_;
	Rasteriser raster_;
	RevealPlan plan_;
};

void BurinOFXPlugin::ReadParams( double time, float* params )
{
	DefaultParams( params );

	for( int i = 0; i < PT_COUNT_ALL; ++i )
	{
		if( doubles_[ i ] != nullptr )
			params[ i ] = static_cast< float >( doubles_[ i ]->getValueAtTime( time ) );
		else if( choices_[ i ] != nullptr )
		{
			int v = 0;
			choices_[ i ]->getValueAtTime( time, v );
			params[ i ] = static_cast< float >( v );
		}
	}

	double r = 1.0, g = 1.0, b = 1.0;
	colour_->getValueAtTime( time, r, g, b );
	params[ PT_COL_R ] = static_cast< float >( r );
	params[ PT_COL_G ] = static_cast< float >( g );
	params[ PT_COL_B ] = static_cast< float >( b );

	tint_->getValueAtTime( time, r, g, b );
	params[ PT_TINT_R ] = static_cast< float >( r );
	params[ PT_TINT_G ] = static_cast< float >( g );
	params[ PT_TINT_B ] = static_cast< float >( b );

	back_->getValueAtTime( time, r, g, b );
	params[ PT_BACK_R ] = static_cast< float >( r );
	params[ PT_BACK_G ] = static_cast< float >( g );
	params[ PT_BACK_B ] = static_cast< float >( b );

	int iv = 0;
	first_->getValueAtTime( time, iv );
	params[ PT_FIRST ] = static_cast< float >( iv );
	count_->getValueAtTime( time, iv );
	params[ PT_COUNT ] = static_cast< float >( iv );

	// The Sync dropdown here has two entries, not four: an OFX host has no
	// tempo. Element 1 is Manual, which is element 3 in the shared enum, so it
	// is translated rather than passed through -- an index that meant Beat here
	// would silently select a mode with no clock behind it.
	if( choices_[ PT_SYNC ] != nullptr )
	{
		int sync = 0;
		choices_[ PT_SYNC ]->getValueAtTime( time, sync );
		params[ PT_SYNC ] = ( sync == 1 ) ? static_cast< float >( SyncMode::Manual )
		                                  : static_cast< float >( SyncMode::Free );
	}
}

void BurinOFXPlugin::changedParam( const OFX::InstanceChangedArgs& args, const std::string& name )
{
	// The About links open a browser and change nothing about the render.
	if( stoatworks::about::ofx::changedParam( args, name ) )
		return;

	if( name != "preset" )
		return;

	int choice = 0;
	preset_->getValue( choice );
	if( choice <= 0 || choice > presets::kPresetCount )
		return;

	static const unsigned int kCovered[ presets::kParamCount ] = {
		PT_FIT, PT_DETAIL, PT_DRAW, PT_STROKE_SCALE, PT_STROKE_MIN,
		PT_RECOLOUR, PT_COL_R, PT_COL_G, PT_COL_B, PT_COL_SPREAD,
		PT_REVEAL_MODE, PT_REVEAL, PT_REVEAL_STAGGER, PT_REVEAL_ORDER, PT_REVEAL_FILL,
		PT_RATE, PT_WAVE, PT_ZOOM, PT_ZOOM_MOVE, PT_DRIFT_X, PT_DRIFT_Y,
		PT_ROTATE, PT_SPIN, PT_TINT_R, PT_TINT_G, PT_TINT_B, PT_OPACITY,
		PT_BACK_R, PT_BACK_G, PT_BACK_B, PT_BACK_OPACITY
	};

	const presets::Preset& p = presets::kPresets[ choice - 1 ];

	// One edit block, so undo takes the whole preset back at once rather than
	// one slider at a time.
	beginEditBlock( "preset" );
	for( int i = 0; i < presets::kParamCount; ++i )
	{
		const unsigned int id = kCovered[ i ];
		if( doubles_[ id ] != nullptr )
			doubles_[ id ]->setValue( p.values[ i ] );
		else if( choices_[ id ] != nullptr )
			choices_[ id ]->setValue( static_cast< int >( std::lround( p.values[ i ] ) ) );
	}
	colour_->setValue( p.values[ presets::kColR ], p.values[ presets::kColG ], p.values[ presets::kColB ] );
	tint_->setValue( p.values[ presets::kTintR ], p.values[ presets::kTintG ], p.values[ presets::kTintB ] );
	back_->setValue( p.values[ presets::kBackR ], p.values[ presets::kBackG ], p.values[ presets::kBackB ] );
	endEditBlock();

	( void )args;
}

void BurinOFXPlugin::render( const OFX::RenderArguments& args )
{
	std::unique_ptr< OFX::Image > dst( dstClip_->fetchImage( args.time ) );
	if( !dst )
		return;

	const OfxRectI bounds = dst->getBounds();
	const int width       = bounds.x2 - bounds.x1;
	const int height      = bounds.y2 - bounds.y1;
	if( width <= 0 || height <= 0 )
		return;

	// --- the drawing ------------------------------------------------------
	std::string path;
	drawing_->getValue( path );
	if( path != loadedPath_ )
	{
		loadedPath_ = path;
		raster_.Invalidate();
		plan_.Invalidate();
		Document fresh;
		if( fresh.LoadFile( path ) )
			document_ = std::move( fresh );
	}

	// --- the controls -----------------------------------------------------
	float params[ PT_COUNT_ALL ];
	ReadParams( args.time, params );

	// OFX hands render time in FRAMES. The clip's frame rate turns it into the
	// seconds MotionClock wants; a host reporting no frame rate gets 25, which
	// is wrong somewhere but is never zero and never a division by it.
	double fps = dstClip_->getFrameRate();
	if( !( fps > 0.0 ) )
		fps = 25.0;
	const double seconds = args.time / fps;

	// The plain product, deliberately: the FFGL build anchors its Free-mode
	// cycle count so that nudging Rate live does not teleport the drawing, and
	// that anchor is a running carry which needs frames to arrive in order. This
	// host renders arbitrary times in arbitrary order and can keyframe Rate, so
	// an anchor here would make a frame depend on which frames happened to be
	// rendered before it. A pure function of time is the right answer for a
	// timeline; see Burin.h.
	const MotionSettings m = MotionFromParams( params );
	const double cycles    = MotionClock( seconds, 0.0f, 0.0f, m.sync, m.rate, m.phase );

	const RasterRequest request   = RequestFromParams( params, width, height, cycles );
	const ComposeSettings compose = ComposeFromParams( params, isEffect_ );

	raster_.Build( document_, request, plan_ );

	const Rect box  = ( request.bounds == Bounds::Content ) ? document_.Content() : document_.Viewport();
	float baseScale = 1.0f;
	const Transform2D inv = BuildTransform( request, box, baseScale ).Inverse();

	// --- the input clip ---------------------------------------------------
	Frame inputFrame;
	std::unique_ptr< OFX::Image > src;
	if( isEffect_ && srcClip_ != nullptr && srcClip_->isConnected() )
	{
		src.reset( srcClip_->fetchImage( args.time ) );
		if( src )
		{
			inputFrame.Resize( width, height );
			for( int y = 0; y < height; ++y )
			{
				// OFX images have their origin at the BOTTOM left; `Frame` has
				// it at the top, like the raster and like SVG. One flip, here.
				const int oy = bounds.y2 - 1 - y;
				for( int x = 0; x < width; ++x )
				{
					float rgba[ 4 ] = { 0, 0, 0, 0 };
					if( src->getPixelDepth() == OFX::eBitDepthFloat )
					{
						const float* p = static_cast< const float* >( src->getPixelAddress( bounds.x1 + x, oy ) );
						if( p != nullptr )
							for( int c = 0; c < 4; ++c )
								rgba[ c ] = p[ c ];
					}
					else
					{
						const unsigned char* p = static_cast< const unsigned char* >( src->getPixelAddress( bounds.x1 + x, oy ) );
						if( p != nullptr )
							for( int c = 0; c < 4; ++c )
								rgba[ c ] = p[ c ] / 255.0f;
					}

					const size_t i             = ( static_cast< size_t >( y ) * width + x ) * 4;
					inputFrame.rgba[ i + 0 ] = static_cast< uint8_t >( std::min( std::max( rgba[ 0 ], 0.0f ), 1.0f ) * 255.0f + 0.5f );
					inputFrame.rgba[ i + 1 ] = static_cast< uint8_t >( std::min( std::max( rgba[ 1 ], 0.0f ), 1.0f ) * 255.0f + 0.5f );
					inputFrame.rgba[ i + 2 ] = static_cast< uint8_t >( std::min( std::max( rgba[ 2 ], 0.0f ), 1.0f ) * 255.0f + 0.5f );
					inputFrame.rgba[ i + 3 ] = static_cast< uint8_t >( std::min( std::max( rgba[ 3 ], 0.0f ), 1.0f ) * 255.0f + 0.5f );
				}
			}
		}
	}

	// --- composite --------------------------------------------------------
	Frame frame;
	frame.Resize( width, height );
	ComposeFrame( frame, raster_.Pixels(), raster_.Placement(), inv, compose,
	              inputFrame.Valid() ? &inputFrame : nullptr );

	// --- out --------------------------------------------------------------
	const bool isFloat = ( dst->getPixelDepth() == OFX::eBitDepthFloat );
	const int comps    = ( dst->getPixelComponents() == OFX::ePixelComponentRGBA ) ? 4 : 3;

	for( int y = 0; y < height; ++y )
	{
		if( abort() )
			break;

		const int oy = bounds.y2 - 1 - y;// the flip again, on the way out
		for( int x = 0; x < width; ++x )
		{
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;

			// ComposeFrame produces STRAIGHT alpha. OFX images are
			// premultiplied unless the host says otherwise, and Resolve's are,
			// so the multiply happens here -- the same last-line multiply the
			// fragment shader does in the FFGL build.
			const float a = frame.rgba[ i + 3 ] / 255.0f;

			if( isFloat )
			{
				float* p = static_cast< float* >( dst->getPixelAddress( bounds.x1 + x, oy ) );
				if( p == nullptr )
					continue;
				for( int c = 0; c < 3 && c < comps; ++c )
					p[ c ] = ( frame.rgba[ i + c ] / 255.0f ) * a;
				if( comps == 4 )
					p[ 3 ] = a;
			}
			else
			{
				unsigned char* p = static_cast< unsigned char* >( dst->getPixelAddress( bounds.x1 + x, oy ) );
				if( p == nullptr )
					continue;
				for( int c = 0; c < 3 && c < comps; ++c )
					p[ c ] = static_cast< unsigned char >( frame.rgba[ i + c ] * a + 0.5f );
				if( comps == 4 )
					p[ 3 ] = frame.rgba[ i + 3 ];
			}
		}
	}
}

//---------------------------------------------------------------------------
// Factories
//---------------------------------------------------------------------------
void DescribeCommon( OFX::ImageEffectDescriptor& desc, const char* label )
{
	desc.setLabels( label, label, label );
	desc.setPluginGrouping( "Stoatworks" );
	desc.setPluginDescription(
		"Vector artwork, rasterised at the size it is being seen at. Zooming in "
		"rebuilds the picture rather than magnifying it, so a drawing stays as "
		"sharp at 30x as at 1:1. Draw fills, strokes or both; reveal strokes "
		"along their own length; recolour, isolate and animate.\n\n"
		"SVG only. Text must be converted to outlines: live <text> is not "
		"rendered." );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	desc.setSingleInstance( false );
	desc.setHostFrameThreading( false );
	desc.setSupportsMultiResolution( false );

	// Not tiled. The raster is built for the whole frame's transform in one go;
	// a tile would either rebuild it per tile or need the untiled bounds anyway.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderTwiceAlways( false );
	desc.setSupportsMultipleClipPARs( false );
	desc.setRenderThreadSafety( OFX::eRenderInstanceSafe );
}

mDeclarePluginFactory( BurinSourceFactory, {}, {} );

void BurinSourceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	DescribeCommon( desc, "Burin" );
	desc.addSupportedContext( OFX::eContextGenerator );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void BurinSourceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	DescribeParams( desc, false );
}

OFX::ImageEffect* BurinSourceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new BurinOFXPlugin( handle, false );
}

mDeclarePluginFactory( BurinOverFactory, {}, {} );

void BurinOverFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	DescribeCommon( desc, "Burin Over" );
	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void BurinOverFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	DescribeParams( desc, true );
}

OFX::ImageEffect* BurinOverFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new BurinOFXPlugin( handle, true );
}
} // namespace

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static BurinSourceFactory* sourceFactory =
		new BurinSourceFactory( kSourceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	static BurinOverFactory* overFactory =
		new BurinOverFactory( kOverIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( sourceFactory );
	ids.push_back( overFactory );
}
