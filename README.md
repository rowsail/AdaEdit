# AdaEdit

A native Qt5 editor for writing Ada and building/debugging the bare-metal
ESP32-S3 Ada runtime. A clean-room successor to SETEdit (Turbo Vision,
text-mode) rebuilt on Qt + QScintilla, reusing SETEdit's Ada highlighting rules
and a Borland-flavoured keybinding UX.

## Status: scaffold (v0.1)

### Editing
- Tabbed Ada editor (QScintilla) with the `AdaLexer` (Ada 95 keywords from
  SETEdit + Ada 2005/2022 additions), line numbers, folding, auto-indent.
- File: New / Open / Save / Save As (with unsaved-changes guard); open files or a
  folder from the command line.
- Edit: Undo / Redo / Cut / Copy / Paste, and **Format (Ada)** — reformats the
  whole file, or just the selected lines, via the language server.
- Search: Find / Replace / Find next / Go to line / Go to definition / Complete.
- **Session restore**: closing the editor remembers the window size/position, the
  dock layout (which docks are shown/hidden + their sizes), the open files and the
  active tab — reopening with no arguments restores the previous session.

### Workspace
- **Open a folder as a project**: File → Open folder (or `adaedit <dir>`) roots
  the explorer tree at the folder; double-click any file to open it. A `.adaproj`
  in the folder loads automatically.
- **View menu**: a checkable entry per dock (Project, Output, Debug console,
  Breakpoints, Variables, Threads, Call stack, Problems) — toggles visibility,
  default shortcuts **Ctrl+Shift+1..8**.
- **Settings** (File → Settings): **Dark mode**; separate **interface** font
  (menus/titles) and **editor & dock** font (monospaced by default); and a
  **Keyboard Shortcuts** editor — every command is rebindable, with conflict
  detection, per-row + global reset, persisted to `QSettings`.

### ESP32-S3 build + runtime profile
- The editor targets the ESP32-S3 only and drives the repo's **`./x`** launcher
  for **Build / Flash / Run / Monitor** (toolbar buttons and the Build menu).
  Commands run at the project root and expand `{repo}`, `{root}`, `{example}`,
  `{profile}`, `{file}`, `{dir}`, `{base}`, `{exe}`.
- **Runtime profile selector** (toolbar): *Auto (example default)* /
  *Jorvik (light-tasking)* / *Embedded* / *Full*. The choice is passed to
  `./x … --profile <p>`, which maps it to `ESP32S3_RTS_PROFILE` (the external the
  runtime crate reads); *Auto* uses each example's own profile. Persisted in
  `QSettings`.
- **Show runtime path** (Build menu) echoes the runtime directory the build will
  use (`…/crates/esp32s3_rts/<profile>-esp32s3`) to the Output pane, flagging when
  that runtime isn't built yet — confirm a profile change without a full build.

### Debugging (GDB/MI)
- **Start debugging** launches the debug server (OpenOCD on :3333), runs gdb in MI
  mode, connects, and stops at `app_main`. Continue / Step over / Step into /
  Restart / Stop / Pause drive the session; the current line is marked and a Debug
  console shows gdb output and takes raw gdb commands.
- **Breakpoints**: click the left margin or Toggle breakpoint to set one; gdb snaps
  it to the nearest code line. A Breakpoints pane lists them (check to
  enable/disable, double-click to jump); they persist across sessions and re-arm on
  each Start.
- **Dual-core (SMP)**: launches OpenOCD with `ESP_RTOS=hwthread` so both LX7 cores
  appear as gdb threads. **Attach (post-mortem)** connects and `monitor halt`s in
  place (no reset) to inspect a hang.
- **Threads / Call stack / Variables & watch** panes track the stopped target;
  locals auto-refresh at every stop and watch expressions evaluate per stop.

## Keyboard shortcuts

Defaults follow modern conventions for editing and Borland Turbo C/C++ for
build/debug. **All are rebindable** in Settings → Keyboard shortcuts.

| | |
|---|---|
| File | Ctrl+N New · Ctrl+O Open · Ctrl+K Ctrl+O Open folder · Ctrl+S Save · Ctrl+Shift+S Save As · Ctrl+Q Exit |
| Edit | Ctrl+Z Undo · Ctrl+Y Redo · Ctrl+X/C/V Cut/Copy/Paste · Ctrl+Shift+F Format |
| Search | Ctrl+F Find · Ctrl+H Replace · F3 Find next · Ctrl+G Goto line · F12 Go to definition · Ctrl+Space Complete |
| View | Ctrl+Shift+1..8 toggle docks |
| Build | F9 Build · Alt+F9 Flash · Shift+F9 Run · Alt+F5 Monitor |
| Debug | F5 Start · Ctrl+F9 Continue · F8 Step over · F7 Step into · Ctrl+F6 Pause · Ctrl+Shift+F2 Restart · Ctrl+F2 Stop · Ctrl+F8 Toggle breakpoint · Ctrl+F7 Add watch |

## Semantic features (Ada Language Server)

