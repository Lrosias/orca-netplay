#!/bin/bash
# Builds Orca.app for macOS (arm64) as a self-contained bundle and zips it.
#
#   Tools/orca-package-macos.sh [build-dir] [out-dir]
#     build-dir  default <repo>/build-orca-release (configured for a release on every run)
#     out-dir    default <build-dir>/package
#
# Without credentials the bundle stays unsigned (the executable keeps the linker's ad-hoc
# signature) and the zip is named *-unsigned.zip: it runs on this Mac, not through Gatekeeper on
# another. A release signs and notarizes, and needs all of:
#   CSC_LINK (Developer ID Application .p12) and CSC_KEY_PASSWORD
#   APPLE_API_KEY (.p8 path), APPLE_API_KEY_ID, APPLE_API_ISSUER
#   ORCA_SOURCE_URL: the public repository players can fetch the source from (GPL); its
#     orca-<version> tag must point at HEAD, and the tree must be clean before and after the build.
# ORCA_INTERNAL=1 instead signs and notarizes an internal test build from the private repository:
# the same credentials and clean tree, but no ORCA_SOURCE_URL or tag. SOURCE.txt says it is not for
# public distribution and the zip is named *-internal.zip. Such a build only goes to testers.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-orca-release}"
OUT_DIR="${2:-$BUILD_DIR/package}"
ENTITLEMENTS="$ROOT/Source/Core/DolphinNoGUI/Orca.entitlements"
APP="$BUILD_DIR/Binaries/Orca.app"
EXE="$APP/Contents/MacOS/Orca"

fail() {
  echo "orca-package: $*" >&2
  exit 1
}

[ "$(uname -s)" = Darwin ] || fail "macOS only"

RELEASE=0
INTERNAL=0
case "${ORCA_INTERNAL:-}" in
"" | 0) ;;
1) INTERNAL=1 ;;
*) fail "ORCA_INTERNAL is 1 or unset, not ${ORCA_INTERNAL}" ;;
esac
if [ "$INTERNAL" = 1 ]; then
  [ -z "${ORCA_SOURCE_URL:-}" ] || fail "ORCA_INTERNAL=1 is a private build; unset ORCA_SOURCE_URL"
  for var in CSC_LINK CSC_KEY_PASSWORD APPLE_API_KEY APPLE_API_KEY_ID APPLE_API_ISSUER; do
    [ -n "${!var:-}" ] || fail "an internal build is signed and needs $var"
  done
  RELEASE=1
elif [ -n "${CSC_LINK:-}${CSC_KEY_PASSWORD:-}${APPLE_API_KEY:-}${APPLE_API_KEY_ID:-}${APPLE_API_ISSUER:-}" ]; then
  for var in CSC_LINK CSC_KEY_PASSWORD APPLE_API_KEY APPLE_API_KEY_ID APPLE_API_ISSUER ORCA_SOURCE_URL; do
    [ -n "${!var:-}" ] || fail "a signed build is a release and also needs $var"
  done
  RELEASE=1
fi

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
TAG="$(git -C "$ROOT" tag --points-at HEAD --list 'orca-*' | head -n 1)"
VERSION="${ORCA_VERSION:-${TAG#orca-}}"
VERSION="${VERSION:-0.0.0}"
# Info.plist versions are one to three integers: 0.2.0-rc1 shows as 0.2.0.
PLIST_VERSION="$(echo "$VERSION" | grep -Eo '^[0-9]+(\.[0-9]+){0,2}' || true)"
PLIST_VERSION="${PLIST_VERSION:-0.0.0}"

if [ "$RELEASE" = 1 ]; then
  [ -z "$DIRTY" ] || fail "a signed build needs a clean tree (untracked files included)$DIRTY"
