#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one command.
#
# The rule this file exists to enforce: **a check that only ever runs in CI,
# after a tag, is a check that will catch you after the tag.** Anything the
# release job does that can be done locally is done here, where it costs a
# second instead of a failed release and a force-moved tag.
#
# The two release-time traps in particular — the OFX bundle's
# CFBundleExecutable, and codesign's reaction to it — are checked here because
# in flipbook they were not, and both survived a clean build, a full test run
# and a manual smoke test before failing in the release job.
#
#   tools/verify.sh            build and check everything
#   tools/verify.sh --fast     skip the universal build (arm64 only)

set -euo pipefail

cd "$(dirname "$0")/.."

FAST=0
[[ "${1:-}" == "--fast" ]] && FAST=1

FAILED=0
pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; FAILED=1; }
head() { printf '\n%s\n' "$1"; }

#---------------------------------------------------------------------------
head "build"
#---------------------------------------------------------------------------
ARCHFLAG=()
[[ $FAST == 1 ]] && ARCHFLAG=(-DCMAKE_OSX_ARCHITECTURES=arm64)

cmake -B build -DCMAKE_BUILD_TYPE=Release "${ARCHFLAG[@]}" >/dev/null
cmake --build build -j"$(sysctl -n hw.ncpu)" >/dev/null
pass "builds clean"

SOURCE_BUNDLE="build/Burin.bundle/Contents/MacOS/Burin"
EFFECT_BUNDLE="build/Burin Over.bundle/Contents/MacOS/Burin Over"
OFX_BUNDLE="build/Burin.ofx.bundle"
OFX_BIN="$OFX_BUNDLE/Contents/MacOS/Burin.ofx"

#---------------------------------------------------------------------------
head "bundles"
#---------------------------------------------------------------------------
for bin in "$SOURCE_BUNDLE" "$EFFECT_BUNDLE"; do
	name="$(basename "$bin")"

	if [[ ! -f "$bin" ]]; then
		fail "$name: missing"
		continue
	fi

	# The registration trap. CFFGLPluginInfo is constructed at file scope and
	# never referenced by name, so in a STATIC archive the linker is entitled to
	# drop the whole translation unit — giving a bundle that loads, exports
	# plugMain, and reports that it contains no plugins. burin_core is an
	# OBJECT library to prevent it; this is what proves it worked.
	if [[ "$(nm -gU "$bin" | grep -c '_plugMain' || true)" == "1" ]]; then
		pass "$name: exports plugMain"
	else
		fail "$name: plugMain missing or duplicated"
	fi

	# The OTHER direction of the same trap: SourcePlugin.cpp and
	# EffectPlugin.cpp are listed in their own MODULE targets rather than in the
	# shared library, because putting either in the shared one would register
	# BOTH plugins into BOTH bundles. Two IDs in one bundle is that mistake.
	# UNIQUE ids, not matching lines. A universal binary has two architecture
	# slices and `strings` walks both, so every id appears twice in a correct
	# bundle — counting lines reports a clean universal build as carrying two
	# plugins, which is precisely the failure this check exists to detect and
	# would have made it useless exactly when it was needed.
	ids="$(strings "$bin" | grep -oE '^BU0[12]$' | sort -u | tr '\n' ' ' | sed 's/ $//')"
	if [[ "$(printf '%s' "$ids" | wc -w | tr -d ' ')" == "1" ]]; then
		pass "$name: registers exactly one plugin ($ids)"
	else
		fail "$name: registers plugin IDs [$ids] (expected exactly one)"
	fi
done

if [[ $FAST == 0 ]]; then
	for bin in "$SOURCE_BUNDLE" "$EFFECT_BUNDLE"; do
		# Verified with lipo and never with the build log. CMake latches
		# CMAKE_OSX_ARCHITECTURES when the first target is created, so a
		# misplaced set() gives an arm64-only binary that the log calls a
		# success.
		if lipo -archs "$bin" 2>/dev/null | grep -q 'arm64' && lipo -archs "$bin" 2>/dev/null | grep -q 'x86_64'; then
			pass "$(basename "$bin"): universal (arm64 + x86_64)"
		else
			fail "$(basename "$bin"): not universal — got '$(lipo -archs "$bin" 2>/dev/null)'"
		fi
	done
else
	printf '  skip  universal check (--fast)\n'
fi

#---------------------------------------------------------------------------
head "openfx — the two traps that only bite after a tag"
#---------------------------------------------------------------------------
if [[ ! -f "$OFX_BIN" ]]; then
	fail "OFX bundle missing"