Opening an Ada file starts `ada_language_server` (auto-located from PATH or the
AdaCore VS Code/Zed extension) pointed at the nearest `.gpr`. Right-click a symbol
(or **F12**) for **Go to definition**; right-click → **Quick info (hover)** for
type/doc info. **Format (Ada)** uses `textDocument/formatting` (whole file) or
`rangeFormatting` (selection). Communication is LSP/JSON-RPC over stdio; the
buffer is synced (didOpen/didChange) before each request.

**Diagnostics**: ALS errors/warnings appear as squiggles (red = error, orange =
warning) and in a **Problems pane** — click an entry to jump to it.

**Completion** (Ctrl+Space): identifier completion from ALS. The client declares
`completionItem.resolveSupport` so ALS defers documentation computation (eager doc
computation crashes ALS on this runtime's `abstract state` declarations).

Note: ALS indexes the project for a few seconds after a file opens — a request
made before indexing finishes returns "No definition found"; just retry.

## Build

```sh
cmake -S . -B build
cmake --build build -j
./build/adaedit some_file.adb
```

### Build dependencies
Only Qt5 + QScintilla — no other third-party libraries.

| Dependency | Tested |
|---|---|
| C++17 compiler (GCC/Clang/MSVC) | — |
| CMake ≥ 3.16 | 3.31 |
| Qt5 — Widgets + PrintSupport | 5.15 |
| QScintilla for Qt5 (dev) | 2.14 |

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake qtbase5-dev qtbase5-dev-tools libqscintilla2-qt5-dev
# Fedora
sudo dnf install gcc-c++ cmake qt5-qtbase-devel qscintilla-qt5-devel
# Arch
sudo pacman -S base-devel cmake qt5-base qscintilla-qt5
# macOS (Homebrew) — then:  cmake -S . -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qt@5)
brew install cmake qt@5 qscintilla2
```

CMake locates QScintilla via Qt's own `qmake` paths plus the common prefixes; if
it lives somewhere unusual, pass `-DQSCINTILLA_ROOT=<prefix>` (expects
`<prefix>/include/Qsci` and `<prefix>/lib`).

The app compiles cross-platform, but Build/Flash/Run/Monitor/Debug shell out via
`/bin/sh` and `bash` (the `./x` launcher), so full functionality assumes a POSIX
environment (Linux/macOS, or WSL/MSYS on Windows).

### Runtime dependencies (to actually build/flash/debug ESP32-S3 Ada)
Not needed to compile the editor — needed for its actions to work:
- `ada_language_server` on `PATH` (or the AdaCore VS Code/Zed extension) — for
  completion, hover, diagnostics, format.
- **Alire (`alr`)** + the **GNAT `xtensa-esp32-elf`** toolchain — for build/flash/run.
- The **`ada-bare-metal-esp32s3`** repo — provides `./x`, the runtime crate and the
  per-example build scripts the editor invokes.
- **OpenOCD** + the pinned xtensa esp **gdb** — for on-chip debugging (the repo's
  `tools/get-openocd.sh` / `get-gdb.sh` fetch them).
- Serial/USB access (esptool, `/dev/ttyACM*` permissions) — for flash + monitor.

## Layout

    CMakeLists.txt        build (Qt5 + QScintilla; portable QScintilla finder)
    src/main.cpp          entry point, CLI file args, session restore
    src/mainwindow.*      editor shell: tabs, menus, docks, settings, profile, actions
    src/adalexer.*        Ada highlighting via native Scintilla SCLEX_ADA
    src/project.*         single ESP32-S3 command profile + token expansion
    src/debugger.*        GDB/MI debug engine (server + gdb, step/continue/marker)
    src/lspclient.*       Ada Language Server client (definition, hover, completion, format)
    docs/ada_keywords.txt        extracted Ada keyword set + lexing rules
    docs/setedit_keybindings.txt extracted SETEdit menu + keybinding inventory

## ESP32-S3 workflow

Open an example folder under the `ada-bare-metal-esp32s3` repo (e.g.
`examples/esp32s3_gpio0_blink`), pick a **Profile** in the toolbar (or leave
*Auto*), then **Build / Flash / Run / Monitor** — each runs `./x` at the project
root with output in the Output dock. **Show runtime path** confirms which runtime
a build will link. **Start debugging** boots OpenOCD + gdb, halts at `app_main`,
and the step controls take over.

The default action commands are:

    build:   bash {repo}/x build {example} --profile {profile}
    flash:   bash {repo}/x build {example} --profile {profile} && bash {repo}/x flash {example}
    run:     bash {repo}/x run {example} --profile {profile}
    monitor: bash {repo}/x monitor

where `{profile}` is the toolbar selection (`auto` lets `./x` use the example's
own). The profile maps to `ESP32S3_RTS_PROFILE` and selects the runtime
(`crates/esp32s3_rts/<profile>-esp32s3`) — a profile-respecting example builds a
different `.elf` per profile; some examples deliberately pin their profile in
their own `build.sh`, where *Auto* is the right choice.