fi
if [ "$RELEASE" = 1 ] && [ "$INTERNAL" = 0 ]; then
  [ -n "$TAG" ] || fail "a release needs an orca-<version> tag on HEAD"
  # Anonymous, so a private repository can't pass: players must be able to fetch it.
  PUBLISHED="$(GIT_TERMINAL_PROMPT=0 git -c credential.helper= ls-remote "$ORCA_SOURCE_URL" \
    "refs/tags/$TAG" "refs/tags/$TAG^{}" 2>/dev/null | awk '{print $1}' | tail -n 1)" ||
    fail "can't read $ORCA_SOURCE_URL without credentials"
  [ "$PUBLISHED" = "$COMMIT" ] ||
    fail "$TAG at $ORCA_SOURCE_URL is ${PUBLISHED:-missing}, not HEAD ($COMMIT)"
fi

# A release links nothing outside macOS: bundled libraries (not Homebrew's), macOS's own iconv (so
# no LGPL code is bundled), no frame-dump encoder and none of the frontends' extras. Configured on
# every run so an older cache can't slip other settings in, and the bundle is rebuilt from nothing
# so files deleted from Sys don't linger in it.
rm -rf "$APP"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DENABLE_QT=OFF -DENABLE_NOGUI=ON -DORCA_MACOS_BUNDLE=ON -DORCA_VERSION="$PLIST_VERSION" \
  -DUSE_SYSTEM_LIBS=OFF -DUSE_SYSTEM_ICONV=ON -DENCODE_FRAMEDUMPS=OFF -DENABLE_LLVM=OFF \
  -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF -DUSE_DISCORD_PRESENCE=OFF \
  -DUSE_RETRO_ACHIEVEMENTS=OFF -DUSE_MGBA=OFF -DUSE_UPNP=OFF -DENABLE_VULKAN=OFF \
  -DENABLE_CLI_TOOL=OFF -DENABLE_TESTS=OFF -DUSE_SANITIZERS=OFF >/dev/null
cmake --build "$BUILD_DIR" --target dolphin-nogui

if [ "$RELEASE" = 1 ]; then
  # The source must not have moved under the build.
  [ "$(git -C "$ROOT" rev-parse HEAD)" = "$COMMIT" ] && [ -z "$(tree_state)" ] ||
    fail "the tree changed during the build; start again from a clean checkout of ${TAG:-$COMMIT}"
fi

[ -x "$EXE" ] || fail "no $EXE"
[ -d "$APP/Contents/Resources/Sys" ] && [ ! -L "$APP/Contents/Resources/Sys" ] ||
  fail "Sys is missing or a link in $APP"

# Self-contained: every library the executable loads ships with macOS.
FOREIGN="$(otool -L "$EXE" | tail -n +2 | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/' || true)"
[ -z "$FOREIGN" ] || fail "Orca links libraries that don't ship with macOS:
$FOREIGN"

# What the GPL asks to travel with the binary: where the source is, and the bundled licences.
RESOURCES="$APP/Contents/Resources"
if [ "$INTERNAL" = 1 ]; then
  SOURCE_LINE="Internal test build of Orca $COMMIT from a private repository. Not for public distribution."
else
  SOURCE_LINE="Source:  ${ORCA_SOURCE_URL:-not published (a development build)}"
fi
cat >"$RESOURCES/SOURCE.txt" <<EOF
Orca $VERSION for macOS (arm64)

