# AdaEdit

A native Qt5 editor for writing Ada and targeting different processors.
A clean-room successor to SETEdit (Turbo Vision, text-mode) rebuilt on Qt +
QScintilla, reusing SETEdit's Ada highlighting rules and keybinding UX.

## Status: scaffold (v0.1)

Working now:
- Tabbed Ada editor (QScintilla) with the `AdaLexer` (Ada 95 keywords from
  SETEdit + Ada 2005/2022 additions), line numbers, folding, auto-indent.
- File: New / Open / Save / Save As (with unsaved-changes guard), open files
  from the command line.
- Edit: Undo / Redo / Cut / Copy / Paste.
- Search: Find / Replace / Search again / Go to line.
- **Open a folder as a project**: File → Open folder (or `adaedit <dir>`) roots
  the left-hand explorer tree at the folder; double-click any file to open it.
  If the folder has a `.adaproj`, it loads automatically.
- Per-target **project model**: a project holds named target profiles, each with
  configurable **build / flash / run / monitor** commands plus an on-chip
  **debug** config. A toolbar selector switches the active target; Manage
  Targets… edits every field; projects persist to `.adaproj` JSON. Commands run
  at the project root and expand `{root}`, `{file}`, `{dir}`, `{base}`, `{exe}`.
  Defaults are seeded for the `./x` ESP32-S3 launcher.
- **In-editor debugger** (GDB/MI): Start debugging launches the target's debug
  server (OpenOCD on :3333), then runs gdb in MI mode, connects, and stops at
  the init breakpoint (`app_main`). Continue / Step over / Step into / Restart /
  Stop drive the session; the current line is marked in the editor and a Debug
  console shows gdb output and takes raw gdb commands.
- **Breakpoints**: click the left margin or right-click a line to toggle a source
  breakpoint (red circle). gdb snaps it to the nearest code line (and rejects
  dead lines). A **Breakpoints pane** lists them all — check/uncheck to
  enable/disable (disabled = grey circle), double-click to jump to the line.
  They persist across sessions and are re-armed on each Start; add/remove/enable
  during a live session is applied immediately via MI.
- **Dual-core (SMP)**: Debug → Dual-core (SMP) toggle (also per-target in Manage
  Targets) launches OpenOCD with `ESP_RTOS=hwthread` so both LX7 cores appear as
  gdb threads. Applies on the next Start.
- **Attach (post-mortem)**: Debug → Attach connects and `monitor halt`s the
  running target in place — no reset, no run-to-`app_main` — then shows where
  each core/thread is. Pair with Dual-core (SMP) to inspect a hang (one core
  crashed, the other idles). This is the SMP-friendly flow; normal Start uses
  reset + run-to-`app_main` and is best with SMP off.
- **Threads pane**: lists the stopped target's gdb threads (both cores in SMP)
  with each one's function + file:line; the current thread is marked. Click a
  thread to switch context — the editor jumps to its frame and locals/watches
  refresh for it.
- **Call stack pane**: the current thread's frames (`#level func (file:line)`);
  click a frame to select it — the editor jumps there and locals/watches
  re-evaluate in that frame's context.
- **Pause** (F6): interrupt a running target (SIGINT to gdb, since this ESP
  target runs gdb in sync mode); enabled only while running.
- **Variables / watch pane**: Locals auto-refresh at every stop
  (`-stack-list-variables --all-values`). Add watch expressions in the input box
  (evaluated via `-data-evaluate-expression` each stop); right-click a watch to
  remove it. Failed watches show the gdb error in red.

Shortcuts: F2 Save, F3 Open file, Ctrl+K Ctrl+O Open folder, Alt+X Exit,
Ctrl+Q,F Find, Ctrl+Q,A Replace, Ctrl+L Search again, Ctrl+J Goto line,
Ctrl+F9 Build, Ctrl+Shift+F9 Flash, F9 Run, Ctrl+M Monitor,
F5 Start debug, Ctrl+F5 Continue, F10 Step over, F11 Step into,
Ctrl+Shift+F5 Restart, Shift+F5 Stop.

## Semantic features (Ada Language Server)

Opening an Ada file starts `ada_language_server` (auto-located from PATH or the
AdaCore VS Code extension) pointed at the nearest `.gpr`. Right-click a symbol
(or **F12**) for **Go to definition**; right-click → **Quick info (hover)** for
type/doc info. Communication is LSP/JSON-RPC over stdio; the editor syncs buffer
contents (didOpen/didChange) before each request.

**Diagnostics**: ALS errors/warnings appear as squiggles (red = error, orange =
warning) and in a **Problems pane** (severity, file:line, message) — click an
entry to jump to it. Updated live as you edit (the buffer is synced to ALS).

**Completion** (Ctrl+Space): identifier completion from ALS, shown in the
editor's autocomplete list (case-insensitive, Ada-style). The client declares
`completionItem.resolveSupport` so ALS defers documentation computation — eager
doc computation crashes ALS on this runtime's `abstract state` declarations.

Note: ALS indexes the project for a few seconds after a file opens — a request
made before indexing finishes returns "No definition found"; just retry.

Planned next: find-references and an outline pane.

Not yet implemented (later phases): rectangle/column selection, macros,
the Tool&Ops palette.

## Build

    sudo apt install libqscintilla2-qt5-dev    # one-time dependency
    cmake -S . -B build
    cmake --build build -j
    ./build/adaedit some_file.adb

Requires Qt5 (Widgets, PrintSupport) and QScintilla2-Qt5.

## Layout

    CMakeLists.txt        build (Qt5 + QScintilla, auto-locates QScintilla)
    src/main.cpp          entry point, CLI file args
    src/mainwindow.*      editor shell: tabs, menus, file/edit/search/build
    src/adalexer.*        Ada highlighting via native Scintilla SCLEX_ADA
    src/project.*         project model: TargetProfile + Project (JSON .adaproj)
    src/targetdialog.*    Manage Targets master-detail editor dialog
    src/debugger.*        GDB/MI debug engine (server + gdb, step/continue/marker)
    src/lspclient.*       Ada Language Server client (definition, hover)
    docs/ada_keywords.txt        extracted Ada keyword set + lexing rules
    docs/setedit_keybindings.txt extracted SETEdit menu + keybinding inventory

## Cross-target workflow

A new project seeds an ESP32-S3 target (driving the `./x` launcher) and a Host
GNAT target. Project → Manage Targets… edits every field. Example ESP32-S3:

    build:   ./x build           flash:   ./x flash
    run:     ./x run             monitor: ./x monitor
    debug server: bash tools/openocd.sh   gdb: xtensa-esp32s3-elf-gdb
    gdb program:  {dir}/app.elf  remote: localhost:3333  init bp: app_main

Open the project folder, pick the active target in the toolbar, then Build /
Flash / Run / Monitor — each runs at the project root with output in the Output
dock. Start debugging boots OpenOCD + gdb, halts at `app_main`, and the step
buttons take over. Save the target set with Project → Save project (writes
`.adaproj` into the folder).

Debug settings are project-structure-specific; adjust the gdb program / server
command in Manage Targets to match your repo's elf path and OpenOCD launch.
