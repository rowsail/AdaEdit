#!/bin/sh
# AdaEdit AppImage hook -- sourced by linuxdeploy's generated AppRun (it sources
# $APPDIR/apprun-hooks/*.sh before launching the editor), so the Qt env it sets
# up still applies.  This adds the bundled ESP32-S3 toolchain / ALS / debug tools
# and seeds a WRITABLE SDK workspace on first run, because the AppImage's own
# filesystem is read-only (builds must write obj/, app.bin and unpacked runtimes
# somewhere).
: "${APPDIR:?APPDIR must be set by the AppRun}"

# Bundled, read-only toolchain.  The SDK build resolves the cross/native GNAT +
# gprbuild from here (Alire-free -- see the SDK's tools/sdk-env.sh).
export ESP32S3_ADA_TOOLCHAINS="$APPDIR/opt/toolchains"

# First run: copy the read-only SDK template into a writable per-user workspace.
__ae_data="${XDG_DATA_HOME:-$HOME/.local/share}/adaedit"
__ae_sdk="$__ae_data/sdk"
if [ ! -e "$__ae_sdk/tools/bin/esp32-ada" ]; then
    echo "AdaEdit: first run -- seeding SDK workspace at $__ae_sdk" >&2
    mkdir -p "$__ae_data"
    cp -a "$APPDIR/opt/sdk" "$__ae_sdk"
fi
export ESP32S3_ADA_SDK="$__ae_sdk"

# Tools on PATH: ALS (semantic features), OpenOCD + gdb (debug), the SDK's
# esp32-ada/x launcher, plus the cross + native toolchain (via the SDK helper).
export PATH="$APPDIR/opt/als/bin:$APPDIR/opt/debug/bin:$__ae_sdk/tools/bin:$PATH"
if [ -f "$__ae_sdk/tools/sdk-env.sh" ]; then
    . "$__ae_sdk/tools/sdk-env.sh"
    esp32s3_toolchain_on_path
fi
