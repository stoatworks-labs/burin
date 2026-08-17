#pragma once

#include <cstdint>

/**
    Host parameters, and what they mean.

    ## Two kinds of parameter, and the difference is load-bearing

    Most of the fleet declares **every** numeric parameter as a plain 0..1
    `FF_TYPE_STANDARD` float and does the conversion here, because
    `CFFGLPluginManager::SetParamInfo` clamps an `FF_TYPE_STANDARD` default into
    0..1 *before* returning, and `SetParamRange` can only be called afterwards.
    There is no `SetParamDefault`, so a parameter declared in shapes cannot
    declare a default in shapes: 12 becomes 1, silently.

    **That clamp applies to `FF_TYPE_STANDARD` only.** The SDK's `SetParamInfo`
    guards it with `if( pType == FF_TYPE_STANDARD )` and nothing else, so an
    `FF_TYPE_INTEGER` default passes through untouched and `SetParamRange` is
    free to widen it afterwards. The two shape indices here — First Shape and
    Shape Count — are therefore real integers.

    They have to be, for the same reason flipbook's grid does: those two are
    *counting somebody else's file*. Shape 14 of a drawing is shape 14, and a
    0..1 slider that lands on 13 or 15 either side of it makes the isolate
    control useless on every file it is pointed at. Everything continuous stays
    0..1 and is converted below, with curves wherever the useful part of a range
    is bunched at one end.

    ## Zoom is exponential, and that is not a nicety either

    A zoom control that runs 0.1..10 linearly spends 90% of its travel above
    1:1. Worse, the *interesting* part of a vector zoom is the part a bitmap
    cannot do — going in far enough that a raster would break up — and a linear
    slider gives that a sliver at the top end. `ZoomFromParam` is exponential
    over six octaves with 1:1 landing exactly at the centre detent, so "the size
    it says it is" is a place you can find by dragging to the middle rather than
    by reading this file.
*/
namespace burin
{
/// Parameter ids. The declaration order in Burin.cpp is the order they
/// appear in the host, and the groups depend on consecutive ids staying
/// consecutive -- SetParamGroup collapses *runs* of same-group parameters, so
/// reordering these silently splits a group into two.
enum ParamId : unsigned int
{
	// Document
	PT_FILE = 0,
	PT_BOUNDS,
	PT_FIT,
	PT_DETAIL,

	// Style
	PT_DRAW,
	PT_STROKE_SCALE,
	PT_STROKE_MIN,
	PT_RECOLOUR,
	PT_COL_R,
	PT_COL_G,
	PT_COL_B,
	PT_COL_SPREAD,

	// Isolate
	PT_FIRST,
	PT_COUNT,

	// Reveal
	PT_REVEAL_MODE,
	PT_REVEAL,
	PT_REVEAL_STAGGER,
	PT_REVEAL_ORDER,
	PT_REVEAL_FILL,

	// Motion
	PT_SYNC,
	PT_RATE,
	PT_PHASE,
	PT_RESET,
	PT_WAVE,
	PT_ZOOM,
	PT_ZOOM_MOVE,
	PT_POS_X,
	PT_POS_Y,
	PT_DRIFT_X,
	PT_DRIFT_Y,
	PT_ROTATE,
	PT_SPIN,

	// Colour
	PT_TINT_R,
	PT_TINT_G,
	PT_TINT_B,
	PT_OPACITY,
	PT_BACK_R,
	PT_BACK_G,
	PT_BACK_B,
	PT_BACK_OPACITY,

	// Output. Declared by both plugins so that a composition can be moved
	// between them without the parameter list shifting underneath it; the
	// source plugin has nothing to mix against and ignores it.
	PT_MIX,

	// Preset. Declared after the real controls so their ids — which a saved
	// composition refers to — do not shift under existing users.
	PT_PRESET,

	PT_COUNT_ALL
};

/// The largest raster the plugin will build, on either side, in pixels.
///
/// Conservative on purpose. The true ceiling is `GL_MAX_TEXTURE_SIZE`, which is
/// a runtime query and is 16384 on most of what runs Resolume, and crossing it
/// gives no error worth reading: `glTexImage2D` fails and the sampler returns
/// black, which looks exactly like a file that did not load. 8192 is safe
/// everywhere and is already four times a 4K frame on each axis.
///
/// This is also what makes the deep zoom bounded rather than unbounded. The
/// raster covers the *visible* part of the document, so zooming in does not
/// grow it — see Raster.h.
constexpr int kMaxRasterPx = 8192;

/// The most shapes the isolate controls will address. Not a limit of anything
/// downstream — the document holds what it holds — but an unbounded spinner is
/// a way to type 40000 by accident, and a drawing with more than four thousand
/// separate shapes is a map, not a graphic.
constexpr int kMaxShapes = 4096;

/// Which box the drawing is fitted to.
///
/// **Viewport** is what the file declares: its width and height, or its viewBox
/// mapped through them. **Content** is the union of the shapes' own bounding
/// boxes, expanded for stroke width.
///
/// They are genuinely different questions. A logo exported with its artboard
/// intact wants Viewport, because the whitespace around it is a decision
/// somebody made and cropping to the ink would throw it away. One path pulled
/// out of a larger drawing, or anything exported tight, wants Content — its
/// viewport may be a metre of empty space with a mark in one corner.
enum class Bounds : int
{
	Viewport = 0,
	Content,

