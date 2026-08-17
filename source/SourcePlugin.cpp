#include "Rasterizer.h"

/**
    The generator: a drawing on its own background, no input.

    **This file is listed directly in the RasterizerSource target, not in
    rasterizer_core.** Both plugins share the class; what they do not share is
    the `CFFGLPluginInfo` below, and putting either registration in the shared
    library would register both plugins into both bundles.

    It is also why the shared library is an OBJECT library rather than a STATIC
    one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
    nothing ever references it by name, so in an archive the linker is entitled
    to drop the whole translation unit -- giving a bundle that loads, exports
    `plugMain`, and reports that it contains no plugins.

        nm -gU Rasterizer.bundle/Contents/MacOS/Rasterizer | grep plugMain
*/
namespace
{
class RasterizerSource : public rasterizer::RasterizerPlugin
{
public:
	RasterizerSource() :
		RasterizerPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< RasterizerSource >,                   // Create method
	"RZ01",                                              // Plugin unique ID of maximum length 4
	"Rasterizer",                                        // Plugin name
	2,                                                   // API major version number
	1,                                                   // API minor version number
	0,                                                   // Plugin major version number
	1,                                                   // Plugin minor version number
	FF_SOURCE,                                           // Plugin type
	"Vector artwork, rasterised at the size it is seen",  // Plugin description
	"Rasterizer FFGL source"                             // About
);

extern "C" const char* RasterizerSourceBuildStamp()
{
	return "rasterizer " RASTERIZER_VERSION " source, built " __DATE__ " " __TIME__;
}
