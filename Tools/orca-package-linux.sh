#!/bin/bash
# Builds Orca for Linux (x86-64) and zips it the way the YouGame app's linux-x64 build unpacks it:
#   Orca/orca          the NoGUI program (the manifest's entry, "Orca/orca")
#   Orca/Sys/          Dolphin's and Orca's data, read from beside the program
#   Orca/COPYING, LICENSES/, THIRD_PARTY/, SOURCE.txt, VERSION.txt
#
#   Tools/orca-package-linux.sh [build-dir] [out-dir]
#     build-dir  default <repo>/build-orca-linux (configured for a release on every run)
#     out-dir    default <build-dir>/package
#
# Linux has no code signing the app checks: it trusts the zip by the manifest's SHA-256 and every
# file by the hashes it records when it unpacks it (desktop/src/orca-policy.ts treeProblem).
#
# A release sets ORCA_VERSION (X.Y.Z) and ORCA_SOURCE_URL (the public repository's URL, which
# players fetch the source from: GPL) and builds the private repository at the release commit, like
# the Windows build: that commit is in the compatibility key (scmrev.h), so a build of the public
# snapshot would never meet a Mac or a PC. The tree must be clean before and after the build.
# VERSION.txt and SOURCE.txt link <ORCA_SOURCE_URL>/tree/orca-<version>; the script warns while
# that tag is not public yet. Without ORCA_VERSION the zip is a *-dev.zip.
#
# Build it on the oldest distribution it must run on (ORCA_GLIBC_MAX, default 2.35: Ubuntu 22.04,
# Debian 12, SteamOS 3 and everything since): glibc is the one library it cannot carry. Needs git,
# cmake, ninja, pkg-config, zip, binutils, GCC 12 or newer (Orca's CMakeLists refuses 11; on Ubuntu
# 22.04 install gcc-12 g++-12 and run with CC=gcc-12 CXX=g++-12), and the headers of X11, Xi, Xext,
# Xrandr, EGL/OpenGL and the sound servers Cubeb opens at run time (Debian/Ubuntu: libx11-dev
# libxi-dev libxext-dev libxrandr-dev libegl-dev libgl-dev libpulse-dev libasound2-dev). Without the
# PulseAudio headers Cubeb has no Linux backend and Orca would play silent, so the build is refused.
set -euo pipefail
# readelf and objdump print what is parsed below in the C locale only.
export LC_ALL=C

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-orca-linux}"
OUT_DIR="${2:-$BUILD_DIR/package}"
GLIBC_MAX="${ORCA_GLIBC_MAX:-2.35}"

fail() {
  echo "orca-package: $*" >&2
  exit 1
}

[ "$(uname -s)" = Linux ] || fail "Linux only"
[ "$(uname -m)" = x86_64 ] || fail "x86-64 only (the app's linux-x64 build)"
for tool in git cmake ninja pkg-config zip readelf objdump objcopy strip; do
  command -v "$tool" >/dev/null || fail "needs $tool"
done
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
# Changed or untracked paths. A git status that fails (a broken submodule link, say) counts as
# dirty, never as clean.
tree_state() {
  local out
  out="$(git -C "$ROOT" status --porcelain --untracked-files=normal)" ||
    { echo "git status failed"; return 0; }
  printf '%s' "$out"
}
STATE="$(tree_state)"
DIRTY=""
if [ "$STATE" = "git status failed" ]; then
  DIRTY=" (tree state unknown: git status failed)"
elif [ -n "$STATE" ]; then
  DIRTY=" (with local changes)"
fi

RELEASE=0
VERSION="${ORCA_VERSION:-}"
if [ -n "$VERSION" ]; then
  [[ "$VERSION" =~ ^[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}$ ]] || fail "ORCA_VERSION must be X.Y.Z, not $VERSION"
  [ -z "$DIRTY" ] || fail "a release needs a clean tree (untracked files included)$DIRTY"
  PUBLIC="${ORCA_SOURCE_URL:-}"
  PUBLIC="${PUBLIC%/}"
  [ -n "$PUBLIC" ] || fail "a release needs ORCA_SOURCE_URL, the public repository players get the source from"
  RELEASE=1
  SOURCE="$PUBLIC/tree/orca-$VERSION"
  # Anonymous, so a private repository can't pass: players must be able to fetch it.
  GIT_TERMINAL_PROMPT=0 git -c credential.helper= ls-remote --exit-code "$PUBLIC" \
    "refs/tags/orca-$VERSION" >/dev/null 2>&1 ||
    echo "orca-package: WARNING: orca-$VERSION is not public yet at $PUBLIC" >&2