Orca is free software under the GNU General Public License; see COPYING and LICENSES/.
It is based on the Dolphin emulator source (https://github.com/dolphin-emu/dolphin).

$SOURCE_LINE
Commit:  $COMMIT$DIRTY
Tag:     ${TAG:-none}

CMake settings:
$(grep -E '^(CMAKE_BUILD_TYPE|CMAKE_OSX_ARCHITECTURES|CMAKE_OSX_DEPLOYMENT_TARGET|ENABLE_[A-Z_]+|USE_[A-Z_]+|ENCODE_FRAMEDUMPS|ORCA_[A-Z_]+):' \
  "$BUILD_DIR/CMakeCache.txt" | sed 's/^/  /')
EOF
rm -rf "$RESOURCES/THIRD_PARTY"
mkdir -p "$RESOURCES/THIRD_PARTY"
cp "$ROOT/Externals/licenses.md" "$RESOURCES/THIRD_PARTY/"
for dir in "$ROOT"/Externals/*/; do
  name="$(basename "$dir")"
  case "$name" in
  # Not in Orca.app (libiconv: macOS's own is used).
  Qt | MoltenVK | Vulkan-Headers | VulkanMemoryAllocator | FFmpeg-bin | discord-rpc | gtest | mGBA | \
    miniupnpc | rcheevos | libadrenotools | wil | OpenAL | gettext | libiconv) continue ;;
  esac
  find "$dir" -maxdepth 2 -type f \
    \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' \) |
    while read -r file; do
      mkdir -p "$RESOURCES/THIRD_PARTY/$name"
      cp "$file" "$RESOURCES/THIRD_PARTY/$name/"
    done
done

mkdir -p "$OUT_DIR"
if [ "$RELEASE" = 0 ]; then
  ZIP="$OUT_DIR/Orca-$VERSION-macos-arm64-unsigned.zip"
  rm -f "$ZIP"
  ditto -c -k --keepParent "$APP" "$ZIP"
  echo "orca-package: no signing credentials; Orca.app stays unsigned"
  echo "orca-package: $ZIP"
  shasum -a 256 "$ZIP"
  exit 0
fi

KEYCHAIN_DIR="$(mktemp -d)"
KEYCHAIN="$KEYCHAIN_DIR/orca-signing.keychain-db"
KEYCHAIN_PASSWORD="$(uuidgen)"
OLD_KEYCHAINS="$(security list-keychains -d user | tr -d '"')"
cleanup() {
  security delete-keychain "$KEYCHAIN" || true
  # shellcheck disable=SC2086
  security list-keychains -d user -s $OLD_KEYCHAINS || true
  rm -rf "$KEYCHAIN_DIR"
}
trap cleanup EXIT
security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security set-keychain-settings -lut 3600 "$KEYCHAIN"
security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security import "$CSC_LINK" -k "$KEYCHAIN" -P "$CSC_KEY_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KEYCHAIN_PASSWORD" \
  "$KEYCHAIN" >/dev/null
# shellcheck disable=SC2086
security list-keychains -d user -s "$KEYCHAIN" $OLD_KEYCHAINS
IDENTITY="$(security find-identity -v -p codesigning "$KEYCHAIN" |
  awk '/Developer ID Application/ {print $2; exit}')"
[ -n "$IDENTITY" ] || fail "no valid Developer ID Application identity in CSC_LINK (is Apple's \
Developer ID Certification Authority G2 certificate in a keychain on this Mac?)"
codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" \
  --keychain "$KEYCHAIN" --sign "$IDENTITY" "$APP"
codesign --verify --strict --verbose=2 "$APP"

ZIP="$OUT_DIR/Orca-$VERSION-macos-arm64.zip"
[ "$INTERNAL" = 0 ] || ZIP="$OUT_DIR/Orca-$VERSION-macos-arm64-internal.zip"
RESULT="$KEYCHAIN_DIR/notary.json"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --key "$APPLE_API_KEY" --key-id "$APPLE_API_KEY_ID" \
  --issuer "$APPLE_API_ISSUER" --wait --output-format json >"$RESULT" || true
STATUS="$(plutil -extract status raw -o - "$RESULT" 2>/dev/null || echo unknown)"
if [ "$STATUS" != Accepted ]; then
  SUBMISSION="$(plutil -extract id raw -o - "$RESULT" 2>/dev/null || true)"
  [ -z "$SUBMISSION" ] || xcrun notarytool log "$SUBMISSION" --key "$APPLE_API_KEY" \
    --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER" >&2 || true
  rm -f "$ZIP"
  fail "notarization: $STATUS"
fi
xcrun stapler staple "$APP"
spctl --assess --type execute --verbose "$APP"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "orca-package: $ZIP"
shasum -a 256 "$ZIP"
