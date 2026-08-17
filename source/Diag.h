#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family. The rest of the repos get a
    rotating log, a crash report and a diagnostics bundle; an FFGL plugin gets
    only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- a plugin is a
      list of sliders in someone else's inspector.

    What it covers is the failures that actually happen, all of which look
    identical from the operator's side ("it does nothing") with no message
    anywhere:

    - **The file would not load or would not parse.** A path that was valid on
      the machine where the composition was saved, a file that is not an SVG, a
      truncated download.
    - **The file parsed and contains nothing this renderer can draw.** By some
      distance the most important line in this log, because it is the one
      failure that is *nobody's bug*: nanosvg has no text support whatever, so
      an SVG whose content is live `<text>` parses perfectly, reports no error,
      and draws an empty frame. The note names the count and says to convert
      text to outlines. `<use>`, `<image>`, `<clipPath>`, `<mask>`, `<pattern>`
      and `<filter>` are counted the same way.
    - **A raster that was refused for size.** The cover rect is capped at
      `kMaxRasterPx`; past that the detail is quietly reduced rather than the
      frame going black, and the log says by how much.
    - **A shader would not compile**, so `InitGL` returns `FF_FAIL`. The GL
      vendor, renderer and version strings are logged next to the compile log,
      because a shader that builds on one machine and not on another is a driver
      answer, not a source answer.
*/
namespace rasterizer::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace rasterizer::diag
