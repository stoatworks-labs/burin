/**
    The one translation unit that carries nanosvg's implementation.

    Deliberately empty apart from these defines. Putting anything else in here
    would mean recompiling four thousand lines of vendored C every time it
    changed, and — worse — would put our own code in a file where the two
    `_IMPLEMENTATION` macros are live, which is the one place a stray include
    can turn into duplicate symbols.
*/

#ifdef _MSC_VER
	#pragma warning( push )
	#pragma warning( disable : 4996 )// strncpy
	#pragma warning( disable : 4244 )// double -> float in the vendored source
#endif

#if defined( __clang__ ) || defined( __GNUC__ )
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#if defined( __clang__ ) || defined( __GNUC__ )
	#pragma GCC diagnostic pop
#endif

#ifdef _MSC_VER
	#pragma warning( pop )
#endif
