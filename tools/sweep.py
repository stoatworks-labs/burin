#!/usr/bin/env python3
"""Sweep every parameter and fail if any of them does nothing.

The only thing in this repo that catches a **dead control**: a parameter that
is declared, appears in the host's inspector, and is never read — because
`BuildRequest` forgot it, or because a GLSL uniform name does not match the C++
and `glGetUniformLocation` quietly returned -1. Neither of those fails to
compile, fails to load, or fails any other test here. Both look like a slider
that does nothing.

It works by driving `burintest --frame`, which goes through `SetFloatParameter` by
parameter **name** on the real plugin class, so the whole chain from the host's
call to the pixels is under test.

## The two tables, and why they exist

**CONTEXT** — most parameters are supposed to do nothing in the default
configuration, and every one of them is a false failure waiting to happen. The
colour controls need Recolour switched on; Stagger and Order need a reveal
running; Mix is effect-only; Rate and Wave need something moving to apply
themselves to. The table says what else to set before the parameter under test
can possibly matter.

**ENDS** — a few parameters have two settings that are the same picture, so
sweeping the obvious ends reports a working control as dead:

- **Rotate** runs -180 to +180, and the two ends are the same frame to the bit.
- **Colour Spread** is signed and symmetrical about its centre; the two ends
  walk the hue wheel in opposite directions and, on a drawing whose shapes are
  evenly spread, land on the same set of colours.
- **Progress** at 0 and 1 with the reveal off is the same picture by design.

Run it after any change to the parameter list, and after any change to a
uniform name.

    python3 tools/sweep.py [--exe build/burintest] [--file docs/example-plate.svg]
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

# Parameter -> extra settings needed before it can possibly do anything.
CONTEXT = {
    "Colour":            {"Recolour": 3},
    "Colour_Green":      {"Recolour": 3},
    "Colour_Blue":       {"Recolour": 3},

    # A SATURATED base colour, not the default white. RotateHue returns a grey
    # unchanged — a grey has no hue to rotate, and inventing one would turn the
    # blacks and whites of a drawing into an arbitrary colour the moment the
    # spread left zero. So sweeping this against the default white correctly
    # reports no change, and the parameter is not dead; the test was.
    "Colour Spread":     {"Recolour": 3, "Colour": 1.0, "Colour_Green": 0.1, "Colour_Blue": 0.0},

    "Stroke Width":      {"Draw": 2},
    "Min Stroke px":     {"Draw": 2, "Stroke Width": 0.0, "Zoom": 0.1},

    "Progress":          {"Reveal": 3},                 # Hold: the slider IS the clock
    "Stagger":           {"Reveal": 3, "Progress": 0.5},
    "Order":             {"Reveal": 3, "Progress": 0.5, "Stagger": 0.6},
    "Fill Fade":         {"Reveal": 3, "Progress": 0.6},

    # The motion controls need the clock to have gone somewhere. --time is
    # passed separately; these make the excursion large enough to measure.
    "Rate":              {"Zoom Move": 0.8},
    "Wave":              {"Zoom Move": 0.8},
    "Zoom Move":         {},
    "Drift X":           {},
    "Drift Y":           {},
    "Spin":              {},

    "Background":        {"Background Alpha": 1.0},
    "Background_Green":  {"Background Alpha": 1.0},
    "Background_Blue":   {"Background Alpha": 1.0},

    "Shape Count":       {"First Shape": 0},

    # Phase drives the motion only in Manual sync; in every other mode the
    # clock does and Phase is correctly ignored.
    "Phase":             {"Sync": 3, "Zoom Move": 0.8},
}

# Parameter -> the two values to compare, when 0 and 1 are the same picture.
ENDS = {
    "Rotate":        (0.5, 0.75),   # +180 and -180 are the same frame
    "Colour Spread": (0.5, 1.0),    # signed and symmetrical about the centre
    "Progress":      (0.15, 0.85),  # 0 draws nothing and 1 is "no reveal"
    "Spin":          (0.5, 0.9),    # signed about the centre detent
    "Drift X":       (0.0, 1.0),
    "Drift Y":       (0.0, 1.0),

    # Pan runs -1..1 fitted boxes. At BOTH ends a drawing that fits the frame is
    # entirely off it — two empty frames, identical, and a working control
    # reported dead. This is the subtlest entry in the table: the parameter is
    # not symmetrical and the two ends are not the same picture, they are both
    # the *absence* of a picture.
    "Position X":    (0.35, 0.65),
    "Position Y":    (0.35, 0.65),

    # Phase spans exactly one cycle, and a cycle is a loop: 1.0 is the same
    # position as 0.0, by design and as documented on MotionClock. Same shape of
    # trap as Rotate's two ends.
    "Phase":         (0.0, 0.25),
}

# Parameters that are not swept, with the reason. Each one is a deliberate
# exclusion rather than an oversight, and the list is short on purpose.
SKIP = {
    "Drawing":  "a file path, not a float — the sweep sets one for every case anyway",
    "Reset":    "an event; it re-bases a clock, and with the clock forced by the "
                "harness it provably cannot change the frame",
    "Preset":   "covered exhaustively by burintest --presets, which checks each one "
                "renders rather than merely that they differ",
    "Mix":      "effect-only; the sweep drives the source build, which ignores it "
                "by design so that a composition can move between the two",
    "Sync":     "selects which clock drives the motion; with the clock forced to a "
                "fixed instant by the harness, Free and Manual land on the same phase",
}


def run(exe, out, svg, sets, seconds, size):
    cmd = [exe, "--frame", out, "--size", size, "--time", str(seconds)]
    if svg:
        cmd += ["--file", svg]
    for k, v in sets.items():
        cmd += ["--set", f"{k}={v}"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"    burintest failed: {r.stdout.strip()} {r.stderr.strip()}")
        return None
    with open(out, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="build/burintest")
    ap.add_argument("--file", default="docs/example-plate.svg")
    # Deliberately NOT square. Fit and Fill are the same picture whenever the
    # drawing's aspect matches the frame's, and the example drawing is square —
    # so a square sweep frame reports Fit as a dead control. 16:9 is also what
    # the plugin will actually run at.
    ap.add_argument("--size", default="480x270")
    ap.add_argument("--time", type=float, default=3.7,
                    help="clock reading; deliberately not a whole number of "
                         "cycles, or every waveform would be sampled at zero")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print(f"no such executable: {args.exe}")
        return 1
    svg = args.file if os.path.exists(args.file) else ""
    if not svg:
        print(f"note: {args.file} not found, using the built-in drawing")

    listing = subprocess.run([args.exe, "--list"], capture_output=True, text=True)
    names = []
    for line in listing.stdout.splitlines()[1:]:
        parts = line.split(None, 1)
        if len(parts) < 2 or not parts[0].isdigit():
            continue
        # The name column is fixed width in --list; everything after it is type,
        # default and range.
        names.append(line[5:26].strip())

    # The About block is a text field and browser buttons, declared last. They
    # never touch a pixel, so sweeping them only buries a real dead control.
    # Truncating at "About" rather than naming each button keeps this correct
    # when a link is added -- writing the user guide added a fourth button.
    for i, name in enumerate(names):
        if name == "About":
            names = names[:i]
            break

    if not names:
        print("could not read the parameter list")
        return 1

    tmp = tempfile.mkdtemp(prefix="rzsweep-")
    dead, skipped, checked = [], [], 0

    for name in names:
        if name in SKIP:
            skipped.append(name)
            continue

        lo, hi = ENDS.get(name, (0.0, 1.0))
        base = dict(CONTEXT.get(name, {}))

        a = run(args.exe, os.path.join(tmp, "a.png"), svg, {**base, name: lo}, args.time, args.size)
        b = run(args.exe, os.path.join(tmp, "b.png"), svg, {**base, name: hi}, args.time, args.size)

        if a is None or b is None:
            return 1

        checked += 1
        if a == b:
            dead.append(name)
            print(f"  DEAD  {name}  ({lo} and {hi} are the same frame)")
        else:
            print(f"  ok    {name}")

    print()
    for name in skipped:
        print(f"  skip  {name} — {SKIP[name]}")

    print(f"\n{checked} parameters swept, {len(skipped)} skipped, {len(dead)} dead")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        print("Either the parameter is never read, or a uniform name does not "
              "match the C++, or CONTEXT is missing what it needs to matter.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
