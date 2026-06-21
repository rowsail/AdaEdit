# Design: Self-Contained AdaEdit IDE (AppImage)

Status: **feasibility / design notes** (not yet implemented)

## 1. Goal

Ship AdaEdit as a *completely self-contained* IDE: a user downloads one
AppImage, double-clicks it, and has the editor **plus** every tool needed to
build, flash and debug bare-metal Ada on the ESP32-S3 — no system package
installs, no Alire fetch, no toolchain hunting.

The model is the 1980s Borland boxed product (Turbo C / Turbo Pascal): editor +
compiler + libraries + debugger, all in the box, working out of the box. The
modern difference is that the host is **not** the target — we cross-compile and
flash over USB — which is the source of the one unavoidable caveat (§6).

## 2. Verdict

**Feasible.** An AppImage is a self-mounting compressed filesystem, so it can
carry arbitrary binaries and trees, not just the Qt app. The two real pieces of
work are (a) handling the **read-only** AppImage filesystem vs. things that
currently expect to write inside the repo, and (b) making the build run
**Alire-free** against a bundled toolchain. One thing cannot be fully
self-contained on modern Linux: **USB/serial device permissions** for flashing
and on-chip debug (§6).

## 3. What ships in the bundle

Everything the current build/debug flow already reaches for:

| Component | Source today | Approx size |
|---|---|---|
| AdaEdit + Qt5 (Widgets, PrintSupport) + QScintilla | this repo | ~50 MB |
| GNAT `xtensa-esp32-elf` cross-compiler + `gprbuild` | Alire toolchain (`~/.local/share/alire/toolchains/gnat_xtensa_esp32_elf_*`) | ~400–800 MB |
| `ada_language_server` (completion/hover/diagnostics/format) | PATH / AdaCore VS Code/Zed extension | ~100 MB |
| OpenOCD + pinned xtensa esp `gdb` | `ada-bare-metal-esp32s3/tools/get-{openocd,gdb}.sh` | ~50 MB |
| `ada-bare-metal-esp32s3`: `./x`, RTS sources, build scripts, **prebuilt runtimes** | that repo | ~50–200 MB |
| esptool/espflash + bare-boot bootloader tooling | bare-boot tooling | small |

Total: a **~1–1.5 GB single file**. Large but unremarkable for an AppImage that
carries a compiler. Everything is userspace; the ESP32-S3 target is identical
regardless of host architecture.

## 4. Architecture

### 4.1 AppDir layout (sketch)

```
AdaEdit.AppDir/
  AppRun                      # entry point: sets env, seeds workspace, launches editor
  adaedit.desktop, adaedit.png
  usr/
    bin/adaedit               # the editor
    lib/                      # Qt5, QScintilla, platform plugins (via linuxdeploy)
  opt/
    toolchain/                # GNAT xtensa-esp32-elf cross-compiler + gprbuild
    als/                      # ada_language_server
    debug/                    # openocd, xtensa-esp32s3-elf-gdb
    bare-metal/               # ada-bare-metal-esp32s3 tree (./x, crates/, tools/)
      crates/esp32s3_rts/
        light-tasking-esp32s3/   # PREBUILT
        embedded-esp32s3/        # PREBUILT
        full-esp32s3/            # PREBUILT
```

### 4.2 AppRun responsibilities

- Prepend `opt/toolchain/bin`, `opt/als`, `opt/debug` to `PATH` so the editor's
  existing auto-location (ALS already searches `PATH`) and the build commands
  find the bundled tools.
- Export an `ADAEDIT_HOME` (or similar) pointing at the bundled `bare-metal/`
  tree, which the editor uses to resolve `{repo}` and the debug-server path
  instead of walking up from the open file.
- On first run, **seed a writable workspace** (see §5.1).
- `exec usr/bin/adaedit "$@"`.

### 4.3 Writable workspace model

The user's projects must live somewhere writable (the AppImage is read-only).
On first run, AppRun copies the example skeletons + a "new project" template into
`$XDG_DATA_HOME/adaedit/workspace` (default `~/.local/share/adaedit/workspace`),
and the editor opens projects from there. Build *outputs* (`obj/`, `app.elf`,
`app.bin`) land in the user's project dir, as today.

## 5. Key challenges and mitigations

### 5.1 Read-only AppImage filesystem
Two things currently write inside the repo:

- **RTS runtimes are generated on first use** (`gen_runtime.sh` →
  `crates/esp32s3_rts/<profile>-esp32s3`). Mitigation: **ship all three profiles
  prebuilt** in the AppImage (read-only is fine for prebuilt). If on-demand
  generation of an unbundled profile is ever needed, generate into the writable
  workspace, not the AppImage.
