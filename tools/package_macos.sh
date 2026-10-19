#!/bin/zsh
# Builds a portable macOS app bundle of ki_play: dist/KIRecompiled.app and
# dist/KIRecompiled-macOS-arm64.zip.
#
# - SDL3 and sdl2-compat are built from source with a macOS 11 deployment
#   target into deps-macos/ (ignored by Git), so the bundle does not depend on
#   Homebrew or on this machine's macOS version.
# - The game is recompiled from the build machine's game files as usual
#   (data/, see README); the bundle contains the recompiled code but no game
#   data. At run time the player points it at their own MAME set.
#
# usage: tools/package_macos.sh [version]
set -euo pipefail

VERSION=${1:-0.1.0}
SDL3_VERSION=3.4.18
SDL2_COMPAT_VERSION=2.32.74
DEPLOYMENT_TARGET=11.0

ROOT=${0:A:h:h}
DEPS=$ROOT/deps-macos
BUILD=$ROOT/build-release
DIST=$ROOT/dist
APP=$DIST/KIRecompiled.app

# ---- dependencies ----
if [[ ! -f "$DEPS/lib/libSDL2-2.0.0.dylib" || ! -f "$DEPS/lib/libSDL3.0.dylib" ]]; then
    mkdir -p "$DEPS/src"
    [[ -d "$DEPS/src/SDL3" ]] || git clone --depth 1 --branch "release-$SDL3_VERSION" \
        https://github.com/libsdl-org/SDL.git "$DEPS/src/SDL3"
    cmake -S "$DEPS/src/SDL3" -B "$DEPS/src/SDL3/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF
    cmake --build "$DEPS/src/SDL3/build"
    cmake --install "$DEPS/src/SDL3/build"
    [[ -d "$DEPS/src/sdl2-compat" ]] || git clone --depth 1 --branch "release-$SDL2_COMPAT_VERSION" \
        https://github.com/libsdl-org/sdl2-compat.git "$DEPS/src/sdl2-compat"
    cmake -S "$DEPS/src/sdl2-compat" -B "$DEPS/src/sdl2-compat/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DCMAKE_PREFIX_PATH="$DEPS" -DSDL2COMPAT_TESTS=OFF
    cmake --build "$DEPS/src/sdl2-compat/build"
    cmake --install "$DEPS/src/sdl2-compat/build"
fi

# ---- build ----
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET -DCMAKE_PREFIX_PATH="$DEPS" \
    -DCMAKE_FIND_FRAMEWORK=NEVER -DKI_BUILD_TRACE=OFF
cmake --build "$BUILD" --target ki_play

# ---- bundle ----
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BUILD/ki_play" "$APP/Contents/MacOS/KIRecompiled"
cp "$DEPS/lib/libSDL2-2.0.0.dylib" "$APP/Contents/Frameworks/"
# sdl2-compat dlopens SDL3 as @loader_path/libSDL3.dylib.
cp "$DEPS/lib/libSDL3.0.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"
sdl2=$(otool -L "$APP/Contents/MacOS/KIRecompiled" | awk '/libSDL2/ {print $1}')
install_name_tool -change "$sdl2" @rpath/libSDL2-2.0.0.dylib "$APP/Contents/MacOS/KIRecompiled"
# Drop the build tree's rpaths (they point into deps-macos/), keep only the bundle's.
for rp in $(otool -l "$APP/Contents/MacOS/KIRecompiled" | awk '/LC_RPATH/ {getline; getline; print $2}'); do
    install_name_tool -delete_rpath "$rp" "$APP/Contents/MacOS/KIRecompiled"
done
install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/MacOS/KIRecompiled"
for lib in "$APP"/Contents/Frameworks/*.dylib; do
    install_name_tool -id "@rpath/${lib:t}" "$lib"
done
if [[ -f "$ROOT/tools/macos/AppIcon.icns" ]]; then
    cp "$ROOT/tools/macos/AppIcon.icns" "$APP/Contents/Resources/"
fi
sed -e "s/@VERSION@/$VERSION/g" "$ROOT/tools/macos/Info.plist.in" > "$APP/Contents/Info.plist"

# Licenses of everything compiled into or shipped with the app.
CHDR=$BUILD/_deps/libchdr-src
{
    print "Killer Instinct Recompiled $VERSION\n"
    print "Unofficial fan project, not affiliated with or endorsed by Rare, Midway, Nintendo or Microsoft."
    print "No game data is included; the program needs the player's own MAME 'kinst' set.\n"
    for title file in \
        "ki-recomp (MIT)" "$ROOT/LICENSE" \
        "ADSP-2105 core, derived from MAME (BSD-3-Clause)" "$ROOT/third_party/adsp2100/LICENSE" \
        "libchdr (BSD-3-Clause)" "$CHDR/LICENSE.txt" \
        "LZMA SDK (public domain)" "$CHDR/deps/lzma-26.02/LICENSE" \
        "Zstandard decompressor (BSD)" "$ROOT/tools/macos/LICENSE-zstd.txt" \
        "SDL3 (zlib)" "$DEPS/src/SDL3/LICENSE.txt" \
        "sdl2-compat (zlib)" "$DEPS/src/sdl2-compat/LICENSE.txt"; do
        print "==== $title ====\n"
        cat "$file"
        print ""
    done
    print "==== miniz (MIT) ====\n"
    sed -n '2,24p' "$CHDR/deps/miniz-3.1.2/miniz.c"
} > "$APP/Contents/Resources/LICENSES.txt"
cp "$APP/Contents/Resources/LICENSES.txt" "$DIST/LICENSES.txt"
codesign --force --deep --sign - "$APP"

# Nothing outside the bundle and the system may be linked, and the app must
# start with deps-macos/ out of the way (loads only its own SDL).
mv "$DEPS" "$DEPS.hidden"
trap 'mv "$DEPS.hidden" "$DEPS"' EXIT
probe=$DIST/dyld.log
DYLD_PRINT_LIBRARIES=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$APP/Contents/MacOS/KIRecompiled" --mute > "$probe" 2>&1 &
pid=$!
for i in {1..30}; do
    grep -q "libSDL3" "$probe" && break
    sleep 0.5
done
kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true
loaded=$(grep -i sdl "$probe" || true)
rm -f "$probe"
echo "$loaded"
if ! echo "$loaded" | grep -q "KIRecompiled.app/Contents/Frameworks/libSDL3"; then
    echo "error: the app does not load its own SDL3" >&2
    exit 1
fi
if otool -L "$APP/Contents/MacOS/KIRecompiled" "$APP"/Contents/Frameworks/*.dylib |
        grep -E '^\s+/(opt|usr/local|Users)/'; then
    echo "error: the bundle links libraries outside itself" >&2
    exit 1
fi

rm -f "$DIST/KIRecompiled-macOS-arm64.zip"
rm -rf "$DIST/zip" && mkdir "$DIST/zip"
cp -R "$APP" "$DIST/LICENSES.txt" "$DIST/zip/"
(cd "$DIST" && ditto -c -k --norsrc zip KIRecompiled-macOS-arm64.zip)
rm -rf "$DIST/zip"
echo "built $APP and $DIST/KIRecompiled-macOS-arm64.zip (version $VERSION)"
