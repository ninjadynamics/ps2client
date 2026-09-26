#!/bin/sh
# Refreshes lib/SDL3, the static SDL3 embedded in ps2client's gamepad input
# module (new controllers, such as a future DualSense, arrive with SDL).
#
#   sh lib/update-sdl3.sh              # newest MSYS2 mingw64 build
#   sh lib/update-sdl3.sh 3.4.16-1     # a specific package version
#
# lib/SDL3 is not committed: lib/sdl3.version (committed) pins the package,
# this script rewrites it, and Makefile.mingw32 fetches the pinned version
# when lib/SDL3 is missing. The package is downloaded and unpacked here only;
# pacman and the installed toolchain are never touched. Needs curl, tar and
# zstd (DreamSDK has them). Rebuild with `make ps2-input` from HyperSolar.
set -eu

REPO=https://repo.msys2.org/mingw/mingw64
PKG=mingw-w64-x86_64-sdl3
LIB=$(cd "$(dirname "$0")" && pwd)
DIR=$LIB/SDL3

if [ $# -ge 1 ]; then
    FILE=$PKG-$1-any.pkg.tar.zst
else
    FILE=$(curl -fsS "$REPO/" |
        grep -o "$PKG-[0-9][^\"]*-any\.pkg\.tar\.zst\"" | tr -d '"' |
        sort -V | tail -1)
    [ -n "$FILE" ] || { echo "update-sdl3: no $PKG package found at $REPO" >&2; exit 1; }
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
echo "update-sdl3: downloading $FILE"
curl -fsSL -o "$WORK/$FILE" "$REPO/$FILE"
zstd -dcq "$WORK/$FILE" | tar -xf - -C "$WORK" \
    mingw64/lib/libSDL3.a mingw64/lib/pkgconfig/sdl3.pc \
    mingw64/include/SDL3 mingw64/share/licenses/SDL3/LICENSE.txt

rm -rf "$DIR"
mkdir -p "$DIR/lib"
cp "$WORK/mingw64/lib/libSDL3.a" "$DIR/lib/"
cp -r "$WORK/mingw64/include" "$DIR/"
cp "$WORK/mingw64/share/licenses/SDL3/LICENSE.txt" "$DIR/"
# The system libraries a static link needs change between SDL releases.
sed -n 's/^Libs\.private: *//p' "$WORK/mingw64/lib/pkgconfig/sdl3.pc" > "$DIR/libs.txt"
sed -n 's/^Version: *//p' "$WORK/mingw64/lib/pkgconfig/sdl3.pc" > "$DIR/VERSION"
echo "$FILE" >> "$DIR/VERSION"
PIN=${FILE#"$PKG"-}
echo "${PIN%-any.pkg.tar.zst}" > "$LIB/sdl3.version"

echo "update-sdl3: SDL $(head -1 "$DIR/VERSION") ($(cat "$LIB/sdl3.version")) in $DIR"
