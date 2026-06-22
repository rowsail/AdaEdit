#!/bin/sh
# AdaEdit AppImage hook -- sourced by linuxdeploy's generated AppRun BEFORE it
# execs the editor.  That AppRun runs under `set -e`, so ANY non-zero command
# here would abort startup and the editor would never appear.  Everything below
# is therefore best-effort: we disable errexit for the hook's lifetime and guard
# the workspace sync so a hiccup can't stop the app from launching.
set +e

# APPDIR is normally exported by the AppImage runtime; derive it from our own
# location as a fallback so a missing APPDIR can't break startup.
if [ -z "${APPDIR:-}" ]; then
    APPDIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE:-$0}")/.." 2>/dev/null && pwd)"
    export APPDIR
fi

# Bundled, read-only toolchain.  The SDK build resolves the cross/native GNAT +
# gprbuild from here (Alire-free -- see the SDK's tools/sdk-env.sh).
export ESP32S3_ADA_TOOLCHAINS="$APPDIR/opt/toolchains"

# Seed a WRITABLE SDK workspace from the read-only bundle (the AppImage FS is
# read-only; builds must write obj/, app.bin and unpacked runtimes somewhere).
# Re-sync from the bundle whenever the AppImage is updated (.sdk-version differs)
# so bundled SDK fixes -- e.g. tools/openocd.sh -- reach an EXISTING workspace.
# cp -a of the bundle OVER the workspace overwrites tracked SDK files but leaves
# the user's build outputs / compiled runtimes intact.  Never fatal.
__ae_data="${XDG_DATA_HOME:-$HOME/.local/share}/adaedit"
__ae_sdk="$__ae_data/sdk"
__bundle_ver="$(cat "$APPDIR/opt/sdk/.sdk-version" 2>/dev/null)"
__ws_ver="$(cat "$__ae_sdk/.sdk-version" 2>/dev/null)"
# cp WITHOUT preserving ownership: the bundle's files come from a read-only
# squashfs whose uids may not be ours, and `cp -a`'s chown would fail (non-zero)
# for a non-root user.  Keep mode/timestamps/symlinks; skip ownership.
__ae_cp="cp -a --no-preserve=ownership"
if [ ! -e "$__ae_sdk/tools/bin/esp32-ada" ]; then
    echo "AdaEdit: first run -- seeding SDK workspace at $__ae_sdk" >&2
    mkdir -p "$__ae_data" && $__ae_cp "$APPDIR/opt/sdk" "$__ae_sdk" \
        || echo "AdaEdit: warning -- SDK workspace seed was incomplete" >&2
elif [ -n "$__bundle_ver" ] && [ "$__bundle_ver" != "$__ws_ver" ]; then
    echo "AdaEdit: updating bundled SDK in workspace (${__ws_ver:-old} -> $__bundle_ver)" >&2
    $__ae_cp "$APPDIR/opt/sdk/." "$__ae_sdk/" \
        || echo "AdaEdit: warning -- SDK workspace update was incomplete" >&2
fi
export ESP32S3_ADA_SDK="$__ae_sdk"

# The editor's repo-root / {repo} resolution honors ADAEDIT_HOME: it points at
# the bundled SDK (./x, tools/, crates/) so builds/flash/debug + "Set up device
# access" resolve correctly even when the user opens a project from OUTSIDE the
# seeded workspace (where walking up from the file would find no ./x launcher).
export ADAEDIT_HOME="$__ae_sdk"

# Tools on PATH: ALS (semantic features), OpenOCD + gdb (debug), the SDK's
# esp32-ada/x launcher, plus the cross + native toolchain (via the SDK helper).
export PATH="$APPDIR/opt/als/bin:$APPDIR/opt/debug/bin:$__ae_sdk/tools/bin:$PATH"
if [ -f "$__ae_sdk/tools/sdk-env.sh" ]; then
    . "$__ae_sdk/tools/sdk-env.sh"
    esp32s3_toolchain_on_path
fi
:   # ensure the hook's final status is success (belt-and-braces under set -e)
