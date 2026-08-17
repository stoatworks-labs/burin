#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Included rather than forward-declared, because `ShapeInfo` stores `NSVGpaint`
// **by value** and that type is a union of a colour and a pointer. Storing it
// as its own type is the whole point: see the note on paint in ShapeInfo.
#include "SvgLib.h"

/**
    The document: an SVG parsed once, plus the two things nanosvg does not
    record about it.

    ## Why this is not just an NSVGimage

    Two of them, and both are load-bearing.

    **A pristine copy of every shape's paint.** Everything this plugin does to
    the picture — fill only, stroke only, recolour, isolate a range, reveal
    along the length — is done by *writing into* the live `NSVGshape` structs
    immediately before `nsvgRasterize`, because that is where nanosvg reads
    them. There is no render-time parameter block to pass instead. So the parsed
    image is working memory, not a record of the file, and something has to
    remember what the file actually said. That is `ShapeInfo`. Every frame
    restores from it and then applies that frame's settings; nothing accumulates
    and no setting can be a one-way door.

    **The arc length of every shape.** nanosvg measures nothing: `NSVGpath` has
    points and a bounding box and no length. The write-on needs one, because a
    dash pattern is specified in document units and "reveal this path 40% of the
    way along" is a question about distance. `Measure()` flattens the cubics and
    sums the segments.

    ## Length is per shape, not per subpath, and it is the longest

    nanosvg's dash state is set up inside its per-path loop, so the pattern
    **restarts at every subpath** and there is exactly one dash array per shape.
    A shape of three subpaths therefore reveals all three at once, at the same
    absolute rate — a pen moving at constant speed over three strokes at the
    same time, which is the natural reading and happens to be the only one
    available.

    What follows is that a shape is finished when its *longest* subpath is, so
    that is the length recorded. Recording the total would finish the reveal
    early and leave the long stroke short; recording the shortest would leave
    the pattern running long after everything was drawn.

    ## Bounds: the viewport, or the ink

    `viewport` is what the file declares — its `width`/`height`, or the
    `viewBox` mapped through them. `content` is the union of the shape bounding
    boxes, expanded for stroke width. They are different questions and both get
    asked: a logo exported with the artboard intact wants the viewport, because
    the whitespace is a decision somebody made; the same logo exported tight, or
    one path pulled out of a bigger drawing, wants the content box. Fit picks
    between them.

    Note that nanosvg's per-shape `bounds` is the bound of the **path**, with no
    allowance for the stroke drawn on it — a 40-unit-wide stroke hangs 20 units
    outside. `Measure()` expands each one, which is why a stroke-heavy drawing
    fitted to Content does not lose its outermost edge off the frame.
*/
namespace rasterizer
{
/// What the file said about one shape, before this frame's settings were
/// written over it. Restored at the top of every rasterise.
///
/// ## The paint structs are copied WHOLE, and that is not tidiness
///
/// `NSVGpaint` is a `signed char type` beside an **anonymous union** of an
/// `unsigned int color` and an `NSVGgradient* gradient`. Two consequences, both
/// of which bite:
///
/// **Storing the colour as a `uint32_t` truncates a gradient to its low half.**
/// On any 64-bit build the pointer is twice the width of the field, so a
/// snapshot taken through `.color` cannot restore a gradient-filled shape — it
/// restores half a pointer. Copying the union at its own width is the only
/// correct way to hold it.
///
/// **`nsvg__deletePaint` frees the gradient only while `type` still says
/// gradient.** So a shape that is left recoloured — `type` switched to
/// `NSVG_PAINT_COLOR`, pointer overwritten — is a gradient leaked *and* a
/// pointer destroyed. `Document::Release()` therefore calls `ResetShapes()`
/// before `nsvgDelete`, which puts every paint back to the file's own before
/// nanosvg walks them. The plugin recolours the live image every frame, so
/// without that line every unload leaks one allocation per gradient in the
/// drawing, inside somebody else's process.
///
/// The gradient itself is owned by the `NSVGimage` and nothing here ever frees
/// one; this holds the pointer, and the image outlives every restore.
struct ShapeInfo
{
	NSVGpaint fill{};  ///< the file's own paint, union and all
	NSVGpaint stroke{};///< kept as the file had it, so "stroke only" restores a
	                   ///< fill that was NONE to NONE rather than inventing one

	float strokeWidth = 1.0f;
	float opacity     = 1.0f;
	float fillOpacity = 1.0f;///< derived: the alpha byte of the fill colour, 0..1

	unsigned char flags = 0;///< NSVG_FLAGS_VISIBLE as parsed

	/// The file's **own** dash pattern, if it declared one. Kept because the
	/// reveal writes the dash array to do its work, and a restore that zeroed
	/// the count would quietly turn every dashed border in the drawing solid —
	/// a change to the artwork, made by a feature that was switched off.
	float dashArray[ 8 ]    = { 0, 0, 0, 0, 0, 0, 0, 0 };
	float dashOffset        = 0.0f;
	signed char dashCount   = 0;

