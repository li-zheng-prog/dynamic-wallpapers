#!/usr/bin/env bash
# ============================================================================
#  build.sh - Build the app and installer with MinGW-w64 g++.
#  Outputs: dist/DynamicWallpapers-portable.exe (no installation required)
#           dist/DynamicWallpapersSetup.exe (app and example themes, about 24 MB)
#  The application has used the name Dynamic Wallpapers since 2026-09-29.
#  Copy sources to D:\DynamicWallpapers_build because g++/windres cannot reliably
#  handle non-ASCII source paths, then copy the outputs back to dist/.
#  Usage: bash build/build.sh from Git Bash.
#  Requirements: D:\mingw64 (MinGW-w64 g++ 16+) and Python with Pillow.
# ============================================================================
set -e

GXX=/d/mingw64/bin/g++.exe
WINDRES=/d/mingw64/bin/windres.exe
ROOT="$(cd "$(dirname "$0")/.." && pwd)"       # MSYS path for cp/cd.
ROOTW="$(cd "$(dirname "$0")/.." && pwd -W)"   # Windows path for native tools.
BUILD="D:/DynamicWallpapers_build"
DIST="$ROOT/dist"
# APPEXE and PORTABLE have different purposes:
#   APPEXE is the compiled and installed filename referenced by setup.cpp kAppExe.
#   Shortcuts and uninstall entries rely on it; do not append -portable here.
#   PORTABLE is only the distribution filename used under dist/.
APPEXE="DynamicWallpapers.exe"
PORTABLE="DynamicWallpapers-portable.exe"
SETUPEXE="DynamicWallpapersSetup.exe"

CXXFLAGS="-std=c++17 -municode -O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 -Wall -Wno-unknown-pragmas"
LIBS="-lole32 -luuid -luser32 -lgdi32 -lshell32 -lshlwapi -ladvapi32 -ldwmapi -ld2d1 -ldwrite -ld3d11 -ldxgi -ldcomp -lwindowscodecs -lwinmm -limm32 -static -static-libgcc -static-libstdc++"

echo "==> Preparing build directory $BUILD"
rm -rf "$BUILD"
mkdir -p "$BUILD/src" "$BUILD/setup" "$BUILD/payload/themes" "$DIST"

echo "==> Copying source files"
cp "$ROOT"/src/*.cpp "$ROOT"/src/*.h "$ROOT"/src/*.rc "$BUILD/src/"
cp "$ROOT"/setup/*.cpp "$ROOT"/setup/*.rc "$BUILD/setup/"

echo "==> Generating icons"
python "$ROOTW/build/make_icon.py" "$BUILD/src/app.ico"

echo "==> Compiling application"
cd "$BUILD/src"
"$WINDRES" -I. --codepage=65001 -i app.rc -o app_res.o
"$GXX" $CXXFLAGS -mwindows -o "$APPEXE" \
    main.cpp gfx.cpp ui.cpp core.cpp thumbs.cpp app_res.o \
    -include windowsx.h $LIBS
cp "$APPEXE" "$DIST/$PORTABLE"
# Portable themes live beside the executable; dist/ is ignored by Git.
# Remove the old theme copy first to avoid nesting themes/themes on repeated builds.
rm -rf "$DIST/themes"
cp -r "$ROOT/themes" "$DIST/themes"
echo "    Portable app: $DIST/$PORTABLE"

# Build the installer payload with themes from the repository root.
echo "==> Collecting installer payload"
cp "$BUILD/src/$APPEXE" "$BUILD/payload/"
cp -r "$ROOT/themes/." "$BUILD/payload/themes/"

echo "==> Generating payload resources"
python "$ROOTW/build/pack_setup.py" "$BUILD/payload" "$BUILD/setup"

echo "==> Compiling installer"
cd "$BUILD/setup"
"$WINDRES" -I"$BUILD/src" --codepage=65001 -i setup.rc -o setup_res.o
"$WINDRES" -I"$BUILD/src" --codepage=65001 -i payload.rc -o payload_res.o
"$GXX" $CXXFLAGS -mwindows -o "$SETUPEXE" \
    setup.cpp "$BUILD/src/gfx.cpp" "$BUILD/src/ui.cpp" \
    setup_res.o payload_res.o -I"$BUILD/src" -include windowsx.h $LIBS
cp "$SETUPEXE" "$DIST/$SETUPEXE"

echo
echo "==> Build complete"
ls -la "$DIST/$PORTABLE" "$DIST/$SETUPEXE"
