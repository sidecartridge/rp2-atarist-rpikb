#!/bin/bash
# Build the core firmware out of tree for one board flavor, flash it through
# the Debug Probe, and confirm the RP runs exactly that build.
#
#   tools/dev/flash.sh <souffle|croissant> <release|debug>
#                      [--build-only] [--timeout S]
#
# Builds into tools/dev/builds/<flavor>-<type>/core (incremental; never touches
# build-*/, dist/ or any submodule) with the settings of build.sh and the CMake
# presets: Pico 2 W for both flavors, release is CMake Release, debug is CMake
# Debug with the 921,600 baud console. Release ELFs keep their symbols, so
# every swd.py command works on either.
#
# Everything builds against worktrees in tools/dev/sdk/, created on first use
# from the submodules' own history at the refs build.sh pins (pico-sdk 2.2.0
# with its libraries, pico-extras sdk-2.2.0, Bluepad32 synthetic-HID-descriptor).
# The submodules themselves are never checked out, and local edits in them do
# not reach these builds. The ELF is kept as
# tools/dev/builds/elf/core-<flavor>-<type>-<id>.elf for swd.py.
#
# Then flashes through the Debug Probe (tools/dev/swd.py program: only the
# core firmware's own flash, so Booster, the config sectors, the settings and
# the Bluetooth keys are kept; a watchdog reset after) and checks over SWD,
# without the firmware's help: the core firmware runs, flash matches the ELF
# byte for byte, and it carries the new build ID. With MODE 255 in the settings
# the core firmware jumps straight into Booster, so the run check fails: set
# MODE first (swd.py settings set MODE=1). On failure exits 1 and prints the
# console since the last boot (tools/dev/console.py watch must be running for
# that). The merged image with Booster is flashed from the Booster repository.
#
# Environment: OPENOCD and PICO_OPENOCD_PATH (see swd.py), RELEASE_DATE
# (default: the date of the HEAD commit, so builds of one tree are
# byte-identical), FLASH_BUILDS_DIR (default tools/dev/builds: where the
# <flavor>-<type> build directories go; measure_builds.sh points it elsewhere).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SDK_DIR="$HERE/sdk"
ELF_DIR="$HERE/builds/elf"

usage() { sed -n '5,6p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }

[ $# -ge 2 ] || usage
FLAVOR="$1"; TYPE="$2"; shift 2
case "$FLAVOR" in
  croissant) BOARD_TARGET=1 ;;
  souffle)   BOARD_TARGET=2 ;;
  *) usage ;;
esac
case "$TYPE" in release|debug) ;; *) usage ;; esac
BUILD_ONLY=0; TIMEOUT=30
while [ $# -gt 0 ]; do
  case "$1" in
    --build-only) BUILD_ONLY=1 ;;
    --timeout) shift; TIMEOUT="${1:?}" ;;
    *) usage ;;
  esac
  shift
done

# --- worktrees ----------------------------------------------------------------
# Each pinned repository gets a worktree of its own history at the pinned ref,
# plus worktrees of the nested submodules the firmware compiles, at the commits
# that ref records. Everything comes from local history, except the first run
# in a fresh checkout, which clones the submodules from the network.
mkdir -p "$HERE/builds"
GIT_ERR="$HERE/builds/git.err"
git_retry() {  # the IDE's git can hold an index.lock for a moment
  local i
  for i in 1 2 3 4 5 6 7 8 9 10; do
    if "$@" 2> "$GIT_ERR"; then return 0; fi
    grep -q 'index.lock' "$GIT_ERR" || { cat "$GIT_ERR" >&2; return 1; }
    sleep 1
  done
  cat "$GIT_ERR" >&2; return 1
}
ensure_worktree() {  # ensure_worktree SUBMODULE REF MARKER_FILE [NESTED ...]
  local sub="$1" ref="$2" marker="$3"; shift 3
  local wt="$SDK_DIR/$sub-$ref" nested commit
  # A fresh checkout has not cloned the submodule: without it, git would read
  # this repository instead and fail with "invalid reference". Clone it once,
  # as build.sh does; this is the only step that needs the network.
  if [ ! -e "$REPO/$sub/.git" ]; then
    echo "Cloning the $sub submodule (once per checkout, from the network)"
    git_retry git -C "$REPO" submodule update --init "$sub"
  fi
  if [ ! -f "$wt/$marker" ]; then
    echo "Creating the $sub $ref worktree in ${wt#$REPO/}"
    mkdir -p "$SDK_DIR"
    git_retry git -C "$REPO/$sub" worktree add --detach --quiet "$wt" "$ref"
  fi
  for nested in "$@"; do
    [ -e "$wt/$nested/.git" ] && continue
    if [ ! -e "$REPO/$sub/$nested/.git" ]; then
      echo "Cloning $sub's $nested submodule (once per checkout, from the network)"
      git_retry git -C "$REPO/$sub" submodule update --init "$nested"
    fi
    commit="$(git -C "$REPO/$sub" ls-tree "$ref" "$nested" | awk '{print $3}')"
    if ! git -C "$REPO/$sub/$nested" cat-file -e "$commit^{commit}" 2> /dev/null; then
      echo "$sub $ref pins $nested at $commit, which is not in the local" \
           "history: run 'git -C $sub/$nested fetch' once" >&2
      exit 1
    fi
    echo "Adding $nested ($commit) to the $sub $ref worktree"
    rmdir "$wt/$nested" 2> /dev/null || true
    git_retry git -C "$REPO/$sub/$nested" worktree add --detach --quiet "$wt/$nested" "$commit"
  done
}