	/// Longest subpath, in document units. Zero for a shape with no stroke
	/// geometry worth measuring, which the reveal reads as "instant".
	float length = 0.0f;

	/// Path bounds expanded by half the stroke width. See the class comment.
	float bounds[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };

	/// Position in the file, 0-based, in document order. Redundant with the
	/// vector index today and written down anyway, because the ordering the
	/// reveal uses is a *sort* and the sorted position must not be mistaken for
	/// the document one.
	int index = 0;
};

/// A rectangle in document units. minx/miny/maxx/maxy, matching nanosvg's
/// bounds layout so the two can be passed to each other without a shuffle.
struct Rect
{
	float minx = 0.0f, miny = 0.0f, maxx = 0.0f, maxy = 0.0f;

	float Width() const { return maxx - minx; }
	float Height() const { return maxy - miny; }
	bool Valid() const { return maxx > minx && maxy > miny; }
};

/// Elements nanosvg silently drops. Counted while scanning the source text so
/// the operator is told *why* their file came out empty, rather than being left
/// with a plugin that appears to do nothing.
struct Unsupported
{
	int text    = 0;///< the big one: no text support at all, not even a stub
	int image   = 0;
	int use     = 0;
	int clips   = 0;///< clipPath + mask
	int filters = 0;
	int pattern = 0;

	bool Any() const { return text || image || use || clips || filters || pattern; }
};

class Document
{
public:
	Document() = default;
	~Document();

	Document( const Document& )            = delete;
	Document& operator=( const Document& ) = delete;
	Document( Document&& ) noexcept;
	Document& operator=( Document&& ) noexcept;

	/// Parse `path`. On failure the document is left invalid and `Note()` says
	/// why; there is no error code, because every caller does the same thing
	/// with one (log it, draw nothing) and a code would be a second way to
	/// spell the message.
	bool LoadFile( const std::string& path );

	/// Parse from memory. `text` is copied before parsing: nanosvg's `nsvgParse`
	/// writes into the buffer it is given — it null-terminates attribute values
	/// in place — so handing it a caller's string would corrupt it. This is the
	/// entry point the harness and the OFX build use.
	bool LoadString( const std::string& text, const std::string& name );

	bool Valid() const { return image_ != nullptr && !shapes_.empty(); }

	NSVGimage* Image() const { return image_; }
	const std::vector< ShapeInfo >& Shapes() const { return shapes_; }
	int ShapeCount() const { return static_cast< int >( shapes_.size() ); }

	/// Shape `i` of the live image, for writing this frame's settings into.
	/// Indexed rather than walked, because every consumer wants shape *n* and
	/// nanosvg only offers a linked list.
	NSVGshape* Shape( int i ) const;

	const Rect& Viewport() const { return viewport_; }
	const Rect& Content() const { return content_; }

	/// Restore every shape to what the file said. Called at the top of each
	/// rasterise, before Style and Reveal write over it.
	void ResetShapes() const;

	const std::string& Note() const { return note_; }
	const Unsupported& Missing() const { return missing_; }

private:
	void Adopt( NSVGimage* image, const std::string& name );
	void Measure();
	void Release();

	NSVGimage* image_ = nullptr;
	std::vector< ShapeInfo > shapes_;
	std::vector< NSVGshape* > order_;///< index -> shape, so Shape(i) is O(1)
	Rect viewport_;
	Rect content_;
	std::string note_;
	Unsupported missing_;
};

/// Count the elements nanosvg will drop, by scanning the source text.
///
/// Text, not a parse tree, because nanosvg does not build one for the elements
/// it does not know — they are gone before anything is queryable. It is
/// therefore a *heuristic*: it looks for the element open tags, and it would
/// count one inside a comment or a CDATA block. Over-reporting an ignored text
/// element is a message that says "some of this file did not render"; the
/// alternative is a blank frame and no message at all.
Unsupported ScanUnsupported( const std::string& text );

/// Flattened length of one nanosvg subpath, in document units.
///
/// Fixed subdivision rather than nanosvg's adaptive flattening, on purpose.
///
/// The adaptive version's tolerance is in *device* pixels, so the polyline it
/// builds — and therefore the length it implies — changes with zoom. A write-on
/// measured that way would have its end point move as you zoomed, which is
/// unusable. This is scale-independent instead.
///
/// The two therefore disagree, by a fraction of a percent in one direction or
/// the other depending on how far in the frame is. That is why full and empty
/// are **special-cased** rather than being the ends of the dash range: at
/// progress 1 the dash is switched off entirely and the stroke draws whole, and
/// at 0 the stroke is switched off. So the endpoints are exact by construction
/// and the flattening error only ever affects where the pen is mid-stroke,
/// where a half-percent is invisible. See Reveal.cpp.
float SubpathLength( const float* pts, int npts );

} // namespace rasterizer
