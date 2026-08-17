# Attributions

Burin is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### nanosvg

<https://github.com/memononen/nanosvg>  
Licence: zlib  
Copyright: Mikko Mononen

Vendored, unmodified, at external/nanosvg (two headers and the licence).

The SVG parser and rasteriser. It is the reason this plugin can do what it does rather than merely draw a picture: it hands over the **shape list** — every path with its own fill, stroke, width, dash and geometry still separate — which is what makes "strokes only", "reveal along the path's own length" and "stagger one shape against the next" possible at all. A more complete renderer that returned a finished bitmap could not be asked any of those questions. Nothing in this repo modifies it; everything is achieved by writing into the parsed structures before each rasterise.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### OpenFX

<https://github.com/AcademySoftwareFoundation/openfx>  
Licence: BSD-3-Clause  
Copyright: The Open Effects Association

A subset vendored at external/openfx — the C headers plus the official C++ Support library.

The plugin ABI for the Resolve, Nuke, Natron and Vegas build.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### zlib

<https://zlib.net>  
Licence: zlib  
Copyright: Jean-loup Gailly and Mark Adler

Linked from the operating system by the offline harness only. Not part of either shipped plugin.

Writes the PNGs that `burintest` produces, which is why the PNG writer in the harness is fifty lines rather than a vendored dependency.