else
  VERSION=0.0.0
  SOURCE="not published (a development build)"
fi

# The same switches as the Windows and macOS releases (no frame-dump encoder, none of the
# frontends' extras), plus what keeps the program to libraries every desktop Linux has: no ALSA or
# PulseAudio backends of Dolphin's own (Cubeb, the default, opens the sound server at run time), no
# BlueZ, evdev or udev (the app hands Orca its controllers; libusb and hidapi would link libudev
# whenever its headers are there), glibc's own iconv (not the bundled LGPL libiconv, linked in
# statically otherwise), and libstdc++ linked in. -g: symbols for the .debug file beside the zip,
# which change no generated code; -ffile-prefix-map: no path of the build machine in them or in the
# program. Configured on every run so an older cache can't slip other settings in.
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT=OFF -DENABLE_NOGUI=ON -DORCA_VERSION="$VERSION" \
  -DUSE_SYSTEM_LIBS=OFF -DENCODE_FRAMEDUMPS=OFF -DENABLE_LLVM=OFF \
  -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF -DUSE_DISCORD_PRESENCE=OFF \
  -DUSE_RETRO_ACHIEVEMENTS=OFF -DUSE_MGBA=OFF -DUSE_UPNP=OFF -DENABLE_VULKAN=OFF \
  -DENABLE_CLI_TOOL=OFF -DENABLE_TESTS=OFF -DUSE_SANITIZERS=OFF \
  -DENABLE_ALSA=OFF -DENABLE_PULSEAUDIO=OFF -DENABLE_BLUEZ=OFF -DENABLE_EVDEV=OFF \
  -DENABLE_HWDB=OFF -DCMAKE_DISABLE_FIND_PACKAGE_LIBUDEV=ON -DUSE_SYSTEM_ICONV=ON \
  -DCMAKE_C_FLAGS="-g -ffile-prefix-map=$ROOT=." -DCMAKE_CXX_FLAGS="-g -ffile-prefix-map=$ROOT=." \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" >/dev/null
grep -q '^USE_PULSE:INTERNAL=1' "$BUILD_DIR/CMakeCache.txt" ||
  fail "Cubeb found no PulseAudio headers (libpulse-dev): Orca would play no sound"
cmake --build "$BUILD_DIR" --target dolphin-nogui

if [ "$RELEASE" = 1 ]; then
  # The source must not have moved under the build.
  [ "$(git -C "$ROOT" rev-parse HEAD)" = "$COMMIT" ] && [ -z "$(tree_state)" ] ||
    fail "the tree changed during the build; start again from a clean checkout of $COMMIT"
fi

BIN="$BUILD_DIR/Binaries/dolphin-emu-nogui"
[ -x "$BIN" ] || fail "no $BIN"
[ -d "$BUILD_DIR/Binaries/Sys" ] || fail "no Sys beside $BIN"
grep -q "$COMMIT" "$BUILD_DIR/Source/Core/Common/scmrev.h" ||
  fail "scmrev.h does not name $COMMIT: the compatibility key would be another build's"

# Self-contained: what the program loads ships with every desktop Linux. And it has its window: a
# build that found no X11 or EGL headers has only Dolphin's framebuffer platform, which CMake only
# warns about.
NEEDED="$(readelf -d "$BIN" | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p')"
ALLOWED='^(libc|libm|libdl|libpthread|librt|ld-linux-x86-64|libX11|libXi|libXrandr|libXext|libxcb|libEGL|libOpenGL|libGL|libGLX)\.so'
FOREIGN="$(printf '%s\n' "$NEEDED" | grep -Ev "$ALLOWED" | grep -v '^$' || true)"
[ -z "$FOREIGN" ] || fail "Orca needs libraries a player may not have:
$FOREIGN"
for lib in libX11.so.6 libXi.so.6 libXext.so.6 libEGL.so.1; do
  printf '%s\n' "$NEEDED" | grep -qx "$lib" || fail "Orca does not link $lib: was it built without its headers?"
done
GLIBC="$(objdump -T "$BIN" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)"
[ "$(printf '%s\n%s\n' "$GLIBC" "$GLIBC_MAX" | sort -V | tail -n 1)" = "$GLIBC_MAX" ] ||
  fail "Orca needs glibc $GLIBC, newer than $GLIBC_MAX: build it on an older distribution"