else
	if [[ "$(nm -gU "$OFX_BIN" | grep -c 'OfxGetPlugin' || true)" == "1" ]]; then
		pass "exports OfxGetPlugin"
	else
		fail "OfxGetPlugin missing"
	fi

	# TRAP 1: CFBundleExecutable against the binary actually on disk.
	#
	# cmake/InfoOFX.plist.in is one of the files copied from repo to repo when a
	# new plugin starts, and the version flipbook was copied FROM had the
	# previous plugin's name hardcoded here. That does not fail the build: the
	# bundle assembles, the binary is correct, lipo and nm both pass, and
	# ofxprobe loads it and renders a correct frame.
	declared="$(/usr/libexec/PlistBuddy -c 'Print CFBundleExecutable' "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null || echo '?')"
	actual="$(basename "$OFX_BIN")"
	if [[ "$declared" == "$actual" ]]; then
		pass "CFBundleExecutable ('$declared') matches the binary on disk"
	else
		fail "CFBundleExecutable is '$declared' but the binary is '$actual'"
	fi

	# TRAP 2: codesign, on a COPY, with an ad-hoc identity.
	#
	# This is the exact command the release job runs, and it is the one that
	# actually failed in flipbook — with "code object is not signed at all / In
	# subcomponent: .../Contents/MacOS/<name>.ofx", because codesign read the
	# plist, looked for an executable that was not there, and treated the real
	# binary as a nested object needing its own signature first. Nothing in that
	# message mentions the plist, and nothing before the release job runs it.
	#
	# A copy, so a real signature on the real bundle is never disturbed.
	tmp="$(mktemp -d)"
	trap 'rm -rf "$tmp"' EXIT
	cp -R "$OFX_BUNDLE" "$tmp/"
	if codesign --force --sign - "$tmp/$(basename "$OFX_BUNDLE")" >/dev/null 2>&1; then
		pass "ad-hoc codesign succeeds (the release job's command)"
	else
		fail "codesign fails — almost always CFBundleExecutable above"
	fi
fi

#---------------------------------------------------------------------------
head "the example drawing"
#---------------------------------------------------------------------------
if [[ -f docs/example-plate.svg ]]; then
	# Regenerated and compared, so a hand edit to the shipped file — which would
	# be lost the next time anyone runs the generator — is caught here rather
	# than discovered later.
	#
	# Compared against the FILE, not against git. An earlier version regenerated
	# and then ran `git diff --quiet`, which conflates two different questions:
	# "does the shipped drawing match its generator" (what is being asked) and
	# "is the working tree clean" (what git answers). Any uncommitted change to
	# either file made it fail, which it duly did during a rename with both
	# staged and neither committed — reporting a drawing that matched its
	# generator perfectly as drifted.
	snapshot="$(mktemp)"
	cp docs/example-plate.svg "$snapshot"
	python3 tools/make_example_svg.py >/dev/null
	if cmp -s docs/example-plate.svg "$snapshot"; then
		pass "docs/example-plate.svg matches its generator"
	else
		fail "docs/example-plate.svg differs from what make_example_svg.py produces"
	fi
	rm -f "$snapshot"

	# It must contain no <text>: nanosvg ignores text entirely, and the file
	# that demonstrates the plugin must not also demonstrate its main limitation.
	if grep -qi '<text' docs/example-plate.svg; then
		fail "the example drawing contains <text>, which nanosvg does not render"
	else
		pass "the example drawing has no live text"
	fi
else
	fail "docs/example-plate.svg missing — run python3 tools/make_example_svg.py"
fi

#---------------------------------------------------------------------------
head "measurements"
#---------------------------------------------------------------------------
if ./build/burintest --all 2>&1 | tee /tmp/burintest.log | grep -qE '^all checks passed'; then
	pass "burintest --all"
else
	fail "burintest --all — see /tmp/burintest.log"
	grep -E '^  FAIL' /tmp/burintest.log | sed 's/^/    /' || true
fi

#---------------------------------------------------------------------------
head "no dead controls"
#---------------------------------------------------------------------------
if python3 tools/sweep.py >/tmp/rzsweep.log 2>&1; then
	pass "$(tail -1 /tmp/rzsweep.log)"
else
	fail "sweep found dead controls — see /tmp/rzsweep.log"
	grep -E 'DEAD' /tmp/rzsweep.log | sed 's/^/    /' || true
fi

#---------------------------------------------------------------------------
printf '\n'
if [[ $FAILED == 0 ]]; then
	printf 'everything passed\n'
else
	printf 'FAILURES ABOVE\n'
fi
exit $FAILED
