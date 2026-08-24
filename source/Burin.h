#pragma once

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

#include "Compose.h"
#include "Controls.h"
#include "Document.h"
#include "Motion.h"
#include "Presets.h"
#include "Raster.h"
#include "Reveal.h"
#include "Style.h"

/**
    Burin — vector artwork, rendered at the size it is being seen at.

    What is worth knowing about how it works is in the other headers and is not
    repeated here:

    - **Raster.h** — the scale ladder, why it always snaps *up*, why the raster
      covers the visible window rather than the document, and what that costs.
    - **Document.h** — why the parsed image is working memory rather than a
      record of the file, and why the paint snapshot is a whole union.
    - **Reveal.h** — the write-on, and the zero-length dash entry that hangs the
      host if it is ever allowed to happen.
    - **Style.h** — fill and stroke as the file's own paint rather than
      something approximated from the picture.
    - **Compose.h** / **Shaders.h** — the only mirrored arithmetic in the repo,
      and why the fragment shader inverts the transform per pixel instead of
      doing the obvious faster thing.

    This class is the part that talks to the host: it declares the parameters,
    reloads the document when the file changes, drives the clock, asks the
    rasteriser for a raster and draws it.

    Both plugins are this class. The source draws over its own background; the
    effect draws over the incoming clip. They differ by a constructor flag and
    their input count — little enough that keeping them as one class is what
    stops them drifting apart.

    See AGENTS.md for the traps.
*/
namespace burin
{
class BurinPlugin : public CFFGLPlugin
{
public:
	explicit BurinPlugin( bool isEffect );
	~BurinPlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Overridden even though the only text parameter is the file path, and
	/// **the override is load-bearing for parameters that are display-only
	/// too**. The SDK's `instantiateGL` sets every parameter's default on a
	/// fresh instance and deletes the instance if any set returns FF_FAIL, and
	/// the base `SetTextParameter` is a stub that returns exactly that. A
	/// plugin that declares any text parameter and does not override this
	/// cannot be instantiated by any real host — and the fleet's harnesses call
	/// the class directly, so nothing offline catches it.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	//-----------------------------------------------------------------------
	// The harness reaches in here. None of this is used by the shipped plugin.
	//-----------------------------------------------------------------------

	/// Drive the clock directly instead of reading the host's, so a frame can
	/// be rendered cold at any time in any order.
	void SetSecondsForTest( double seconds );

	/// Load a document from memory, for the built-in test drawings.
	bool LoadDocumentString( const std::string& text, const std::string& name );

	const Document& GetDocument() const { return document_; }
	const Rasteriser& GetRasteriser() const { return raster_; }

	/// The settings this frame's parameters resolve to. Exposed so the harness
	/// can check the plugin's *own* reading of its controls rather than
	/// building an equivalent one and testing that instead.
	RasterRequest BuildRequest( int width, int height ) const;
	ComposeSettings BuildCompose() const;

private:
	void ApplyPreset( int presetIndex );
	void ReloadDocument();
	void UpdateClock( double hostTime );

public:
	FFResult SetTime( double time ) override;

	void SetClockScaleForTest( double scale );
	void TickClockForTest();
	double ClockScaleForTest() const;
	double HostSecondsForTest() const;

	/// The cycle count the next frame would be drawn at. `--rate` needs it: the
	/// thing being tested is that a Rate change does NOT move the drawing, and
	/// reading the count either side of one says so directly, where comparing
	/// rendered frames would only say the two frames match -- and a sine at a
	/// whole number of cycles away matches for the wrong reason.
	double CyclesForTest() const;

private:
	bool UploadRaster();

	const bool isEffect_;

	float params_[ PT_COUNT_ALL ] = { 0.0f };

	// The buttons are declared one per link, so the run in Controls.h and the
	// run the block actually has must agree. They diverge the day somebody
	// writes a user guide, and this is what says so.
	static_assert( PT_COUNT_ALL - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
	               "Controls.h's About run no longer matches StoatworksAbout.h -- "
	               "add or remove a PT_ABOUT_BUTTON_n to match" );

	//-----------------------------------------------------------------------
	// Content
	//-----------------------------------------------------------------------
	Document document_;
	Rasteriser raster_;
	RevealPlan plan_;

	/// The file path arrives on the host's thread and is read on the render
	/// thread. One mutex, held only across the copy.
	std::mutex textMutex_;
	std::string filePath_;
	std::string loadedPath_;
	char textReturn_[ 4096 ] = { 0 };

	bool contentDirty_ = true;

	//-----------------------------------------------------------------------
	// GL
	//-----------------------------------------------------------------------
	ffglex::FFGLShader shader_;
	GLuint vao_       = 0;
	GLuint texture_   = 0;
	int textureW_     = 0;
	int textureH_     = 0;
	bool glReady_     = false;
	bool uploadDirty_ = true;

	//-----------------------------------------------------------------------
	// Clock
	//-----------------------------------------------------------------------
	double hostSeconds_  = 0.0;
	double clockBase_    = 0.0;

	//-----------------------------------------------------------------------
	// Cycle continuity across a Rate change.
	//
	// The drawing stays a pure function of `cycles` -- that is the whole design
	// and none of it changes here. What changes is only which cycle count a
	// given clock reading maps to.
	//
	// `cycles = seconds * rate` means a rate change moves the count by
	// `seconds * delta`, and `seconds` is however long the composition has been
	// open. Nudging Rate an hour in is a jump of hundreds of cycles: the zoom
	// and the drift both teleport, which is what orrery issue #6 reported once
	// the 1000x clock bug was out of the way. So remember the count reached so
	// far and carry on from there at the new rate.
	//
	// Free only. Beat and Bar deliberately keep jumping: their contract is that
	// a cycle boundary lands on the host's grid, and an offset that made a rate
	// change seamless would slide the motion off the grid it exists to sit on.
	// Manual is driven entirely by the Phase slider. Nor is any of this in the
	// OpenFX build: that host renders arbitrary times in arbitrary order and can
	// keyframe Rate, so a running anchor there would make a frame depend on
	// which frames were rendered before it.
	//
	// Reset clears it along with `clockBase_` -- starting again means starting
	// again.
	//-----------------------------------------------------------------------
	void UpdateMotionAnchor();

	/// Where the motion has got to, in cycles. The one copy of that expression:
	/// BuildRequest draws from it and CyclesForTest reads it, so the test cannot
	/// be measuring different arithmetic from the plugin.
	double CurrentCycles() const;

	double cycleAnchor_   = 0.0; ///< cycles already reached at `anchorSeconds_`
	double anchorSeconds_ = 0.0; ///< the elapsed reading that count belongs to
	float anchorRate_     = -1.0f;///< rate in force since then; < 0 until the first frame
	double lastRawTime_  = -1.0;
	double clockScale_   = 0.0;
	double lastWallTime_ = -1.0;
	double wallStart_    = -1.0;
	int secondsVotes_    = 0;
	int millisVotes_     = 0;
	bool hostTimeSeen_   = false;
	bool forcedSeconds_  = false;
	bool resetPending_   = false;

	float bpm_      = 120.0f;
	float barPhase_ = 0.0f;
};

} // namespace burin
