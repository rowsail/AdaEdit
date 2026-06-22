#!/usr/bin/env bash
# Build an AdaEdit AppImage.
#
#   BUNDLE=editor (default)  the editor + Qt5 + QScintilla only (Phase 1).
#   BUNDLE=full              + the ESP32-S3 toolchain, ALS, OpenOCD/gdb and the
#                            ada_esp32s3 SDK (with runtime packs) -- a complete,
#                            self-contained IDE (Phase 2).  ~1-1.5 GB.
#
# Nothing here is committed to git: external packages are FETCHED at build time
# (pinned in manifest.env) and the AppImage is a release/CI artifact.  Build on
# an OLD glibc base (e.g. Ubuntu 20.04) for broad-distro compatibility.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TOOLS="$HERE/tools"
APPDIR="$HERE/AppDir"
ARCH="${ARCH:-x86_64}"
BUNDLE="${BUNDLE:-editor}"
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

echo "== assemble AppDir ($BUNDLE) =="
rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr/bin"
cp "$ROOT/build-rel/adaedit" "$APPDIR/usr/bin/"

if [ "$BUNDLE" = full ]; then
    # shellcheck disable=SC1091
    . "$HERE/manifest.env"
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

    echo "  -- SDK (with runtime packs) -> opt/sdk"
    git clone --depth 1 --branch "$SDK_REF" "$SDK_REPO" "$tmp/sdk"
    mkdir -p "$APPDIR/opt/sdk"
    ( cd "$tmp/sdk" && git archive HEAD ) | tar -x -C "$APPDIR/opt/sdk"   # committed files only

    echo "  -- GNAT toolchains (Alire-fetched) -> opt/toolchains"
    command -v alr >/dev/null || { echo "need 'alr' to fetch toolchains" >&2; exit 1; }
    alr -n toolchain --select "$GNAT_XTENSA_CRATE=$TOOLCHAIN_GNAT_VERSION" || true
    alr -n toolchain --select "$GNAT_NATIVE_CRATE=$TOOLCHAIN_GNAT_VERSION" || true
    alr -n toolchain --select "$GPRBUILD_CRATE=$TOOLCHAIN_GPRBUILD_VERSION" || true
    mkdir -p "$APPDIR/opt/toolchains"
    for d in "$HOME"/.local/share/alire/toolchains/gnat_xtensa_esp32_elf_* \
             "$HOME"/.local/share/alire/toolchains/gnat_native_* \
             "$HOME"/.local/share/alire/toolchains/gprbuild_*; do
        [ -d "$d" ] && cp -a "$d" "$APPDIR/opt/toolchains/"
    done

    echo "  -- ada_language_server -> opt/als"
    mkdir -p "$APPDIR/opt/als"
    curl -fsSL "$ALS_URL" -o "$tmp/als.tar.gz"
    tar -xzf "$tmp/als.tar.gz" -C "$APPDIR/opt/als"
    als_bin="$(find "$APPDIR/opt/als" -type f -name ada_language_server | head -1)"
    [ -n "$als_bin" ] || { echo "ALS binary not found in tarball" >&2; exit 1; }
    mkdir -p "$APPDIR/opt/als/bin"
    # relative symlink so it resolves wherever the AppImage mounts
    ln -sf "$(realpath --relative-to="$APPDIR/opt/als/bin" "$als_bin")" \
           "$APPDIR/opt/als/bin/ada_language_server"

    echo "  -- OpenOCD + gdb (the SDK's own fetchers) -> opt/sdk/tools"
    ( cd "$APPDIR/opt/sdk" && bash tools/get-openocd.sh && bash tools/get-gdb.sh ) || \
        echo "  (warning: debug-tool fetch failed; debugging won't work until they're present)"
    mkdir -p "$APPDIR/opt/debug/bin"   # reserved (PATH'd by the hook)

    echo "  -- AppRun hook (toolchain/SDK env + first-run workspace seeding)"
    mkdir -p "$APPDIR/apprun-hooks"
    cp "$HERE/apprun-hook.sh" "$APPDIR/apprun-hooks/zz-adaedit.sh"
fi

echo "== linuxdeploy + qt plugin -> AppImage =="
export PATH="$TOOLS:$PATH"
sfx=""; [ "$BUNDLE" = full ] && sfx="-full"
export OUTPUT="AdaEdit${sfx}-$ARCH.AppImage"
cd "$HERE"
"$TOOLS/linuxdeploy-$ARCH.AppImage" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/adaedit" \
  --desktop-file "$HERE/adaedit.desktop" \
  --icon-file "$HERE/adaedit.png" \
  --plugin qt \
  --output appimage

echo "== done: $HERE/$OUTPUT =="
ls -lh "$HERE"/AdaEdit*"$ARCH".AppImage
