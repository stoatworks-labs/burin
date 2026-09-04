#include "Burin.h"

/**
    The effect: the same drawing, over the incoming clip.

    It exists because the obvious way to get a logo over footage in Resolume --
    put the source on its own layer and pick a blend mode -- costs a layer and
    puts the drawing's controls somewhere other than the clip they belong to.
    This one sits on the clip, and Mix crossfades it.

    Note what this plugin does **not** offer, in deliberate contrast to
    flipbook's effect build: there is no "take the incoming clip as the
    drawing". A clip is a raster. The whole of this plugin's reason to exist is
    that its source is *not* a raster -- there are paths in it, with fills and
    strokes and lengths -- and a mode that fed it a bitmap would be a mode in
    which every interesting control did nothing. See AGENTS.md.

    See SourcePlugin.cpp for why this file is listed in its own target rather
    than in the shared library.
*/
namespace
{
class BurinEffect : public burin::BurinPlugin
{
public:
	BurinEffect() :
		BurinPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< BurinEffect >,          // Create method
	"BU02",                                     // Plugin unique ID of maximum length 4
	"Burin Over",                          // Plugin name
	2,                                          // API major version number
	1,                                          // API minor version number
	0,                                          // Plugin major version number
	1,                                          // Plugin minor version number
	FF_EFFECT,                                  // Plugin type
	"Vector artwork over the clip, always sharp.\n\nPoint it at an SVG and it draws it at the resolution the frame actually needs, rebuilding whenever that changes. A bitmap zoomed eight times is eight times blurrier; this is not, because it is re-rasterised rather than scaled.\n\nRe-rasterising a thousand paths every frame is not something a VJ rig can afford, so the scale is snapped to a ladder half an octave apart and the drawing is rebuilt only when the rung changes. It always snaps up, never to the nearest rung - which is the difference between sharp and usually sharp.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Burin FFGL effect"                    // About
);

extern "C" const char* BurinEffectBuildStamp()
{
	return "burin " BURIN_VERSION " effect, built " __DATE__ " " __TIME__;
}
