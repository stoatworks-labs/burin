#include "Document.h"

#include "SvgLib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace burin
{
namespace
{
/// Subdivisions per cubic segment when measuring. Twenty-four is well past the
/// point where the sum stops moving for any curve a drawing program emits — the
/// difference between 24 and 96 on a full circle built from four cubics is
/// under a part in ten thousand — and it is a fixed cost paid once per load
/// rather than per frame.
constexpr int kMeasureSteps = 24;

/// How much of the stroke width hangs outside the path, as a multiple of the
/// half width.
///
/// One — that is, exactly half the stroke width — is the geometrically right
/// answer for butt and round caps and round joins, which is what a drawing
/// program emits unless somebody went looking. A **miter** join on a sharp
/// corner reaches out to `miterLimit` half-widths and nanosvg defaults that to
/// 4, so a drawing with a genuine spike can still poke past the content box by
/// a few units. That is the deliberate trade: covering the worst case would
/// mean insetting every ordinary drawing by four stroke widths of empty margin,
/// which is a visible loss on every file to protect against a rare one.
constexpr float kStrokeOutset = 1.0f;

float Bez( float a, float b, float c, float d, float t )
{
	const float mt = 1.0f - t;
	return mt * mt * mt * a + 3.0f * mt * mt * t * b + 3.0f * mt * t * t * c + t * t * t * d;
}

/// Case-insensitive search for `needle` in `hay` starting at `from`.
size_t FindNoCase( const std::string& hay, const char* needle, size_t from )
{
	const size_t n = std::strlen( needle );
	if( n == 0 || hay.size() < n )
		return std::string::npos;
	for( size_t i = from; i + n <= hay.size(); ++i )
	{
		size_t j = 0;
		while( j < n && std::tolower( static_cast< unsigned char >( hay[ i + j ] ) ) == std::tolower( static_cast< unsigned char >( needle[ j ] ) ) )
			++j;
		if( j == n )
			return i;
	}
	return std::string::npos;
}

/// Count occurrences of an element *open tag* — `<name` followed by whitespace,
/// `>` or `/`. The trailing check is what keeps `<use` from also matching a
/// hypothetical `<usemap`, and `<pattern` from matching nothing at all.
int CountElement( const std::string& text, const char* name )
{
	std::string open = "<";
	open += name;

	int count   = 0;
	size_t from = 0;
	for( ;; )
	{
		const size_t at = FindNoCase( text, open.c_str(), from );
		if( at == std::string::npos )
			break;
		const size_t after = at + open.size();
		if( after >= text.size() )
			break;
		const char c = text[ after ];
		if( c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r' )
			++count;
		from = after;
	}
	return count;
}
} // namespace

float SubpathLength( const float* pts, int npts )
{
	if( pts == nullptr || npts < 4 )
		return 0.0f;

	float total = 0.0f;

	// nanosvg stores a subpath as x0,y0 then triples of (cp1, cp2, end), so the
	// cubic starting at point i spans pts[i*2 .. i*2+7] and i advances by 3.
	for( int i = 0; i < npts - 1; i += 3 )
	{
		const float* p = &pts[ i * 2 ];

		float px = p[ 0 ];
		float py = p[ 1 ];
		for( int s = 1; s <= kMeasureSteps; ++s )
		{
			const float t  = static_cast< float >( s ) / static_cast< float >( kMeasureSteps );
			const float qx = Bez( p[ 0 ], p[ 2 ], p[ 4 ], p[ 6 ], t );
			const float qy = Bez( p[ 1 ], p[ 3 ], p[ 5 ], p[ 7 ], t );
			total += std::sqrt( ( qx - px ) * ( qx - px ) + ( qy - py ) * ( qy - py ) );
			px = qx;
			py = qy;
		}
	}

	return total;
}

Unsupported ScanUnsupported( const std::string& text )
{
	Unsupported u;
	u.text    = CountElement( text, "text" ) + CountElement( text, "tspan" );
	u.image   = CountElement( text, "image" );
	u.use     = CountElement( text, "use" );
	u.clips   = CountElement( text, "clipPath" ) + CountElement( text, "mask" );
	u.filters = CountElement( text, "filter" );
	u.pattern = CountElement( text, "pattern" );
	return u;
}

Document::~Document()
{
	Release();
}

Document::Document( Document&& other ) noexcept
{
	*this = std::move( other );
}

Document& Document::operator=( Document&& other ) noexcept
{
	if( this != &other )
	{
		Release();
		image_        = other.image_;
		shapes_       = std::move( other.shapes_ );
		order_        = std::move( other.order_ );
		viewport_     = other.viewport_;
		content_      = other.content_;
		note_         = std::move( other.note_ );
		missing_      = other.missing_;
		other.image_  = nullptr;
	}
	return *this;
}

void Document::Release()
{
	if( image_ != nullptr )
	{
		// Put every paint back to the file's own BEFORE nanosvg walks them.
		//
		// This line is load-bearing and its absence is invisible. The render
		// path recolours the live image in place, and recolouring a
		// gradient-filled shape means writing a flat colour over the union that
		// holds the gradient pointer and switching `type` to
		// NSVG_PAINT_COLOR. `nsvg__deletePaint` frees the gradient only when
		// `type` still says gradient — so a document unloaded while recoloured
		// leaks one allocation per gradient, every time the operator points the
		// plugin at a different file, inside Resolume's process.
		ResetShapes();

		nsvgDelete( image_ );
		image_ = nullptr;
	}
	shapes_.clear();
	order_.clear();
}

NSVGshape* Document::Shape( int i ) const
{
	if( i < 0 || i >= static_cast< int >( order_.size() ) )
		return nullptr;
	return order_[ i ];
}

bool Document::LoadFile( const std::string& path )
{
	Release();
	note_.clear();
	missing_ = Unsupported();

	if( path.empty() )
	{
		note_ = "no file set";
		return false;
	}

	std::ifstream in( path, std::ios::binary );
	if( !in )
	{
		note_ = "could not open " + path;
		return false;
	}

	std::ostringstream buf;
	buf << in.rdbuf();
	const std::string text = buf.str();

	if( text.empty() )
	{
		note_ = path + " is empty";
		return false;
	}

	return LoadString( text, path );
}

bool Document::LoadString( const std::string& text, const std::string& name )
{
	Release();
	note_.clear();
	missing_ = ScanUnsupported( text );

	// nsvgParse writes into the buffer it is handed — it null-terminates
	// attribute values in place rather than copying them — so it gets its own
	// mutable copy and the caller's string is left alone.
	std::vector< char > mutableCopy( text.begin(), text.end() );
	mutableCopy.push_back( '\0' );

	// 96 dpi is the CSS reference pixel, which is what every drawing program
	// means by a "px" in an SVG. It only matters for physical units — a file
	// laid out in mm — and getting it wrong scales those files by 96/72.
	NSVGimage* parsed = nsvgParse( mutableCopy.data(), "px", 96.0f );

	if( parsed == nullptr )
	{
		note_ = "could not parse " + name;
		return false;
	}

	Adopt( parsed, name );
	return Valid();
}

void Document::Adopt( NSVGimage* image, const std::string& name )
{
	image_ = image;
	Measure();

	// The note is the one line the log and `burintest --doc` both print. It has to
	// carry the shape count (is the file even reaching us), the viewport (is the
	// scale sane) and — above all — what was dropped, because a file that is
	// mostly live text parses perfectly and renders nothing.
	std::ostringstream note;

	const char* base = name.c_str();
	for( const char* p = base; *p; ++p )
		if( *p == '/' || *p == '\\' )
			base = p + 1;

	note << base << ": " << shapes_.size() << ( shapes_.size() == 1 ? " shape" : " shapes" );
	note << ", viewport " << viewport_.Width() << "x" << viewport_.Height();

	if( shapes_.empty() )
		note << " — NOTHING TO DRAW";

	if( missing_.Any() )
	{
		note << "; ignored:";
		if( missing_.text )
			note << " " << missing_.text << " text (convert text to outlines)";
		if( missing_.use )
			note << " " << missing_.use << " use";
		if( missing_.image )
			note << " " << missing_.image << " image";
		if( missing_.clips )
			note << " " << missing_.clips << " clip/mask";
		if( missing_.filters )
			note << " " << missing_.filters << " filter";
		if( missing_.pattern )
			note << " " << missing_.pattern << " pattern";
	}

	note_ = note.str();
}

void Document::Measure()
{
	shapes_.clear();
	order_.clear();

	if( image_ == nullptr )
		return;

	// The viewport is what the file declares. nanosvg has already resolved a
	// viewBox against width/height by the time it sets these, so the origin is
	// always 0,0 in the coordinate space the shapes are in.
	viewport_ = Rect{ 0.0f, 0.0f, image_->width, image_->height };

	bool haveContent = false;
	Rect content{};

	int index = 0;
	for( NSVGshape* s = image_->shapes; s != nullptr; s = s->next, ++index )
	{
		ShapeInfo info;
		info.index       = index;
		info.fill        = s->fill;// whole union — see ShapeInfo
		info.stroke      = s->stroke;
		info.strokeWidth = s->strokeWidth;
		info.opacity     = s->opacity;
		info.flags       = s->flags;

		// Only meaningful for a flat fill; a gradient carries its alpha per
		// stop. The reveal's fill fade reads it, and on a gradient shape it
		// falls back to fully opaque, which is the right answer there because
		// the fade is applied to the shape opacity instead.
		info.fillOpacity = ( s->fill.type == NSVG_PAINT_COLOR )
		                       ? static_cast< float >( ( s->fill.color >> 24 ) & 0xFF ) / 255.0f
		                       : 1.0f;

		info.dashCount  = static_cast< signed char >( s->strokeDashCount );
		info.dashOffset = s->strokeDashOffset;
		for( int d = 0; d < 8; ++d )
			info.dashArray[ d ] = s->strokeDashArray[ d ];

		// The longest subpath, not the total: nanosvg restarts its dash pattern
		// at every subpath, so a shape is finished when its longest one is.
		float longest = 0.0f;
		for( NSVGpath* p = s->paths; p != nullptr; p = p->next )
			longest = std::max( longest, SubpathLength( p->pts, p->npts ) );
		info.length = longest;

		// nanosvg's bounds are the path's, with no allowance for the stroke
		// painted on it. A drawing fitted to Content would otherwise lose half
		// its outermost stroke off the edge of the frame.
		const float outset = ( s->stroke.type != NSVG_PAINT_NONE ) ? s->strokeWidth * 0.5f * kStrokeOutset : 0.0f;

		info.bounds[ 0 ] = s->bounds[ 0 ] - outset;
		info.bounds[ 1 ] = s->bounds[ 1 ] - outset;
		info.bounds[ 2 ] = s->bounds[ 2 ] + outset;
		info.bounds[ 3 ] = s->bounds[ 3 ] + outset;

		if( ( s->flags & NSVG_FLAGS_VISIBLE ) != 0 )
		{
			if( !haveContent )
			{
				content     = Rect{ info.bounds[ 0 ], info.bounds[ 1 ], info.bounds[ 2 ], info.bounds[ 3 ] };
				haveContent = true;
			}
			else
			{
				content.minx = std::min( content.minx, info.bounds[ 0 ] );
				content.miny = std::min( content.miny, info.bounds[ 1 ] );
				content.maxx = std::max( content.maxx, info.bounds[ 2 ] );
				content.maxy = std::max( content.maxy, info.bounds[ 3 ] );
			}
		}

		shapes_.push_back( info );
		order_.push_back( s );
	}

	// A document with no visible geometry still needs a content box that is not
	// degenerate, or every downstream division by its width is a division by
	// zero. Falling back to the viewport is the honest answer: there is nothing
	// to be tight around.
	content_ = haveContent && content.Valid() ? content : viewport_;

	// Same guard on the viewport. A file with no width/height and no viewBox
	// leaves nanosvg's width and height at zero.
	if( !viewport_.Valid() )
		viewport_ = content_.Valid() ? content_ : Rect{ 0.0f, 0.0f, 1.0f, 1.0f };
}

void Document::ResetShapes() const
{
	const int n = static_cast< int >( shapes_.size() );
	for( int i = 0; i < n; ++i )
	{
		NSVGshape* s = order_[ i ];
		if( s == nullptr )
			continue;

		const ShapeInfo& info = shapes_[ i ];

		// Whole struct, so a gradient pointer is restored at its own width
		// rather than at the 32 bits a colour field would have held.
		s->fill        = info.fill;
		s->stroke      = info.stroke;
		s->strokeWidth = info.strokeWidth;
		s->opacity     = info.opacity;
		s->flags       = info.flags;

		// Back to the file's own dash pattern — usually none, but a drawing is
		// entitled to have declared one and the reveal must hand it back when
		// it is finished. Restoring to *zero* here instead would turn every
		// dashed border in the artwork solid, which is a change to somebody's
		// drawing made by a feature they had switched off.
		s->strokeDashCount  = static_cast< char >( info.dashCount );
		s->strokeDashOffset = info.dashOffset;
		for( int d = 0; d < 8; ++d )
			s->strokeDashArray[ d ] = info.dashArray[ d ];
	}
}

} // namespace burin
