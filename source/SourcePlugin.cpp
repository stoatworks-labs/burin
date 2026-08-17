#include "Burin.h"

/**
    The generator: a drawing on its own background, no input.

    **This file is listed directly in the BurinSource target, not in
    burin_core.** Both plugins share the class; what they do not share is
    the `CFFGLPluginInfo` below, and putting either registration in the shared
    library would register both plugins into both bundles.

    It is also why the shared library is an OBJECT library rather than a STATIC
    one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
    nothing ever references it by name, so in an archive the linker is entitled
    to drop the whole translation unit -- giving a bundle that loads, exports
    `plugMain`, and reports that it contains no plugins.

        nm -gU Burin.bundle/Contents/MacOS/Burin | grep plugMain
*/
namespace
{
class BurinSource : public burin::BurinPlugin
{
public:
	BurinSource() :
		BurinPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< BurinSource >,                   // Create method
	"BU01",                                              // Plugin unique ID of maximum length 4
	"Burin",                                        // Plugin name
	2,                                                   // API major version number
	1,                                                   // API minor version number
	0,                                                   // Plugin major version number
	1,                                                   // Plugin minor version number
	FF_SOURCE,                                           // Plugin type
	"Vector artwork, rasterised at the size it is seen",  // Plugin description
	"Burin FFGL source"                             // About
);

extern "C" const char* BurinSourceBuildStamp()
{
	return "burin " BURIN_VERSION " source, built " __DATE__ " " __TIME__;
}