# The refs build.sh checks out.
SDK_REF=2.2.0; EXTRAS_REF=sdk-2.2.0; BLUEPAD32_REF=synthetic-HID-descriptor
ensure_worktree pico-sdk "$SDK_REF" pico_sdk_init.cmake \
  lib/btstack lib/cyw43-driver lib/lwip lib/mbedtls lib/tinyusb
ensure_worktree pico-extras "$EXTRAS_REF" external/pico_extras_import.cmake
ensure_worktree bluepad32 "$BLUEPAD32_REF" src/components/bluepad32/CMakeLists.txt

# --- build ----------------------------------------------------------------------
export PICO_SDK_PATH="$SDK_DIR/pico-sdk-$SDK_REF"
export PICO_EXTRAS_PATH="$SDK_DIR/pico-extras-$EXTRAS_REF"
export BLUEPAD32_ROOT="$SDK_DIR/bluepad32-$BLUEPAD32_REF"
export BOARD_TYPE=pico2_w PICO_BOARD=pico2_w PICO_PLATFORM=pico2_w
export BOARD_TARGET RELEASE_TYPE=""
export RELEASE_VERSION="$(tr -d '\r\n ' < "$REPO/version.txt")"
export RELEASE_DATE="${RELEASE_DATE:-$(git -C "$REPO" log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M:%S')}"
if [ "$TYPE" = release ]; then
  export DEBUG_MODE=0; CMAKE_TYPE=Release
else
  export DEBUG_MODE=1; CMAKE_TYPE=Debug
fi
mkdir -p "$ELF_DIR"
JOBS="$(sysctl -n hw.ncpu 2> /dev/null || nproc)"
OUT="${FLASH_BUILDS_DIR:-$HERE/builds}/$FLAVOR-$TYPE/core"
mkdir -p "$OUT"

echo "Building core ($FLAVOR $TYPE) in ${OUT#$REPO/}"
cmake -S "$REPO/src" -B "$OUT" -DCMAKE_BUILD_TYPE=$CMAKE_TYPE \
  > "$OUT/cmake.log" 2>&1 || { tail -20 "$OUT/cmake.log"; exit 1; }
cmake --build "$OUT" -j"$JOBS" > "$OUT/make.log" 2>&1 \
  || { grep -m5 -E 'error|Error' "$OUT/make.log"; \
       echo "core: build failed (${OUT#$REPO/}/make.log)" >&2; exit 1; }
[ -f "$OUT/rp2-ikbd.elf" ] || { echo "core: no rp2-ikbd.elf in ${OUT#$REPO/}" >&2; exit 1; }
ID="$(sed -n 's/^#define RELEASE_BUILD_ID "\(.*\)"$/\1/p' \
      "$OUT/generated/build_id/build_id.h")"
CORE_ELF="$ELF_DIR/core-$FLAVOR-$TYPE-$ID.elf"
cp "$OUT/rp2-ikbd.elf" "$CORE_ELF"
echo "  core: $ID"
[ "$BUILD_ONLY" = 1 ] && exit 0

# --- flash and verify ---------------------------------------------------------
fail() {
  echo "FAILED: $1" >&2
  python3 "$HERE/console.py" since-boot 2> /dev/null | tail -40 >&2 || true
  exit 1
}
python3 "$HERE/swd.py" program "$CORE_ELF" || fail "programming"
python3 "$HERE/swd.py" running "$CORE_ELF" --timeout "$TIMEOUT" \
  || fail "the core firmware just flashed is not running (MODE 255?)"
python3 "$HERE/swd.py" verify "$CORE_ELF" || fail "flash differs from the ELF"
python3 "$HERE/swd.py" build-id "$CORE_ELF" || fail "build ID mismatch"
echo "OK: $FLAVOR $TYPE flashed and verified"