- **The `./x` flow assumes the example lives inside the (writable) repo.**
  Mitigation: the seeded writable workspace (§4.3) holds the examples; `./x` runs
  with the bundled `crates/` referenced read-only and per-project outputs written
  in the workspace.

### 5.2 Alire decoupling
Today the build leans on Alire (`alr`) for the toolchain and path-pins. A
self-contained bundle must not require network or `alr` at runtime. Work:
make `./x` / `build.sh` run against the bundled `gprbuild` + prebuilt runtimes
directly (resolve `for Runtime ("Ada")` to the bundled `…-esp32s3` paths;
drop the `alr exec`/path-pin assumptions for the bundled case). **This is the
main non-packaging effort** and should be proven first (§7, Phase 0).

### 5.3 Relocatability
GCC/GNAT toolchains locate their libraries relative to the binary, and the Alire
toolchains already run from an arbitrary prefix, so this mostly works. The
`.gpr` runtime path and any hardcoded tool paths must resolve inside the AppDir
(handled by `AppRun` env + the Alire-free build).

### 5.4 glibc compatibility
Build the AppImage on an **old** base (e.g. Ubuntu 20.04 / Debian 11) so it runs
on a wide range of distros. Building on Debian 13 would restrict it to
equally-new systems. (Confirm the prebuilt GNAT/ALS binaries' own minimum
glibc — typically fine on anything modern.)

### 5.5 Per-architecture builds
One AppImage per host arch (`x86_64`, `aarch64`). The ESP32-S3 cross-target is
the same for both.

## 6. The unavoidable caveat: USB/serial permissions

Flashing (`/dev/ttyACM*`) and on-chip debug (USB-JTAG via OpenOCD) require the
user to be in `dialout` or to have a udev rule — and an AppImage **cannot install
a udev rule without root**. So:

- **Edit / build / inspect**: fully turnkey from the AppImage.
- **Flash / debug real hardware**: needs a **one-time host step** — add the user
  to a group, or run a tiny `sudo` helper that drops in one udev rule.

This is the modern-Linux tax 80s Borland never paid: back then the OS let any
program drive the serial/parallel port directly; today the kernel gates USB
devices. It is one documented step, not a blocker. We can ship the udev rule + a
one-line installer and surface a clear in-editor message when device access
fails.

## 7. Phasing / roadmap

- **Phase 0 — Alire-free build.** Make `./x build|flash|run` work against a
  bundled (or fixed-path) `gprbuild` + prebuilt runtimes with no `alr`/network.
  Pure shell/gpr work; de-risks the whole effort. *(prerequisite)*
- **Phase 1 — Editor AppImage.** Package just AdaEdit + Qt5 + QScintilla via
  `linuxdeploy` + the Qt plugin, built on an old glibc base. Proves the GUI
  bundling and `AppRun`.
- **Phase 2 — Full toolchain bundle.** Add the cross-toolchain, ALS,
  OpenOCD/gdb, the bare-metal tree with prebuilt runtimes; implement `AppRun`
  env + first-run workspace seeding; wire the editor's `{repo}` / debug paths to
  the bundle.
- **Phase 3 — Device access.** Ship the udev rule + `sudo` installer and an
  in-editor "device not accessible — run setup" path.
- **Phase 4 — CI artifact.** Extend GitHub Actions to build and upload the
  AppImage(s) per arch, alongside the existing Debian binary job.

## 8. Editor-side changes implied

Small, and mostly already half-present:

- `findRepoRoot` / `{repo}` resolution should honor an `ADAEDIT_HOME` set by
  `AppRun` (fall back to today's walk-up behavior when unset).
- The debug-server command (`bash {repo}/tools/openocd.sh`) and `gdb` resolve to
  bundled tools via `PATH` / `ADAEDIT_HOME`.
- First-run workspace seeding (a startup check, optionally a small dialog).
- ALS already auto-locates from `PATH`; `AppRun` just needs to put it there.

## 9. Alternatives considered

- **Flatpak**: better desktop integration, but its sandbox makes raw USB/serial
  access *harder* (needs portal/`--device` grants) — worse for flashing/debug.
- **Plain tarball / self-extracting installer**: simplest to produce, but not
  the "one file, double-click" experience; still needs the device step.
- **Docker/Podman image**: great for reproducible *builds*, poor for a GUI +
  USB device workflow on an end-user desktop.

AppImage is the closest match to the Borland-box, single-download vision while
keeping direct host device access (modulo §6).

## 10. Open questions

- Exact minimum glibc of the prebuilt GNAT and ALS binaries (sets the build base).
- How tightly `./x` / `build.sh` are coupled to Alire today (scope of Phase 0).
- Whether to ship one "full" AppImage (all runtimes prebuilt, ~1.5 GB) or also a
  "lite" one (runtimes generated into the workspace on first use, smaller download).
- Update/auto-update story (AppImageUpdate delta updates vs. full re-download).
