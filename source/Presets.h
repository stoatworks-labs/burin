#pragma once

/**
    Factory presets: named ways of *treating* a drawing.

    The important thing about this table is what is not in it. A preset here
    never touches the **Document** group — not the file, and not the Bounds
    control that says whether the drawing is framed by its artboard or by its
    ink. Those describe the operator's own material: a preset that reached into
    them would take a logo framed the way its designer framed it and re-crop it,
    which is not a look, it is breakage.

    It also leaves alone the **Isolate** range and **Sync** and **Mix**. Which
    shapes are interesting is a fact about the file — shape 14 of one drawing has
    nothing to do with shape 14 of another — and what clock the motion runs on
    and how much effect is wanted are decisions about the show. Sync in
    particular cannot be in the table: the FFGL build offers beat modes the OFX
    build has no clock for, so an index would mean different things in different
    hosts.

    The values live in the same parameter space both builds expose, so ONE table
    drives both and a preset looks identical in Resolume and Resolve. Plain data
    only; the application machinery lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this table:
    it means "the sliders are the truth".
*/

namespace rasterizer
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIds and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kFit,
	kDetail,
	kDraw,
	kStrokeScale,
	kStrokeMin,
	kRecolour,
	kColR,
	kColG,
	kColB,
	kColSpread,
	kRevealMode,
	kReveal,
	kRevealStagger,
	kRevealOrder,
	kRevealFill,
	kRate,
	kWave,
	kZoom,
	kZoomMove,
	kDriftX,
	kDriftY,
	kRotate,
	kSpin,
	kTintR,
	kTintG,
	kTintB,
	kOpacity,
	kBackR,
	kBackG,
	kBackB,
	kBackOpacity,

	kParamCount
};

struct Preset
{
	const char* name;
	float values[ kParamCount ];
};

/// Zoom 0.5 is 1:1 — the control is exponential over six octaves with the
/// detent at unity. Written out here because a table of bare numbers is
/// otherwise impossible to read.
inline constexpr float kZoomUnity = 0.5f;

/// Likewise: the centre detent for the signed controls, and the stroke scale's
/// own two-piece curve, both of which put "leave it alone" at 0.5.
inline constexpr float kCentre = 0.5f;