	Count
};

/// How that box is fitted into the output raster before Zoom is applied.
///
/// **Fit** contains the whole box with its aspect intact, which is what a
/// drawing almost always wants. **Fill** covers the raster and crops the
/// overflow. **Stretch** maps the box onto the raster exactly, aspect be
/// damned, which is right for a drawing authored at the output's own shape and
/// wrong for everything else.
enum class Fit : int
{
	Fit = 0,
	Fill,
	Stretch,

	Count
};

/// Which halves of the drawing are painted.
///
/// This is the control the plugin exists for as much as the zoom is. nanosvg
/// keeps fill and stroke as separate paint on the same shape, and
/// `nsvgRasterize` tests each independently, so turning one off is exact —
/// not a colour key, not an edge detect, the actual fill and the actual
/// stroke. **Stroke** on a drawing that was never meant to be seen as
/// linework is the interesting one: it gives the wireframe of an artwork
/// whose author only ever composed it as solids.
enum class Draw : int
{
	Both = 0,
	FillOnly,
	StrokeOnly,

	Count
};

/// What the override colour replaces.
///
/// **Off** leaves the file's own colours alone, and is the default because a
/// drawing arrives coloured. The others replace the paint's RGB and keep its
/// alpha, so a shape the file made half-transparent stays half-transparent.
enum class Recolour : int
{
	Off = 0,
	Fill,
	Stroke,
	Both,

	Count
};

/// How the write-on runs.
///
/// **On** draws the strokes in along their own length. **Off** retracts them:
/// the drawn end runs back toward where the pen started, so the drawing
/// un-draws itself rather than fading or wiping.
///
/// Off is a *retraction* and not an erase-from-the-start, and that is a
/// limitation rather than a preference. nanosvg's dash walk always begins in
/// the "on" state — `dashState` is initialised to 1 before the offset is
/// consumed, so an offset can only shorten the first drawn run, never turn it
/// into a gap. A two-entry dash can therefore express "the first `p` of this
/// path" and cannot express "the last `p` of it". Erasing from the start would
/// need a three-entry pattern whose odd count doubles nanosvg's period, and it
/// would leave a stub of stroke at the origin. Reverse order gets most of the
/// same look honestly.
///
/// **Hold** is the mode that makes the other two usable in a show — the
/// progress parameter is driven by hand or by a keyframe rather than by the
/// clock, which is what an operator wants when the reveal has to land on a
/// specific bar rather than run free.
enum class RevealMode : int
{
	None = 0,
	On,
	Off,
	Hold,

	Count
};

/// The order the shapes are revealed in when Stagger is not zero.
///
/// **Document** is the order the file lists them, which is the order they were
/// drawn in and usually the order they were *meant* to be drawn in. **Longest**
/// and **Shortest** sort by path length, which turns a technical drawing into
/// something that builds structure-first or detail-first. **Scatter** is seeded
/// off the shape index so it is stable frame to frame — a random order that
/// re-rolled every frame would be a strobe, not an order.
enum class RevealOrder : int
{
	Document = 0,
	Reverse,
	Longest,
	Shortest,
	Scatter,

	Count
};

/// What the motion parameters are measured against.
///
/// Free: cycles per second on the host's clock. Beat and Bar: cycles per beat
/// or per bar, locked to the host's BPM clock. Manual: the clock is ignored
/// entirely and Phase drives the motion, which is the mode for keyframing
/// against an edit rather than free-running.
enum class SyncMode : int
{
	Free = 0,
	Beat,
	Bar,
	Manual,

	Count
};

/// The shape of the motion applied to Zoom Move and Drift.
///
/// **Sine** eases at both ends and is the default: a drawing that breathes.
/// **Triangle** travels at constant speed and turns hard. **Ramp** does not
/// come back — it runs one way and jumps, which is right for a drift across a
/// large drawing and wrong for a zoom unless the artwork is self-similar.
/// **Pulse** is a square wave, for cutting between two framings on the beat.
///
/// Spin does not read this. A rotation rate is a rate: it is always a ramp, and
/// an oscillating one is what Rotate plus a Sine drift already is.
enum class Wave : int
{
	Sine = 0,
	Triangle,
	Ramp,
	Pulse,

