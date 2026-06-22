#!/usr/bin/env bash
# Build a self-contained AdaEdit AppImage (the editor + Qt5 + QScintilla).
# Phase 1 of the self-contained IDE (docs/design-self-contained-ide.md): proves
# the GUI bundling + AppRun.  The toolchain/RTS bundle is Phase 2.
#
# For broad-distro compatibility build this on an OLD glibc base (e.g. Ubuntu
# 20.04) -- the resulting AppImage runs on that glibc and newer.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TOOLS="$HERE/tools"
APPDIR="$HERE/AppDir"
ARCH="${ARCH:-x86_64}"
export APPIMAGE_EXTRACT_AND_RUN=1          # run nested AppImages without FUSE

mkdir -p "$TOOLS"
fetch () { [ -f "$2" ] || { echo "fetch $(basename "$2")"; curl -fsSL "$1" -o "$2"; chmod +x "$2"; }; }
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" \
      "$TOOLS/linuxdeploy-$ARCH.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" \
      "$TOOLS/linuxdeploy-plugin-qt-$ARCH.AppImage"

echo "== build the editor (Release) =="
cmake -S "$ROOT" -B "$ROOT/build-rel" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build-rel" -j"$(nproc)" >/dev/null

echo "== assemble AppDir =="
rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr/bin"
cp "$ROOT/build-rel/adaedit" "$APPDIR/usr/bin/"

echo "== linuxdeploy + qt plugin -> AppImage =="
export PATH="$TOOLS:$PATH"
export OUTPUT="AdaEdit-$ARCH.AppImage"
cd "$HERE"
"$TOOLS/linuxdeploy-$ARCH.AppImage" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/adaedit" \
  --desktop-file "$HERE/adaedit.desktop" \
  --icon-file "$HERE/adaedit.png" \
  --plugin qt \
  --output appimage

echo "== done: $HERE/$OUTPUT =="
ls -lh "$HERE/$OUTPUT"