STAGE="$OUT_DIR/stage"
PKG="$STAGE/Orca"
rm -rf "$STAGE"
mkdir -p "$PKG"
install -m 755 "$BIN" "$PKG/orca"
# Symbols stay beside the zip (for a crash's addresses), not in it.
DEBUG="$OUT_DIR/orca-$VERSION-linux-x64.debug"
objcopy --only-keep-debug "$PKG/orca" "$DEBUG"
strip --strip-unneeded "$PKG/orca"
objcopy --add-gnu-debuglink="$DEBUG" "$PKG/orca"
# Real files only (cp -L): a link would unpack differently from one player to the next. Minus what
# only the Qt UI shows, as in the macOS bundle (DolphinNoGUI/CMakeLists.txt): the themes and the
# Resources images, never the on-screen font.
cp -RL "$BUILD_DIR/Binaries/Sys" "$PKG/Sys"
rm -rf "$PKG/Sys/Themes"
find "$PKG/Sys/Resources" -maxdepth 1 -type f \
  \( -name 'Flag_*' -o -name 'Platform_*' -o -name 'achievements_*' -o -name 'dolphin_logo*' \
  -o -name 'isoproperties_*' -o -name 'nobanner*' \) -delete
[ -z "$(find "$PKG" -type l)" ] || fail "a symbolic link is left in $PKG"
for profile in RSBE01 PPLUS32; do
  [ -s "$PKG/Sys/Orca/$profile.ini" ] || fail "no Sys/Orca/$profile.ini: the app refuses an Orca without it"
done

# What the GPL asks to travel with the binary: where the source is, and the licences.
cp "$ROOT/COPYING" "$PKG/COPYING"
cp -R "$ROOT/LICENSES" "$PKG/LICENSES"
[ "$(stat -c %s "$PKG/LICENSES/GPL-2.0-or-later.txt")" -ge 10000 ] ||
  fail "LICENSES/GPL-2.0-or-later.txt looks wrong"
mkdir -p "$PKG/THIRD_PARTY"
cp "$ROOT/Externals/licenses.md" "$PKG/THIRD_PARTY/"
for dir in "$ROOT"/Externals/*/; do
  name="$(basename "$dir")"
  case "$name" in
  # Not in this build (libiconv: glibc's own is used).
  Qt | MoltenVK | Vulkan-Headers | VulkanMemoryAllocator | FFmpeg-bin | discord-rpc | gtest | mGBA | \
    miniupnpc | rcheevos | libadrenotools | wil | OpenAL | gettext | libiconv) continue ;;
  esac
  find "$dir" -maxdepth 2 -type f \
    \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' \) |
    while read -r file; do
      mkdir -p "$PKG/THIRD_PARTY/$name"
      cp "$file" "$PKG/THIRD_PARTY/$name/"
    done
done
cat >"$PKG/SOURCE.txt" <<EOF
Orca $VERSION is free software under the GNU GPL v2 or later. Source for this exact version: $SOURCE (also linked from yougame.co).

Built for Linux (x86-64) from commit $COMMIT$DIRTY. Orca is based on the Dolphin emulator
(https://github.com/dolphin-emu/dolphin). Needs glibc $GLIBC or newer, X11 (or XWayland), EGL and
OpenGL.

CMake settings:
$(grep -E '^(CMAKE_BUILD_TYPE|CMAKE_EXE_LINKER_FLAGS|ENABLE_[A-Z_]+|USE_[A-Z_]+|ENCODE_FRAMEDUMPS|ORCA_[A-Z_]+):' \
  "$BUILD_DIR/CMakeCache.txt" | sed 's/^/  /')
EOF
printf '%s\n' "Orca $VERSION (YouGame fork of Dolphin), Linux x64" "commit $COMMIT" "source $SOURCE" \
  'License: GPL-2.0-or-later (COPYING, LICENSES/GPL-2.0-or-later.txt).' >"$PKG/VERSION.txt"

if [ "$RELEASE" = 1 ]; then
  ZIP="$OUT_DIR/orca-$VERSION-linux-x64.zip"
else
  ZIP="$OUT_DIR/orca-$VERSION-linux-x64-dev.zip"
fi
rm -f "$ZIP"
# Info-ZIP keeps each file's Unix mode, so Orca/orca unpacks executable (unzip and bsdtar both
# restore it, and the app sets it again before a run). -X: no owner ids or extra timestamps.
(cd "$STAGE" && zip -q -r -X "$ZIP" Orca)
rm -rf "$STAGE"
echo "orca-package: $ZIP (entry Orca/orca, commit $COMMIT, glibc $GLIBC)"
sha256sum "$ZIP"
stat -c '%s bytes' "$ZIP"