	Count
};

/// Round an option parameter's value to an element index and clamp it into
/// range. Option parameters do **not** hold 0..1: they hold the element value
/// the operator chose. The clamp is for a stale composition naming an element
/// that no longer exists.
int Option( float value, int count );

/// Zoom, as a multiple of whatever Fit decided. 1/8..8, exponential, with 1.0
/// landing exactly at 0.5. Six octaves, because the point of a vector source is
/// that going in a long way stays sharp and a range that stopped at 2x would
/// not demonstrate the thing the plugin is for.
float ZoomFromParam( float value );

/// The zoom excursion, in octaves either side of Zoom. 0..3, squared, so the
/// bottom of the slider is a slow breath rather than a lurch.
float ZoomMoveFromParam( float value );

/// Pan, in fractions of the fitted box. -1..1, zero at the centre of the
/// slider. Deliberately *not* in frame widths: panning a drawing that is twice
/// as wide as it is tall by "half a frame" means two different distances on the
/// two axes, and the operator is looking at the drawing, not the raster.
float PanFromParam( float value );

/// Drift excursion, in fractions of the fitted box. 0..1, squared.
float DriftFromParam( float value );

/// Rotation in radians. -pi..pi, zero at the centre of the slider.
float RotateFromParam( float value );

/// Spin rate, in turns per cycle of the motion clock. -4..4, cubed around zero
/// so that a slow drift is reachable — the useful settings for a background
/// element are all within a tenth of a turn and a linear control gives them
/// two pixels of travel. Zero is exactly at the centre detent.
float SpinFromParam( float value );

/// Motion rate: cycles per second, or per beat or per bar under those Sync
/// modes. 0.01..4, exponential. The low end matters more than the high here —
/// a drawing that completes a breath every forty seconds is a different and
/// more useful thing than one that completes four a second.
float RateFromParam( float value );

/// Stroke width, as a multiple of what the file declared. 0..8, squared, with
/// 1.0 at the centre detent. Zero is a real setting and means "no stroke",
/// which is not the same as Draw: Fill Only — this one still lets the reveal
/// run and the recolour apply, it just draws the stroke at no width.
float StrokeScaleFromParam( float value );

/// Minimum stroke width in **device pixels**, applied after scaling. 0..4.
///
/// The guard for the trap in nanosvg's rasteriser: it skips a stroke entirely
/// when `strokeWidth * scale <= 0.01`, so a hairline in a drawing zoomed out
/// does not thin, it *vanishes*, one shape at a time as you pull back. Holding
/// a floor of about a pixel keeps a technical drawing legible at small sizes.
/// Zero disables the floor and gives nanosvg's own behaviour.
float StrokeMinFromParam( float value );

/// How far the override colour walks across the shapes, in hue turns. -1..1,
/// zero at the centre. At zero every recoloured shape is the same colour; at
/// 1 the drawing runs through the whole wheel once from first shape to last.
float ColourSpreadFromParam( float value );

/// Reveal progress, 0..1 straight through. The one parameter that is genuinely
/// linear: it is a fraction of a path's length and any curve on it would make
/// the pen speed up or slow down for no reason the operator asked for.
float RevealFromParam( float value );

/// Per-shape reveal offset, as a fraction of one shape's own draw time. 0..4,
/// squared, with a dead zone at the bottom — a hundredth of a shape of stagger
/// is invisible on a still and reads as jitter in motion, which is worse than
/// either end. At 1 each shape starts as the one before it finishes.
float RevealStaggerFromParam( float value );

/// The window over which a shape's fill fades up, as a fraction of that shape's
/// reveal. 0..1.
///
/// A fill cannot be drawn on — there is no length to run along, it is an area —
/// so it fades. At 0 the fill is simply present from the start, which is right
/// for a drawing whose strokes sit on top of flat colour. At 1 it fades across
/// the whole reveal. The default is the top third, so the line arrives first
/// and the colour follows it in, which is what somebody drawing would do.
float RevealFillFromParam( float value );

/// Raster detail, as a multiple of the output's own resolution. 0.25..2,
/// exponential, 1.0 at the centre detent.
///
/// Above 1 the drawing is rasterised finer than the frame and downsampled,
/// which buys better antialiasing than nanosvg's five-times vertical
/// subsampling gives on its own — worth it on thin diagonal linework. Below 1
/// it is the release valve for a heavy file: a drawing of ten thousand paths
/// with the write-on running is rasterised **every frame**, and halving the
/// detail quarters that cost.
float DetailFromParam( float value );

/// The seeded scatter used by RevealOrder::Scatter. Stable for a given shape
/// count, so the order does not change as the clock advances.
uint32_t ScatterKey( int shapeIndex, int shapeCount );

} // namespace burin