inline constexpr Preset kPresets[] = {
	// The drawing as its author left it: every paint the file declares, at the
	// widths it declares, fitted to the frame and still. Named so it stays
	// reachable after any amount of fiddling.
	{ "As Drawn",
	  { /*Fit*/ 0, /*Detail*/ kCentre,
	    /*Draw*/ 0, /*StrokeScale*/ kCentre, /*StrokeMin*/ 0.25f,
	    /*Recolour*/ 0, /*Col*/ 1.0f, 1.0f, 1.0f, /*Spread*/ kCentre,
	    /*RevealMode*/ 0, /*Reveal*/ 1.0f, /*Stagger*/ 0.0f, /*Order*/ 0, /*Fill*/ 0.33f,
	    /*Rate*/ 0.55f, /*Wave*/ 0, /*Zoom*/ kZoomUnity, /*ZoomMove*/ 0.0f,
	    /*Drift*/ 0.0f, 0.0f, /*Rotate*/ kCentre, /*Spin*/ kCentre,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// The one that shows what the plugin is for: in far enough that a bitmap
	// would have fallen apart, drifting slowly so the sharpness is legible as
	// sharpness rather than as a still. Content bounds are deliberately NOT set
	// here even though they would frame it better -- see the note at the top.
	{ "Close Work",
	  { /*Fit*/ 0, /*Detail*/ kCentre,
	    /*Draw*/ 0, /*StrokeScale*/ kCentre, /*StrokeMin*/ 0.25f,
	    /*Recolour*/ 0, /*Col*/ 1.0f, 1.0f, 1.0f, /*Spread*/ kCentre,
	    /*RevealMode*/ 0, /*Reveal*/ 1.0f, /*Stagger*/ 0.0f, /*Order*/ 0, /*Fill*/ 0.33f,
	    /*Rate*/ 0.30f, /*Wave*/ 0, /*Zoom*/ 0.78f, /*ZoomMove*/ 0.30f,
	    /*Drift*/ 0.22f, 0.14f, /*Rotate*/ kCentre, /*Spin*/ kCentre,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// The wireframe of artwork whose author never drew one. Stroke only, with
	// the width floor up so a hairline survives being zoomed out, recoloured to
	// one cold colour on a dark ground.
	{ "Blueprint",
	  { /*Fit*/ 0, /*Detail*/ kCentre,
	    /*Draw*/ 2, /*StrokeScale*/ 0.42f, /*StrokeMin*/ 0.45f,
	    /*Recolour*/ 3, /*Col*/ 0.55f, 0.80f, 1.0f, /*Spread*/ kCentre,
	    /*RevealMode*/ 0, /*Reveal*/ 1.0f, /*Stagger*/ 0.0f, /*Order*/ 0, /*Fill*/ 0.33f,
	    /*Rate*/ 0.40f, /*Wave*/ 0, /*Zoom*/ kZoomUnity, /*ZoomMove*/ 0.0f,
	    /*Drift*/ 0.0f, 0.0f, /*Rotate*/ kCentre, /*Spin*/ kCentre,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.04f, 0.09f, 0.20f, /*BackOp*/ 1.0f } },

	// The headline trick: the drawing draws itself, shape after shape, with the
	// colour arriving behind the line. Reveal is left at 1 rather than 0 --
	// clicking a preset should show you the finished thing, and the operator
	// pulls the slider back or switches Sync to a beat to run it.
	{ "Write On",
	  { /*Fit*/ 0, /*Detail*/ kCentre,
	    /*Draw*/ 0, /*StrokeScale*/ kCentre, /*StrokeMin*/ 0.35f,
	    /*Recolour*/ 0, /*Col*/ 1.0f, 1.0f, 1.0f, /*Spread*/ kCentre,
	    /*RevealMode*/ 1, /*Reveal*/ 1.0f, /*Stagger*/ 0.55f, /*Order*/ 0, /*Fill*/ 0.45f,
	    /*Rate*/ 0.42f, /*Wave*/ 0, /*Zoom*/ kZoomUnity, /*ZoomMove*/ 0.0f,
	    /*Drift*/ 0.0f, 0.0f, /*Rotate*/ kCentre, /*Spin*/ kCentre,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// The same machinery ordered by size rather than by document order, which
	// builds structure first and detail last -- the way a drawing is actually
	// made, and quite different to watch from the file's own order.
	{ "Rough to Fine",
	  { /*Fit*/ 0, /*Detail*/ kCentre,
	    /*Draw*/ 2, /*StrokeScale*/ 0.56f, /*StrokeMin*/ 0.35f,
	    /*Recolour*/ 0, /*Col*/ 1.0f, 1.0f, 1.0f, /*Spread*/ kCentre,
	    /*RevealMode*/ 1, /*Reveal*/ 1.0f, /*Stagger*/ 0.70f, /*Order*/ 2, /*Fill*/ 0.0f,
	    /*Rate*/ 0.35f, /*Wave*/ 0, /*Zoom*/ kZoomUnity, /*ZoomMove*/ 0.0f,
	    /*Drift*/ 0.0f, 0.0f, /*Rotate*/ kCentre, /*Spin*/ kCentre,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// Stroke only, fat, and walked around the hue wheel shape by shape. The
	// spread is the control worth finding first on any drawing with more than a
	// dozen paths in it.
	{ "Neon Walk",
	  { /*Fit*/ 0, /*Detail*/ 0.62f,
	    /*Draw*/ 2, /*StrokeScale*/ 0.62f, /*StrokeMin*/ 0.40f,
	    /*Recolour*/ 3, /*Col*/ 1.0f, 0.20f, 0.55f, /*Spread*/ 0.78f,
	    /*RevealMode*/ 0, /*Reveal*/ 1.0f, /*Stagger*/ 0.0f, /*Order*/ 0, /*Fill*/ 0.33f,
	    /*Rate*/ 0.50f, /*Wave*/ 0, /*Zoom*/ kZoomUnity, /*ZoomMove*/ 0.0f,
	    /*Drift*/ 0.0f, 0.0f, /*Rotate*/ kCentre, /*Spin*/ kCentre,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// A background element: large, slow, turning, never still and never
	// demanding. Spin is a hair off the detent rather than obviously moving.
	{ "Slow Turn",
	  { /*Fit*/ 0, /*Detail*/ kCentre,
	    /*Draw*/ 0, /*StrokeScale*/ kCentre, /*StrokeMin*/ 0.25f,
	    /*Recolour*/ 0, /*Col*/ 1.0f, 1.0f, 1.0f, /*Spread*/ kCentre,
	    /*RevealMode*/ 0, /*Reveal*/ 1.0f, /*Stagger*/ 0.0f, /*Order*/ 0, /*Fill*/ 0.33f,
	    /*Rate*/ 0.18f, /*Wave*/ 0, /*Zoom*/ 0.60f, /*ZoomMove*/ 0.18f,
	    /*Drift*/ 0.10f, 0.10f, /*Rotate*/ kCentre, /*Spin*/ 0.56f,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 0.85f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// Cutting between two framings on the beat. Pulse is the only waveform that
	// reads as a cut rather than as a move, and this is what it is for.
	{ "Beat Cut",
	  { /*Fit*/ 0, /*Detail*/ kCentre,
	    /*Draw*/ 0, /*StrokeScale*/ kCentre, /*StrokeMin*/ 0.30f,
	    /*Recolour*/ 0, /*Col*/ 1.0f, 1.0f, 1.0f, /*Spread*/ kCentre,
	    /*RevealMode*/ 0, /*Reveal*/ 1.0f, /*Stagger*/ 0.0f, /*Order*/ 0, /*Fill*/ 0.33f,
	    /*Rate*/ 0.63f, /*Wave*/ 3, /*Zoom*/ 0.66f, /*ZoomMove*/ 0.42f,
	    /*Drift*/ 0.18f, 0.0f, /*Rotate*/ kCentre, /*Spin*/ kCentre,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },
};

inline constexpr int kPresetCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

/// Including Custom at element 0.
inline constexpr int kOptionCount = kPresetCount + 1;

} // namespace presets
} // namespace rasterizer
