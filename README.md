# AdaEdit

A native Qt5 editor — and, in its bundled form, a **completely self-contained
IDE** — for writing Ada and building / flashing / debugging the bare-metal
ESP32-S3 Ada runtime. A clean-room successor to SETEdit (Turbo Vision, text-mode)
rebuilt on Qt + QScintilla, reusing SETEdit's Ada highlighting rules and a
Borland-flavoured keybinding UX.

## Download — self-contained AppImage

Prebuilt Linux AppImages are published on the **[Releases page](https://github.com/rowsail/AdaEdit/releases/latest)**.
No install, no dependencies — download, `chmod +x`, run:

| Download | Size | What's inside |
|---|---|---|
| **`AdaEdit-full-x86_64.AppImage`** | ~680 MB | The whole IDE: editor + GNAT `xtensa-esp32-elf` cross-compiler + `gprbuild` + `ada_language_server` + OpenOCD/gdb + the ESP32-S3 SDK (runtime + `./x`). **Builds, flashes and debugs firmware with nothing else installed** — the 1980s Borland-box experience. |
| **`AdaEdit-x86_64.AppImage`** | ~30 MB | Editor only (Qt5 + QScintilla). Language and build features use whatever `ada_language_server` / toolchain are already on the host. |

```sh
chmod +x AdaEdit-full-x86_64.AppImage
./AdaEdit-full-x86_64.AppImage
# if FUSE isn't available (e.g. some WSL2 setups):
APPIMAGE_EXTRACT_AND_RUN=1 ./AdaEdit-full-x86_64.AppImage
```

- **Requirements:** x86_64, glibc ≥ 2.35 (built on Ubuntu 22.04). Runs natively on
  Linux and under **WSL2** on Windows (Win11/WSLg for the GUI).
- **First run** seeds a writable SDK workspace at `~/.local/share/adaedit/sdk`;
  later AppImage updates re-sync bundled SDK fixes into it automatically.
- **Flashing / on-chip debug** needs one-time USB device access — the editor
  offers **Build ▸ Set up device access…** (installs a udev rule; see §Device
  access). On WSL2 you also bridge the board in with `usbipd-win`.

To build the editor (or the AppImages) from source instead, see [§Build](#build).

## Highlights

- **Self-contained**: the full AppImage carries the entire toolchain, runtime,
  language server and debugger. Build/flash/debug works offline, out of the box.
- **The folder is the project** — open any folder; AdaEdit derives its build
  commands from the folder (an in-tree SDK example, or a standalone project).
- **Native serial monitor** and a **multi-board device selector** (no external
  terminal, no python).
- **Refactoring** via the Ada Language Server: project-wide **Rename** plus ALS
  code actions (Extract subprogram, Name parameters, …).

## Editing

- Tabbed Ada editor (QScintilla) with the `AdaLexer` (Ada 95 keywords from
  SETEdit + Ada 2005/2022 additions), line numbers, folding, auto-indent.
  Right-click a tab to **Close**, **Close all others**, or close everything **to
  the left / to the right**.
- File: New / Open / Save / Save As / **Save All** (with unsaved-changes guard).
- Edit: Undo / Redo / Cut / Copy / Paste, **Format (Ada)** (whole file or the
  selection, via the language server), **Rename symbol** (F2) and **Code
  actions** (Ctrl+.) — see §Refactoring.
- Search: Find / Replace / Find next / Go to line / Go to definition / Complete.
  (Replace acts on the whole document.)
- **Session restore**: closing the editor remembers the window size/position, the
  dock layout, the open files and the active tab — reopening restores them.
- **Settings** (File → Settings): **Dark mode**; separate **interface** font and
  **editor & dock** font (monospaced by default); and a **Keyboard Shortcuts**
  editor — every command is rebindable, with conflict detection and reset.

## Projects — the folder is the project

There is no separate project file. A **project is just a folder**, and AdaEdit
derives its Build/Flash/Run/Debug commands from the folder's structure:

- a folder under the SDK's `examples/` → driven via the SDK's **`./x {example}`**;
- a standalone folder (its own `app.gpr` + `build.sh`) → driven via
  **`esp32-ada -C {root}`** (a project can live anywhere on disk).

From the **File** menu:

- **New project…** — pick (or create) a folder; AdaEdit scaffolds a buildable
  standalone project into it (`esp32-ada init`: `src/main.adb`, `app.gpr` already
  `with`-ing the runtime + HAL, `board.ads`, `build.sh`, …).
- **Open project…** — open any folder (this also replaces the old "Open folder").
- **Save project as… (duplicate)** — copy the current project into a different /
  new folder (sources only — build outputs are skipped), then switch to the copy.

The explorer tree roots at the project folder; double-click any file to open it.

## ESP32-S3 build + runtime profile

- The editor targets the ESP32-S3 only. **Build / Flash / Run / Monitor** are on
  the toolbar and the Build menu; they run at the project root and **Save All**
  first, so they never compile stale source.
- **Runtime profile selector** (toolbar): *Auto (example default)* /
  *Jorvik (light-tasking)* / *Embedded* / *Full*. The choice maps to
  `ESP32S3_RTS_PROFILE` and selects the runtime
  (`crates/esp32s3_rts/<profile>-esp32s3`). Persisted in `QSettings`.
- **Show runtime path** (Build menu) echoes the runtime directory a build will
  link, flagging when that runtime isn't built yet.

## Serial monitor

A native **Serial monitor** dock backed by Qt's `QSerialPort` — no external
miniterm/picocom/screen, no python:

- pick the **port** and **baud**, **Connect**, watch live output, and send a line
  (selectable CR/LF). The dock has a View toggle and remembers the last port/baud.
- **Run** = build + flash, then auto-opens the monitor; **Flash/Run** release the
  port first so the flasher can use it.

## Device selection (multiple boards)

A **Device** dropdown on the toolbar enumerates connected USB serial devices —
labelled with description + serial number, so two identical boards are
distinguishable — plus a **Rescan** button. The selection is exported as
`$ESPPORT`, which steers **flash, debug and the monitor to the same board** at
once (OpenOCD maps the tty to that board's USB-JTAG adapter serial and pins it).
*Auto* leaves the SDK default (`/dev/ttyACM0`).

## Device access (flash / debug permissions)

Flashing (`/dev/ttyACM*`) and USB-JTAG debug need device permissions the AppImage
can't grant itself. **Build ▸ Set up device access…** runs the SDK's one-time
installer (via `pkexec`): it drops a udev rule (`60-esp32-ada.rules`, covering the
Espressif `303a` USB-JTAG and common CP210x/CH340/FTDI bridges) and adds you to
the device groups. The editor also offers this automatically if a flash/run fails
on permissions. Equivalent CLI: `./x setup-device`.

## Refactoring (Ada Language Server)

- **Rename symbol** (right-click ▸ Refactor, Edit menu, or **F2**): a project-wide
  rename via `textDocument/rename` — every file that references the symbol is
  updated (left modified for review → Save All).
- **Code actions** (right-click ▸ Refactor ▸ Code actions, Edit menu, or
  **Ctrl+.**): every refactoring/quick-fix ALS offers at the cursor — e.g.
  *Name parameters in the call*, *Extract Procedure* — applied via the server.

## Debugging (GDB/MI)

- **Start debugging** launches OpenOCD (:3333), runs gdb in MI mode, connects and
  stops at `app_main`. Continue / Step over / Step into / Restart / Stop / Pause
  drive the session; the current line is marked and a Debug console shows gdb
  output and takes raw gdb commands.
- **Breakpoints**: click the left margin (or Toggle breakpoint); a Breakpoints
  pane lists them (enable/disable, double-click to jump). They re-arm on each
  Start, are **cleared when you switch projects**, and fall back to matching by
  file name if the full path doesn't resolve.
- **Dual-core (SMP)** presents both LX7 cores as gdb threads; **Attach
  (post-mortem)** connects and `monitor halt`s in place (no reset) to inspect a
  hang. **Threads / Call stack / Variables & watch** panes track the stopped
  target.
- On-chip flash software-breakpoints are off by default (OpenOCD's flash
  auto-probe is unreliable for some bare-metal images and can reject the GDB
  connect); debug uses the two hardware breakpoints instead, which works in every
  profile. Set `ESP_FLASH_SIZE=<MB>` to re-enable flash breakpoints.

## Keyboard shortcuts

Defaults follow modern conventions for editing and Borland Turbo C/C++ for
build/debug. **All are rebindable** in Settings → Keyboard shortcuts.

| | |
|---|---|
| File | Ctrl+N New · Ctrl+O Open file · Ctrl+K Ctrl+O Open project · Ctrl+S Save · Ctrl+Shift+S Save As · Ctrl+Alt+S Save All · Ctrl+Q Exit |
| Edit | Ctrl+Z Undo · Ctrl+Y Redo · Ctrl+X/C/V Cut/Copy/Paste · Ctrl+Shift+F Format · **F2 Rename** · **Ctrl+. Code actions** |
| Search | Ctrl+F Find · Ctrl+H Replace · F3 Find next · Ctrl+G Goto line · F12 Go to definition · Ctrl+Space Complete |
| View | Ctrl+Shift+1..9 toggle docks |
| Build | F9 Build · Alt+F9 Flash · Shift+F9 Run · Alt+F5 Monitor |
| Debug | F5 Start · Ctrl+F9 Continue · F8 Step over · F7 Step into · Ctrl+F6 Pause · Ctrl+Shift+F2 Restart · Ctrl+F2 Stop · Ctrl+F8 Toggle breakpoint · Ctrl+F7 Add watch |

## Semantic features (Ada Language Server)

Opening an Ada file starts `ada_language_server` (bundled in the full AppImage, or
auto-located from PATH / the AdaCore VS Code/Zed extension) pointed at the nearest
`.gpr`. **Go to definition** (F12), **Quick info (hover)**, **Format**, **Rename**
and **Code actions** all flow over LSP/JSON-RPC; the buffer is synced before each
request. **Diagnostics** appear as squiggles and in a **Problems pane**.
**Completion** (Ctrl+Space) draws from ALS.

Note: ALS indexes the project for a few seconds after a file opens — a request
made before indexing finishes may return "not found"; just retry.

## Build

```sh
cmake -S . -B build
cmake --build build -j
./build/adaedit some_file.adb
```

The repo packages the AppImages with `packaging/build-appimage.sh`
(`BUNDLE=editor` or `BUNDLE=full`); external components are fetched at build time
and pinned in `packaging/manifest.env` (nothing heavy is committed). GitHub
Actions builds both on every push and publishes them on a `v*` tag.

### Build dependencies
Only Qt5 + QScintilla — no other third-party libraries.

| Dependency | Tested |
|---|---|
| C++17 compiler (GCC/Clang/MSVC) | — |
| CMake ≥ 3.16 | 3.31 |
| Qt5 — Widgets + PrintSupport + SerialPort | 5.15 |
| QScintilla for Qt5 (dev) | 2.14 |

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake qtbase5-dev qtbase5-dev-tools libqscintilla2-qt5-dev libqt5serialport5-dev
# Fedora
sudo dnf install gcc-c++ cmake qt5-qtbase-devel qscintilla-qt5-devel qt5-qtserialport-devel
# Arch
sudo pacman -S base-devel cmake qt5-base qscintilla-qt5 qt5-serialport
# macOS (Homebrew) — then:  cmake -S . -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qt@5)
brew install cmake qt@5 qscintilla2
```

CMake locates QScintilla via Qt's own `qmake` paths plus the common prefixes; if
it lives somewhere unusual, pass `-DQSCINTILLA_ROOT=<prefix>`.

The app compiles cross-platform, but Build/Flash/Run/Monitor/Debug shell out via
`/bin/sh` and `bash`, so full functionality assumes a POSIX environment
(Linux/macOS, or WSL2 on Windows).

### Runtime dependencies (source builds only)
The full AppImage bundles all of these; a from-source editor needs them on the
host for its actions to work:
- `ada_language_server` on `PATH` — completion, hover, diagnostics, format, refactor.
- The **GNAT `xtensa-esp32-elf`** toolchain + `gprbuild` (e.g. via **Alire**).
- The **`ada_esp32s3`** SDK — `./x` / `esp32-ada`, the runtime crate, build scripts.
- **OpenOCD** + the pinned xtensa esp **gdb** — for on-chip debugging.

## Layout

    CMakeLists.txt        build (Qt5 + QScintilla + SerialPort; portable QScintilla finder)
    src/main.cpp          entry point, CLI file args, session restore
    src/mainwindow.*      editor shell: tabs, menus, docks, settings, profile, device, actions
    src/adalexer.*        Ada highlighting via native Scintilla SCLEX_ADA
    src/project.*         derived ESP32-S3 command profiles + token expansion
    src/debugger.*        GDB/MI debug engine (server + gdb, step/continue/marker)
    src/lspclient.*       Ada Language Server client (definition, hover, completion, format, rename, code actions)
    src/serialmonitor.*   native QSerialPort serial console
    packaging/            AppImage build script, AppRun hook, pinned manifest
    docs/design-self-contained-ide.md  self-contained IDE design notes
