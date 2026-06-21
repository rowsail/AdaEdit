#include "mainwindow.h"
#include "adalexer.h"
#include "debugger.h"
#include "lspclient.h"

#include <Qsci/qsciscintilla.h>

#include <QtWidgets>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace {
constexpr int MARK_DEBUG = 1;        // current execution line (arrow)
constexpr int MARK_BREAKPOINT = 2;   // enabled breakpoint (red circle)
constexpr int MARK_BP_DISABLED = 3;  // disabled breakpoint (grey circle)
constexpr int BP_MARGIN = 1;         // clickable symbol margin
constexpr int INDIC_ERROR = 8;       // diagnostic squiggle (error)
constexpr int INDIC_WARNING = 9;     // diagnostic squiggle (warning/info)

// Walk up from a file/dir to the project root. The ./x launcher is the
// definitive marker and always wins (it must run from the repo root, not the
// example dir). Failing that, fall back to the nearest .adaproj / .git. We do
// NOT key on alire.toml: it appears per-example and would stop the climb short.
QString findProjectRoot(const QString &startPath)
{
    QFileInfo fi(startPath);
    QDir d(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
    QString fallback;
    forever {
        // A self-contained project folder (its own build.sh / .gpr / .adaproj)
        // is the nearest "project we are in".
        if (QFileInfo(d.filePath("build.sh")).isFile()
            || QFileInfo::exists(d.filePath(".adaproj"))
            || !d.entryList({"*.gpr"}, QDir::Files).isEmpty())
            return d.absolutePath();
        if (fallback.isEmpty() && d.exists(".git"))
            fallback = d.absolutePath();
        if (!d.cdUp()) break;
    }
    if (!fallback.isEmpty()) return fallback;
    return fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
}

// Walk up to the repo root: the ancestor holding the ./x launcher or tools/
// (where OpenOCD etc. live), which may sit above the opened project folder.
QString findRepoRoot(const QString &startPath)
{
    QFileInfo fi(startPath);
    QDir d(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
    QString fallback;
    forever {
        if (QFileInfo(d.filePath("x")).isFile()
            || QFileInfo(d.filePath("tools/openocd.sh")).isFile())
            return d.absolutePath();
        if (fallback.isEmpty() && d.exists(".git"))
            fallback = d.absolutePath();
        if (!d.cdUp()) break;
    }
    return !fallback.isEmpty() ? fallback
                              : (fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
}

bool isAdaFile(const QString &path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    return s == "adb" || s == "ads" || s == "ada";
}

// Locate ada_language_server: PATH, then the AdaCore VS Code / Zed extensions.
QString findAlsServer()
{
    if (const QString p = QStandardPaths::findExecutable("ada_language_server"); !p.isEmpty())
        return p;
    const QStringList extRoots = {
        QDir::homePath() + "/.vscode/extensions",
        QDir::homePath() + "/.vscode-server/extensions",
    };
    for (const QString &root : extRoots) {
        QDir d(root);
        const QStringList dirs = d.entryList({"adacore.ada-*"}, QDir::Dirs, QDir::Name | QDir::Reversed);
        for (const QString &e : dirs) {
            const QString cand = d.filePath(e) + "/x64/linux/ada_language_server";
            if (QFileInfo(cand).isExecutable()) return cand;
        }
    }
    return {};
}

// Nearest .gpr at or above a file (the ALS project).
QString findGpr(const QString &start)
{
    QFileInfo fi(start);
    QDir d(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
    forever {
        const QStringList g = d.entryList({"*.gpr"}, QDir::Files);
        if (!g.isEmpty()) return d.filePath(g.first());
        if (!d.cdUp()) break;
    }
    return {};
}

// The examples/<name> segment of a path, relative to the project root.
QString exampleOf(const QString &root, const QString &file)
{
    if (root.isEmpty() || file.isEmpty()) return {};
    const QString rel = QDir(root).relativeFilePath(file);
    const QStringList parts = rel.split('/', Qt::SkipEmptyParts);
    const int i = parts.indexOf("examples");
    return (i >= 0 && i + 1 < parts.size()) ? parts[i + 1] : QString();
}
} // namespace

// ---- construction --------------------------------------------------------

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    // Remember the default look so light mode restores exactly, then load the
    // persisted preferences (applied near the end, once the UI exists).
    m_defaultStyle = qApp->style()->objectName();
    m_lightPalette = qApp->palette();
    m_darkMode = QSettings().value("ui/darkMode", false).toBool();

    QSettings s;
    m_defaultInterfaceFont = qApp->font();
    m_defaultEditorFont = QFont("monospace", 11);   // monospaced editor/dock default
    m_interfaceFont = m_defaultInterfaceFont;
    if (const QString f = s.value("ui/interfaceFont").toString(); !f.isEmpty())
        m_interfaceFont.fromString(f);
    m_editorFont = m_defaultEditorFont;
    if (const QString f = s.value("ui/editorFont").toString(); !f.isEmpty())
        m_editorFont.fromString(f);

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::updateTitle);
    setCentralWidget(m_tabs);

    m_project = Project::makeDefault();
    m_debugger = new Debugger(this);
    connect(m_debugger, &Debugger::output, this, &MainWindow::onDebugOutput);
    connect(m_debugger, &Debugger::stateChanged, this, &MainWindow::onDebugStateChanged);
    connect(m_debugger, &Debugger::stoppedAt, this, &MainWindow::onDebugStopped);
    connect(m_debugger, &Debugger::breakpointResolved, this, &MainWindow::onBreakpointResolved);
    connect(m_debugger, &Debugger::breakpointRejected, this, &MainWindow::onBreakpointRejected);
    connect(m_debugger, &Debugger::breakpointsChanged, this, &MainWindow::onBreakpointsChanged);
    connect(m_debugger, &Debugger::localsUpdated, this, &MainWindow::onLocalsUpdated);
    connect(m_debugger, &Debugger::watchEvaluated, this, &MainWindow::onWatchEvaluated);
    connect(m_debugger, &Debugger::threadsUpdated, this, &MainWindow::onThreadsUpdated);
    connect(m_debugger, &Debugger::stackUpdated, this, &MainWindow::onStackUpdated);

    createDocks();
    createMenus();
    createActionBar();
    createDebugBar();
    updateDebugActions();

    resize(1200, 800);
    newFile();
    applyTheme();                        // honour the saved dark-mode preference
    applyFonts();                        // honour the saved font preferences
    statusBar()->showMessage(tr("Ready"));
}

// ---- docks: explorer, build output, debug console ------------------------

void MainWindow::createDocks()
{
    // Project explorer (folder tree).
    m_fsModel = new QFileSystemModel(this);
    m_tree = new QTreeView(this);
    m_tree->setModel(m_fsModel);
    for (int c = 1; c < m_fsModel->columnCount(); ++c)
        m_tree->hideColumn(c);
    m_tree->setHeaderHidden(true);
    connect(m_tree, &QTreeView::activated, this, &MainWindow::onTreeActivated);
    auto *explorer = new QDockWidget(tr("Project"), this);
    explorer->setWidget(m_tree);
    addDockWidget(Qt::LeftDockWidgetArea, explorer);

    // Build / action output.
    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(5000);
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter);
    m_output->setFont(mono);
    auto *outDock = new QDockWidget(tr("Output"), this);
    outDock->setWidget(m_output);
    addDockWidget(Qt::BottomDockWidgetArea, outDock);

    // Debug console (read-only log + raw gdb input line).
    auto *dbgWidget = new QWidget(this);
    auto *dbgLayout = new QVBoxLayout(dbgWidget);
    dbgLayout->setContentsMargins(0, 0, 0, 0);
    m_debugConsole = new QPlainTextEdit(dbgWidget);
    m_debugConsole->setReadOnly(true);
    m_debugConsole->setMaximumBlockCount(8000);
    m_debugConsole->setFont(mono);
    m_debugInput = new QLineEdit(dbgWidget);
    m_debugInput->setPlaceholderText(tr("gdb command (e.g. bt, info threads)…"));
    connect(m_debugInput, &QLineEdit::returnPressed, this, &MainWindow::onDebugConsoleReturn);
    dbgLayout->addWidget(m_debugConsole);
    dbgLayout->addWidget(m_debugInput);
    auto *dbgDock = new QDockWidget(tr("Debug console"), this);
    dbgDock->setWidget(dbgWidget);
    addDockWidget(Qt::BottomDockWidgetArea, dbgDock);

    // Breakpoints list: check to enable/disable, double-click to navigate.
    m_bpList = new QListWidget(this);
    connect(m_bpList, &QListWidget::itemChanged, this, &MainWindow::onBreakpointItemChanged);
    connect(m_bpList, &QListWidget::itemActivated, this, &MainWindow::onBreakpointActivated);
    connect(m_bpList, &QListWidget::itemDoubleClicked, this, &MainWindow::onBreakpointActivated);
    auto *bpDock = new QDockWidget(tr("Breakpoints"), this);
    bpDock->setWidget(m_bpList);
    addDockWidget(Qt::LeftDockWidgetArea, bpDock);

    // Variables / watch: locals auto-refresh on each stop; watches are user
    // expressions evaluated each stop.
    auto *varsWidget = new QWidget(this);
    auto *varsLayout = new QVBoxLayout(varsWidget);
    varsLayout->setContentsMargins(0, 0, 0, 0);
    m_varsTree = new QTreeWidget(varsWidget);
    m_varsTree->setColumnCount(2);
    m_varsTree->setHeaderLabels({tr("Name"), tr("Value")});
    m_varsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_varsTree, &QWidget::customContextMenuRequested,
            this, &MainWindow::onVarsContextMenu);
    m_localsRoot = new QTreeWidgetItem(m_varsTree, {tr("Locals")});
    m_watchRoot  = new QTreeWidgetItem(m_varsTree, {tr("Watch")});
    m_localsRoot->setExpanded(true);
    m_watchRoot->setExpanded(true);
    m_watchInput = new QLineEdit(varsWidget);
    m_watchInput->setPlaceholderText(tr("add watch expression…"));
    connect(m_watchInput, &QLineEdit::returnPressed, this, &MainWindow::onAddWatch);
    varsLayout->addWidget(m_varsTree);
    varsLayout->addWidget(m_watchInput);
    auto *varsDock = new QDockWidget(tr("Variables"), this);
    varsDock->setWidget(varsWidget);
    addDockWidget(Qt::LeftDockWidgetArea, varsDock);

    // Threads: gdb threads (both cores in SMP mode); click to switch.
    m_threadsList = new QListWidget(this);
    connect(m_threadsList, &QListWidget::itemActivated, this, &MainWindow::onThreadActivated);
    connect(m_threadsList, &QListWidget::itemDoubleClicked, this, &MainWindow::onThreadActivated);
    auto *thrDock = new QDockWidget(tr("Threads"), this);
    thrDock->setWidget(m_threadsList);
    addDockWidget(Qt::LeftDockWidgetArea, thrDock);

    // Call stack: frames of the current thread; click to select a frame.
    m_stackList = new QListWidget(this);
    connect(m_stackList, &QListWidget::itemActivated, this, &MainWindow::onStackActivated);
    connect(m_stackList, &QListWidget::itemDoubleClicked, this, &MainWindow::onStackActivated);
    auto *stackDock = new QDockWidget(tr("Call stack"), this);
    stackDock->setWidget(m_stackList);
    addDockWidget(Qt::LeftDockWidgetArea, stackDock);

    // Problems: ALS diagnostics across files; click to navigate.
    m_problemsList = new QListWidget(this);
    connect(m_problemsList, &QListWidget::itemActivated, this, &MainWindow::onProblemActivated);
    connect(m_problemsList, &QListWidget::itemDoubleClicked, this, &MainWindow::onProblemActivated);
    auto *probDock = new QDockWidget(tr("Problems"), this);
    probDock->setWidget(m_problemsList);
    addDockWidget(Qt::BottomDockWidgetArea, probDock);

    // Ordered list backing the View menu's show/hide toggles.
    m_docks = {explorer, outDock, dbgDock, bpDock, varsDock, thrDock, stackDock, probDock};
}

// ---- editor factory ------------------------------------------------------

QsciScintilla *MainWindow::newEditor()
{
    auto *e = new QsciScintilla(this);
    applyEditorTheme(e);                 // lexer + light/dark colours
    e->setUtf8(true);
    e->setTabWidth(3);
    e->setIndentationsUseTabs(false);
    e->setAutoIndent(true);
    e->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    e->setCaretLineVisible(true);
    e->setMarginType(0, QsciScintilla::NumberMargin);
    e->setMarginWidth(0, "00000");
    e->setMarginLineNumbers(0, true);
    e->setFolding(QsciScintilla::BoxedTreeFoldStyle);

    // Clickable breakpoint / current-line margin.
    e->setMarginType(BP_MARGIN, QsciScintilla::SymbolMargin);
    e->setMarginWidth(BP_MARGIN, 18);
    e->setMarginSensitivity(BP_MARGIN, true);
    e->setMarginMarkerMask(BP_MARGIN,
        (1 << MARK_DEBUG) | (1 << MARK_BREAKPOINT) | (1 << MARK_BP_DISABLED));
    e->markerDefine(QsciScintilla::RightArrow, MARK_DEBUG);
    e->setMarkerBackgroundColor(QColor("#ffd54f"), MARK_DEBUG);
    e->markerDefine(QsciScintilla::Circle, MARK_BREAKPOINT);
    e->setMarkerForegroundColor(QColor("#7a0000"), MARK_BREAKPOINT);
    e->setMarkerBackgroundColor(QColor("#e53935"), MARK_BREAKPOINT);
    e->markerDefine(QsciScintilla::Circle, MARK_BP_DISABLED);
    e->setMarkerForegroundColor(QColor("#9e9e9e"), MARK_BP_DISABLED);
    e->setMarkerBackgroundColor(QColor("#cfcfcf"), MARK_BP_DISABLED);
    // Diagnostic squiggles (errors red, warnings orange).
    e->indicatorDefine(QsciScintilla::SquiggleIndicator, INDIC_ERROR);
    e->setIndicatorForegroundColor(QColor("#d32f2f"), INDIC_ERROR);
    e->indicatorDefine(QsciScintilla::SquiggleIndicator, INDIC_WARNING);
    e->setIndicatorForegroundColor(QColor("#f57c00"), INDIC_WARNING);

    connect(e, &QsciScintilla::marginClicked, this, &MainWindow::onMarginClicked);
    e->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(e, &QWidget::customContextMenuRequested, this, &MainWindow::onEditorContextMenu);
    connect(e, &QsciScintilla::modificationChanged, this, &MainWindow::updateTitle);
    return e;
}

QsciScintilla *MainWindow::currentEditor() const
{
    return qobject_cast<QsciScintilla *>(m_tabs->currentWidget());
}

void MainWindow::setEditorPath(QsciScintilla *e, const QString &path)
{
    e->setProperty("filePath", QFileInfo(path).absoluteFilePath());
}

QString MainWindow::editorPath(QsciScintilla *e) const
{
    return e ? e->property("filePath").toString() : QString();
}

QsciScintilla *MainWindow::editorForPath(const QString &path) const
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *e = qobject_cast<QsciScintilla *>(m_tabs->widget(i));
        if (e && editorPath(e) == abs) return e;
    }
    return nullptr;
}

QsciScintilla *MainWindow::openOrActivate(const QString &path)
{
    if (auto *e = editorForPath(path)) { m_tabs->setCurrentWidget(e); return e; }
    openPath(QFileInfo(path).absoluteFilePath());
    return currentEditor();
}

// ---- menus / toolbars ----------------------------------------------------

void MainWindow::createMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New"), this, &MainWindow::newFile, QKeySequence::New);
    file->addAction(tr("&Open file..."), this, &MainWindow::openFile, QKeySequence(Qt::Key_F3));
    file->addAction(tr("Open &folder..."), this, &MainWindow::openFolder,
                    QKeySequence(Qt::CTRL | Qt::Key_K, Qt::CTRL | Qt::Key_O));
    file->addAction(tr("&Save"), this, &MainWindow::saveFile, QKeySequence(Qt::Key_F2));
    file->addAction(tr("Save &as..."), this, &MainWindow::saveFileAs, QKeySequence::SaveAs);
    file->addSeparator();
    file->addAction(tr("Se&ttings..."), this, &MainWindow::openSettings);
    file->addSeparator();
    file->addAction(tr("E&xit"), this, &QWidget::close, QKeySequence(Qt::ALT | Qt::Key_X));

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("&Undo"), this, &MainWindow::undo, QKeySequence::Undo);
    edit->addAction(tr("&Redo"), this, &MainWindow::redo, QKeySequence::Redo);
    edit->addSeparator();
    edit->addAction(tr("Cu&t"), this, &MainWindow::cut, QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    edit->addAction(tr("&Copy"), this, &MainWindow::copy, QKeySequence(Qt::CTRL | Qt::Key_Insert));
    edit->addAction(tr("&Paste"), this, &MainWindow::paste, QKeySequence(Qt::SHIFT | Qt::Key_Insert));

    // View: one checkable entry per dock. QDockWidget::toggleViewAction() is
    // already checkable, shows/hides the dock, and unticks itself when the dock
    // is closed via its title-bar X — so re-ticking it brings the dock back.
    QMenu *view = menuBar()->addMenu(tr("&View"));
    for (QDockWidget *dock : m_docks)
        view->addAction(dock->toggleViewAction());

    QMenu *search = menuBar()->addMenu(tr("&Search"));
    search->addAction(tr("&Find..."), this, &MainWindow::find,
                      QKeySequence(Qt::CTRL | Qt::Key_Q, Qt::Key_F));
    search->addAction(tr("&Replace..."), this, &MainWindow::replace,
                      QKeySequence(Qt::CTRL | Qt::Key_Q, Qt::Key_A));
    search->addAction(tr("Search &again"), this, &MainWindow::searchAgain,
                      QKeySequence(Qt::CTRL | Qt::Key_L));
    search->addAction(tr("&Go to line..."), this, &MainWindow::gotoLine,
                      QKeySequence(Qt::CTRL | Qt::Key_J));
    search->addSeparator();
    search->addAction(tr("Go to &definition"), this, &MainWindow::gotoDefinitionAtCursor,
                      QKeySequence(Qt::Key_F12));
    search->addAction(tr("&Complete"), this, &MainWindow::triggerCompletion,
                      QKeySequence(Qt::CTRL | Qt::Key_Space));

    QMenu *project = menuBar()->addMenu(tr("&Project"));
    project->addAction(tr("&New project"), this, &MainWindow::newProject);
    project->addAction(tr("&Open project..."), this, &MainWindow::openProject);
    project->addAction(tr("&Save project"), this, &MainWindow::saveProject);
    project->addAction(tr("Save project &as..."), this, &MainWindow::saveProjectAs);

    QMenu *build = menuBar()->addMenu(tr("&Build"));
    build->addAction(tr("&Build"), this, &MainWindow::doBuild, QKeySequence(Qt::CTRL | Qt::Key_F9));
    build->addAction(tr("&Flash"), this, &MainWindow::doFlash, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F9));
    build->addAction(tr("&Run"), this, &MainWindow::doRun, QKeySequence(Qt::Key_F9));
    build->addAction(tr("&Monitor"), this, &MainWindow::doMonitor, QKeySequence(Qt::CTRL | Qt::Key_M));

    QMenu *debug = menuBar()->addMenu(tr("&Debug"));
    m_actStart    = debug->addAction(tr("Start &debugging"), this, &MainWindow::debugStart,
                                     QKeySequence(Qt::Key_F5));
    m_actAttach   = debug->addAction(tr("&Attach (post-mortem)"), this, &MainWindow::debugAttach);
    m_actAttach->setToolTip(tr("Connect and halt in place (no reset / no run-to-app_main)"));
    m_actContinue = debug->addAction(tr("&Continue"), this, &MainWindow::debugContinue,
                                     QKeySequence(Qt::CTRL | Qt::Key_F5));
    m_actStepOver = debug->addAction(tr("Step &over"), this, &MainWindow::debugStepOver,
                                     QKeySequence(Qt::Key_F10));
    m_actStepInto = debug->addAction(tr("Step &into"), this, &MainWindow::debugStepInto,
                                     QKeySequence(Qt::Key_F11));
    m_actPause    = debug->addAction(tr("&Pause"), this, &MainWindow::debugPause,
                                     QKeySequence(Qt::Key_F6));
    m_actRestart  = debug->addAction(tr("&Restart"), this, &MainWindow::debugRestart,
                                     QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F5));
    m_actStop     = debug->addAction(tr("S&top debugging"), this, &MainWindow::debugStop,
                                     QKeySequence(Qt::SHIFT | Qt::Key_F5));
    debug->addSeparator();
    m_actSmp = debug->addAction(tr("&Dual-core (SMP)"));
    m_actSmp->setCheckable(true);
    m_actSmp->setToolTip(tr("Debug both LX7 cores as gdb threads (applies on next Start)"));
    connect(m_actSmp, &QAction::toggled, this, &MainWindow::onSmpToggled);
}

// ---- Settings / appearance ----------------------------------------------

void MainWindow::openSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Settings"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *dark = new QCheckBox(tr("Dark mode"), &dlg);
    dark->setChecked(m_darkMode);
    layout->addWidget(dark);

    // Working copies edited by the font choosers; committed only on OK.
    QFont interfaceFont = m_interfaceFont;
    QFont editorFont = m_editorFont;

    auto describe = [](const QFont &f) {
        return QStringLiteral("%1 %2").arg(f.family()).arg(f.pointSize());
    };

    // A label + "Change..."/"Reset" buttons bound to a font; updates the working
    // copy. "Reset" restores the startup default captured for that font.
    auto addFontRow = [&](const QString &caption, QFont *target,
                          const QFont &defaultFont,
                          QFontDialog::FontDialogOptions opts) {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(caption, &dlg));
        auto *value = new QLabel(describe(*target), &dlg);
        value->setStyleSheet("font-weight:bold;");
        row->addWidget(value, 1);
        auto *btn = new QPushButton(tr("Change..."), &dlg);
        row->addWidget(btn);
        auto *reset = new QPushButton(tr("Reset"), &dlg);
        row->addWidget(reset);
        layout->addLayout(row);
        connect(btn, &QPushButton::clicked, &dlg, [&dlg, target, value, describe, opts] {
            bool ok = false;
            QFont f = QFontDialog::getFont(&ok, *target, &dlg, QString(), opts);
            if (!ok) return;
            // Qt >= 5.13 QFontDialog returns the chosen face via setFamilies();
            // family()/toString() then report the *default* family, so Scintilla
            // would keep its old face (only the size would change). Copy the real
            // family back into the legacy slot so family()/toString() are correct.
            if (!f.families().isEmpty()) f.setFamily(f.families().constFirst());
            *target = f;
            value->setText(describe(f));
        });
        connect(reset, &QPushButton::clicked, &dlg, [target, value, describe, defaultFont] {
            *target = defaultFont;
            value->setText(describe(defaultFont));
        });
    };

    addFontRow(tr("Interface font (menus, titles):"), &interfaceFont,
               m_defaultInterfaceFont, QFontDialog::FontDialogOptions());
    addFontRow(tr("Editor && dock font:"), &editorFont,
               m_defaultEditorFont, QFontDialog::MonospacedFonts);   // fixed-width

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QSettings s;
    if (dark->isChecked() != m_darkMode) {
        m_darkMode = dark->isChecked();
        s.setValue("ui/darkMode", m_darkMode);
        applyTheme();
    }
    if (interfaceFont != m_interfaceFont || editorFont != m_editorFont) {
        m_interfaceFont = interfaceFont;
        m_editorFont = editorFont;
        s.setValue("ui/interfaceFont", m_interfaceFont.toString());
        s.setValue("ui/editorFont", m_editorFont.toString());
        applyFonts();
    }
}

// Apply the application-wide palette and re-theme every open editor. The Qt
// chrome (menus, docks, dialogs, tree, output panes) follows QPalette; the
// QScintilla editors do not, so they are recoloured explicitly.
void MainWindow::applyTheme()
{
    if (m_darkMode) {
        qApp->setStyle(QStyleFactory::create("Fusion"));
        QPalette p;
        p.setColor(QPalette::Window,          QColor(45, 45, 45));
        p.setColor(QPalette::WindowText,      QColor(220, 220, 220));
        p.setColor(QPalette::Base,            QColor(30, 30, 30));
        p.setColor(QPalette::AlternateBase,   QColor(45, 45, 45));
        p.setColor(QPalette::ToolTipBase,     QColor(45, 45, 45));
        p.setColor(QPalette::ToolTipText,     QColor(220, 220, 220));
        p.setColor(QPalette::Text,            QColor(220, 220, 220));
        p.setColor(QPalette::Button,          QColor(53, 53, 53));
        p.setColor(QPalette::ButtonText,      QColor(220, 220, 220));
        p.setColor(QPalette::BrightText,      Qt::red);
        p.setColor(QPalette::Link,            QColor(42, 130, 218));
        p.setColor(QPalette::Highlight,       QColor(38, 79, 120));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Disabled, QPalette::Text,       QColor(127, 127, 127));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
        qApp->setPalette(p);
    } else {
        qApp->setStyle(QStyleFactory::create(m_defaultStyle));
        qApp->setPalette(m_lightPalette);
    }

    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *e = qobject_cast<QsciScintilla *>(m_tabs->widget(i)))
            applyEditorTheme(e);
}

// Editor colours (Scintilla manages its own; it does not follow QPalette).
void MainWindow::applyEditorTheme(QsciScintilla *e)
{
    applyAdaLexer(e, m_editorFont, m_darkMode);
    if (m_darkMode) {
        e->setCaretLineBackgroundColor(QColor("#2a2d2e"));
        e->setCaretForegroundColor(QColor("#d4d4d4"));
        e->setSelectionBackgroundColor(QColor("#264f78"));
        e->setSelectionForegroundColor(QColor("#ffffff"));
        e->setMarginsBackgroundColor(QColor("#252526"));
        e->setMarginsForegroundColor(QColor("#858585"));
        e->setFoldMarginColors(QColor("#3c3c3c"), QColor("#252526"));
        e->setMatchedBraceForegroundColor(QColor("#ffd700"));
        e->setMatchedBraceBackgroundColor(QColor("#264f78"));
    } else {
        e->setCaretLineBackgroundColor(QColor("#eef6ff"));
        e->setCaretForegroundColor(QColor("#000000"));
        e->setSelectionBackgroundColor(QColor("#cce8ff"));
        e->setSelectionForegroundColor(QColor("#000000"));
        e->setMarginsBackgroundColor(QColor("#f0f0f0"));
        e->setMarginsForegroundColor(QColor("#808080"));
        e->setFoldMarginColors(QColor("#e0e0e0"), QColor("#f0f0f0"));
        e->setMatchedBraceForegroundColor(QColor("#0000ff"));
        e->setMatchedBraceBackgroundColor(QColor("#cce8ff"));
    }
}

// The interface font becomes the application default (menus, titles, dialogs,
// tab bar, dock titles); the editor font is then forced onto the editors and
// the dock *content* widgets so they keep their own (monospaced) face.
void MainWindow::applyFonts()
{
    qApp->setFont(m_interfaceFont);

    auto setFontIf = [this](QWidget *w) { if (w) w->setFont(m_editorFont); };
    setFontIf(m_output);
    setFontIf(m_debugConsole);
    setFontIf(m_debugInput);
    setFontIf(m_tree);
    setFontIf(m_bpList);
    setFontIf(m_varsTree);
    setFontIf(m_watchInput);
    setFontIf(m_threadsList);
    setFontIf(m_stackList);
    setFontIf(m_problemsList);

    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *e = qobject_cast<QsciScintilla *>(m_tabs->widget(i)))
            applyEditorTheme(e);     // re-applies the lexer with m_editorFont
}

void MainWindow::createActionBar()
{
    // ESP32-S3 build actions. (This editor targets the ESP32-S3 only, so there
    // is no target picker — the commands come from the single fixed profile.)
    auto *bar = addToolBar(tr("Actions"));
    bar->setMovable(false);
    bar->addAction(tr("Build"), this, &MainWindow::doBuild);
    bar->addAction(tr("Flash"), this, &MainWindow::doFlash);
    bar->addAction(tr("Run"), this, &MainWindow::doRun);
    bar->addAction(tr("Monitor"), this, &MainWindow::doMonitor);
}

void MainWindow::createDebugBar()
{
    auto *bar = addToolBar(tr("Debug"));
    bar->setMovable(false);
    for (QAction *a : {m_actStart, m_actContinue, m_actStepOver,
                       m_actStepInto, m_actPause, m_actRestart, m_actStop})
        bar->addAction(a);
    bar->addSeparator();
    bar->addAction(m_actSmp);
}

// ---- File / folder -------------------------------------------------------

void MainWindow::newFile()
{
    QsciScintilla *e = newEditor();
    int idx = m_tabs->addTab(e, tr("untitled"));
    m_tabs->setCurrentIndex(idx);
    e->setFocus();
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open"), m_project.rootPath,
        tr("Ada sources (*.adb *.ads *.ada);;All files (*)"));
    if (!path.isEmpty())
        openOrActivate(path);
}

void MainWindow::openPath(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Cannot read %1:\n%2").arg(path, f.errorString()));
        return;
    }
    QsciScintilla *e = newEditor();
    e->setText(QString::fromUtf8(f.readAll()));
    e->setModified(false);
    setEditorPath(e, path);
    applyBreakpointMarkers(e);          // restore markers for this file
    if (isAdaFile(path)) {
        ensureLsp(path);
        if (m_lsp) m_lsp->didOpen(path, e->text());
        applyDiagnostics(e);           // re-show any diagnostics already known
    }
    int idx = m_tabs->addTab(e, QFileInfo(path).fileName());
    m_tabs->setCurrentIndex(idx);
    e->setFocus();
}

void MainWindow::openFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open folder"), m_project.rootPath);
    if (!dir.isEmpty())
        openFolderPath(dir);
}

void MainWindow::openFolderPath(const QString &path)
{
    m_project.rootPath = path;
    m_tree->setRootIndex(m_fsModel->setRootPath(path));

    // Load a project file from the folder if present.
    const QString proj = QDir(path).filePath(".adaproj");
    if (QFileInfo::exists(proj)) {
        QString err;
        if (m_project.load(proj, &err))
            statusBar()->showMessage(tr("Loaded project %1").arg(proj), 3000);
    }
    m_project.rootPath = path;             // load() doesn't set rootPath
    updateTitle();
    updateDebugActions();
    setWindowFilePath(path);
    QSettings().setValue("lastFolder", path);
    statusBar()->showMessage(tr("Opened folder %1").arg(path), 3000);
}

void MainWindow::restoreLastFolder()
{
    const QString last = QSettings().value("lastFolder").toString();
    if (!last.isEmpty() && QFileInfo(last).isDir())
        openFolderPath(last);
}

void MainWindow::onTreeActivated(const QModelIndex &index)
{
    const QString path = m_fsModel->filePath(index);
    if (!m_fsModel->isDir(index))
        openOrActivate(path);
}

bool MainWindow::saveFile()
{
    QsciScintilla *e = currentEditor();
    if (!e) return false;
    const QString path = editorPath(e);
    return path.isEmpty() ? saveFileAs() : writeToFile(e, path);
}

bool MainWindow::saveFileAs()
{
    QsciScintilla *e = currentEditor();
    if (!e) return false;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save as"), editorPath(e).isEmpty() ? m_project.rootPath : editorPath(e),
        tr("Ada sources (*.adb *.ads *.ada);;All files (*)"));
    return path.isEmpty() ? false : writeToFile(e, path);
}

bool MainWindow::writeToFile(QsciScintilla *e, const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save"),
                             tr("Cannot write %1:\n%2").arg(path, f.errorString()));
        return false;
    }
    f.write(e->text().toUtf8());
    e->setModified(false);
    setEditorPath(e, path);
    m_tabs->setTabText(m_tabs->indexOf(e), QFileInfo(path).fileName());
    statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
    return true;
}

bool MainWindow::maybeSave(QsciScintilla *e)
{
    if (!e || !e->isModified()) return true;
    const auto ret = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("The document has unsaved changes. Save them?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (ret == QMessageBox::Save) { m_tabs->setCurrentWidget(e); return saveFile(); }
    return ret == QMessageBox::Discard;
}

void MainWindow::closeTab(int index)
{
    auto *e = qobject_cast<QsciScintilla *>(m_tabs->widget(index));
    if (!maybeSave(e)) return;
    if (e == m_markerEditor) { m_markerEditor = nullptr; m_markerHandle = -1; }
    m_tabs->removeTab(index);
    delete e;
    if (m_tabs->count() == 0) newFile();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_debugger->isActive()) m_debugger->stop();
    for (int i = m_tabs->count() - 1; i >= 0; --i)
        if (!maybeSave(qobject_cast<QsciScintilla *>(m_tabs->widget(i)))) {
            event->ignore();
            return;
        }
    event->accept();
}

// ---- Edit ----------------------------------------------------------------

void MainWindow::undo()  { if (auto *e = currentEditor()) e->undo(); }
void MainWindow::redo()  { if (auto *e = currentEditor()) e->redo(); }
void MainWindow::cut()   { if (auto *e = currentEditor()) e->cut(); }
void MainWindow::copy()  { if (auto *e = currentEditor()) e->copy(); }
void MainWindow::paste() { if (auto *e = currentEditor()) e->paste(); }

// ---- Search --------------------------------------------------------------

void MainWindow::find()
{
    QsciScintilla *e = currentEditor();
    if (!e) return;
    bool ok = false;
    const QString s = QInputDialog::getText(this, tr("Find"), tr("Find:"),
                                            QLineEdit::Normal, m_lastSearch, &ok);
    if (ok && !s.isEmpty()) {
        m_lastSearch = s;
        if (!e->findFirst(s, false, false, false, true))
            statusBar()->showMessage(tr("Not found: %1").arg(s), 3000);
    }
}

void MainWindow::replace()
{
    QsciScintilla *e = currentEditor();
    if (!e) return;
    bool ok = false;
    const QString s = QInputDialog::getText(this, tr("Replace"), tr("Find:"),
                                            QLineEdit::Normal, m_lastSearch, &ok);
    if (!ok || s.isEmpty()) return;
    m_lastSearch = s;
    const QString r = QInputDialog::getText(this, tr("Replace"), tr("Replace with:"),
                                            QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    int n = 0;
    if (e->findFirst(s, false, false, false, true)) {
        do { e->replace(r); ++n; } while (e->findNext());
    }
    statusBar()->showMessage(tr("%n replacement(s)", "", n), 3000);
}

void MainWindow::searchAgain()
{
    QsciScintilla *e = currentEditor();
    if (e && !m_lastSearch.isEmpty() && !e->findNext())
        statusBar()->showMessage(tr("Not found: %1").arg(m_lastSearch), 3000);
}

void MainWindow::gotoLine()
{
    QsciScintilla *e = currentEditor();
    if (!e) return;
    bool ok = false;
    int line = QInputDialog::getInt(this, tr("Go to line"), tr("Line:"),
                                    1, 1, e->lines(), 1, &ok);
    if (ok) { e->setCursorPosition(line - 1, 0); e->ensureLineVisible(line - 1); }
}

// ---- Language server (definition / hover) --------------------------------

void MainWindow::ensureLsp(const QString &file)
{
    if (m_lsp) return;                       // started once per session
    const QString als = findAlsServer();
    if (als.isEmpty()) {
        statusBar()->showMessage(tr("ada_language_server not found — semantic features off"), 5000);
        return;
    }
    const QString gpr = findGpr(file);
    const QString root = gpr.isEmpty() ? QFileInfo(file).absolutePath()
                                       : QFileInfo(gpr).absolutePath();
    m_lsp = new LspClient(this);
    connect(m_lsp, &LspClient::log, this,
            [this](const QString &m) { statusBar()->showMessage(m, 3000); });
    connect(m_lsp, &LspClient::diagnosticsPublished, this, &MainWindow::onDiagnostics);
    m_lsp->start(als, root, gpr);
}

void MainWindow::requestDefinition(QsciScintilla *e, int line, int index)
{
    if (!m_lsp || !m_lsp->isRunning()) {
        statusBar()->showMessage(tr("Language server not available"), 3000);
        return;
    }
    const QString path = editorPath(e);
    if (path.isEmpty()) return;
    m_lsp->didChange(path, e->text());       // make sure ALS has the latest text
    m_lsp->definition(path, line, index, [this](const QString &uri, int l, int c) {
        if (uri.isEmpty() || l < 0) {
            statusBar()->showMessage(tr("No definition found"), 3000);
            return;
        }
        if (QsciScintilla *t = openOrActivate(LspClient::uriToPath(uri))) {
            t->setCursorPosition(l, c);
            t->ensureLineVisible(l);
            t->setFocus();
        }
    });
}

void MainWindow::requestHover(QsciScintilla *e, int line, int index, const QPoint &anchorGlobal)
{
    if (!m_lsp || !m_lsp->isRunning()) {
        statusBar()->showMessage(tr("Language server not available"), 3000);
        return;
    }
    const QString path = editorPath(e);
    if (path.isEmpty()) return;
    m_lsp->didChange(path, e->text());
    m_lsp->hover(path, line, index, [this, anchorGlobal](const QString &text) {
        if (text.isEmpty()) { statusBar()->showMessage(tr("No info"), 2000); return; }
        showHoverPopup(text, anchorGlobal);
    });
}

// Reformat to the Ada standard via the language server. With no selection the
// whole document is formatted; with a selection only the lines it touches are
// (the range is widened to whole lines, per the user-facing behaviour).
void MainWindow::requestFormat(QsciScintilla *e)
{
    if (!m_lsp || !m_lsp->isRunning()) {
        statusBar()->showMessage(tr("Language server not available"), 3000);
        return;
    }
    const QString path = editorPath(e);
    if (path.isEmpty()) {
        statusBar()->showMessage(tr("Save the file before formatting"), 4000);
        return;
    }
    m_lsp->didChange(path, e->text());          // format the latest text

    const int tabSize = e->tabWidth() > 0 ? e->tabWidth() : 3;
    const bool insertSpaces = !e->indentationsUseTabs();
    auto apply = [this, e](const QList<LspClient::TextEdit> &edits) {
        if (edits.isEmpty()) { statusBar()->showMessage(tr("No formatting changes"), 3000); return; }
        applyTextEdits(e, edits);
        statusBar()->showMessage(tr("Formatted"), 3000);
    };

    if (e->hasSelectedText()) {
        int l1, i1, l2, i2;
        e->getSelection(&l1, &i1, &l2, &i2);
        // A selection that ends at column 0 of a line doesn't include that line.
        int lastLine = (i2 == 0 && l2 > l1) ? l2 - 1 : l2;
        // Widen to whole lines: from the start of l1 to the end of lastLine.
        int endLine, endChar;
        if (lastLine + 1 < e->lines()) {            // end at the start of the next line
            endLine = lastLine + 1;
            endChar = 0;
        } else {                                    // last line: end at its content length
            QString lt = e->text(lastLine);
            while (lt.endsWith('\n') || lt.endsWith('\r')) lt.chop(1);
            endLine = lastLine;
            endChar = lt.length();
        }
        m_lsp->rangeFormatting(path, l1, 0, endLine, endChar, tabSize, insertSpaces, apply);
    } else {
        m_lsp->formatting(path, tabSize, insertSpaces, apply);
    }
}

// Apply LSP TextEdits to the editor as one undo step. Edits are applied last
// position first so earlier (higher) edits keep their coordinates valid.
void MainWindow::applyTextEdits(QsciScintilla *e, const QList<LspClient::TextEdit> &edits)
{
    QList<LspClient::TextEdit> sorted = edits;
    std::sort(sorted.begin(), sorted.end(),
              [](const LspClient::TextEdit &a, const LspClient::TextEdit &b) {
                  if (a.startLine != b.startLine) return a.startLine > b.startLine;
                  return a.startChar > b.startChar;
              });

    int curLine, curIdx;
    e->getCursorPosition(&curLine, &curIdx);
    int firstVisible = e->firstVisibleLine();

    e->beginUndoAction();
    for (const LspClient::TextEdit &te : sorted) {
        e->setSelection(te.startLine, te.startChar, te.endLine, te.endChar);
        e->replaceSelectedText(te.newText);
    }
    e->endUndoAction();

    // Restore a sensible cursor/scroll position (line count may have changed).
    if (curLine >= e->lines()) curLine = e->lines() - 1;
    e->setCursorPosition(curLine, 0);
    e->setFirstVisibleLine(qMin(firstVisible, qMax(0, e->lines() - 1)));

    if (m_lsp) m_lsp->didChange(editorPath(e), e->text());   // keep the server in sync
}

void MainWindow::showHoverPopup(const QString &text, const QPoint &globalPos)
{
    // A floating info label that stays until the user clicks elsewhere or presses
    // Esc (unlike QToolTip, which hides on the first mouse-move) with selectable
    // text. It MUST NOT use Qt::Popup: that flag takes an exclusive X11
    // keyboard+pointer grab over the whole display, so if anything ever stops the
    // popup being dismissed normally (a callback that blocks, a modal dialog, a
    // crash) the grab is never released and the entire desktop locks up with no
    // way to close it or switch windows. Qt::ToolTip floats on top without
    // grabbing input or stealing focus; we dismiss it ourselves via eventFilter.
    if (!m_hoverPopup) {
        m_hoverPopup = new QLabel(nullptr, Qt::ToolTip);
        m_hoverPopup->setAttribute(Qt::WA_ShowWithoutActivating);
        m_hoverPopup->setFocusPolicy(Qt::NoFocus);
        m_hoverPopup->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_hoverPopup->setWordWrap(true);
        m_hoverPopup->setMargin(6);
        m_hoverPopup->setMaximumWidth(640);
        m_hoverPopup->setStyleSheet(
            "QLabel { background:#ffffe1; color:#000; border:1px solid #707070; }");
        // Watch global input so we can hide the popup on click-outside / Esc /
        // scroll — the dismissal Qt::Popup used to give us for free, minus grab.
        qApp->installEventFilter(this);
    }
    m_hoverPopup->setText(text);
    m_hoverPopup->adjustSize();
    m_hoverPopup->move(globalPos + QPoint(0, 4));
    m_hoverPopup->show();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_hoverPopup && m_hoverPopup->isVisible()) {
        switch (event->type()) {
        case QEvent::KeyPress:
            if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                m_hoverPopup->hide();
                return true;          // swallow the Esc that dismissed the popup
            }
            break;
        case QEvent::MouseButtonPress: {
            // A click anywhere outside the popup dismisses it (the popup itself
            // keeps its clicks so text stays selectable).
            QWidget *w = qobject_cast<QWidget *>(watched);
            if (!w || (w->window() != m_hoverPopup)) m_hoverPopup->hide();
            break;
        }
        case QEvent::Wheel:
        case QEvent::FocusOut:
        case QEvent::WindowDeactivate:
            m_hoverPopup->hide();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::gotoDefinitionAtCursor()
{
    QsciScintilla *e = currentEditor();
    if (!e) return;
    int line, index;
    e->getCursorPosition(&line, &index);
    requestDefinition(e, line, index);
}

void MainWindow::triggerCompletion()
{
    QsciScintilla *e = currentEditor();
    if (!e || !isAdaFile(editorPath(e))) return;
    if (!m_lsp || !m_lsp->isRunning()) {
        statusBar()->showMessage(tr("Language server not available"), 3000);
        return;
    }
    int line, index;
    e->getCursorPosition(&line, &index);
    const QString path = editorPath(e);
    if (path.isEmpty()) return;
    m_lsp->didChange(path, e->text());
    m_lsp->completion(path, line, index, [this, e](const QStringList &items) {
        if (items.isEmpty()) { statusBar()->showMessage(tr("No completions"), 2000); return; }
        // Length of the identifier already typed before the cursor.
        int l, i;
        e->getCursorPosition(&l, &i);
        const QString lineText = e->text(l);
        int start = i;
        while (start > 0) {
            const QChar c = lineText.at(start - 1);
            if (c.isLetterOrNumber() || c == '_') --start; else break;
        }
        const int lenEntered = i - start;
        e->SendScintilla(QsciScintilla::SCI_AUTOCSETIGNORECASE, 1UL);   // Ada is case-insensitive
        e->SendScintilla(QsciScintilla::SCI_AUTOCSETORDER,
                         (unsigned long)QsciScintilla::SC_ORDER_PERFORMSORT);
        e->SendScintilla(QsciScintilla::SCI_AUTOCSETSEPARATOR, (unsigned long)'\n');
        const QByteArray list = items.join('\n').toUtf8();
        e->SendScintilla(QsciScintilla::SCI_AUTOCSHOW, (unsigned long)lenEntered, list.constData());
    });
}

void MainWindow::onDiagnostics(const QString &path, const QVector<LspClient::Diagnostic> &diags)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (diags.isEmpty()) m_diagnostics.remove(abs);
    else                 m_diagnostics.insert(abs, diags);
    if (QsciScintilla *e = editorForPath(abs)) applyDiagnostics(e);
    refreshProblems();
}

void MainWindow::applyDiagnostics(QsciScintilla *e)
{
    if (!e) return;
    const int last = qMax(0, e->lines() - 1);
    const int lastLen = e->lineLength(last);
    for (int ind : {INDIC_ERROR, INDIC_WARNING})
        e->clearIndicatorRange(0, 0, last, lastLen, ind);

    for (const auto &d : m_diagnostics.value(editorPath(e))) {
        int el = d.endLine, ec = d.endChar;
        if (el < d.startLine || (el == d.startLine && ec <= d.startChar))
            ec = d.startChar + 1;                       // ensure a visible extent
        const int ind = (d.severity == 1) ? INDIC_ERROR : INDIC_WARNING;
        e->fillIndicatorRange(d.startLine, d.startChar, el, ec, ind);
    }
}

void MainWindow::refreshProblems()
{
    m_problemsList->clear();
    for (auto it = m_diagnostics.constBegin(); it != m_diagnostics.constEnd(); ++it) {
        const QString file = it.key();
        for (const auto &d : it.value()) {
            const QString sev = d.severity == 1 ? "E" : d.severity == 2 ? "W" : "I";
            auto *item = new QListWidgetItem(QStringLiteral("%1  %2:%3  %4")
                .arg(sev, QFileInfo(file).fileName())
                .arg(d.startLine + 1)
                .arg(d.message.split('\n').first()));
            item->setData(Qt::UserRole, file);
            item->setData(Qt::UserRole + 1, d.startLine);
            item->setData(Qt::UserRole + 2, d.startChar);
            item->setToolTip(d.message);
            if (d.severity == 1) item->setForeground(QBrush(QColor("#b00000")));
            m_problemsList->addItem(item);
        }
    }
}

void MainWindow::onProblemActivated(QListWidgetItem *item)
{
    if (!item) return;
    const QString file = item->data(Qt::UserRole).toString();
    const int line = item->data(Qt::UserRole + 1).toInt();
    const int ch = item->data(Qt::UserRole + 2).toInt();
    if (QsciScintilla *e = openOrActivate(file)) {
        e->setCursorPosition(line, ch);
        e->ensureLineVisible(line);
        e->setFocus();
    }
}

// ---- Project / targets ---------------------------------------------------

void MainWindow::newProject()
{
    m_project = Project::makeDefault();
    updateTitle();
    updateDebugActions();
    statusBar()->showMessage(tr("New project"), 3000);
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open project"), m_project.rootPath, tr("AdaEdit project (*.adaproj)"));
    if (path.isEmpty()) return;
    QString err;
    if (!m_project.load(path, &err)) {
        QMessageBox::warning(this, tr("Open project"),
                             tr("Cannot open %1:\n%2").arg(path, err));
        return;
    }
    if (m_project.rootPath.isEmpty())
        m_project.rootPath = QFileInfo(path).absolutePath();
    updateTitle();
    updateDebugActions();
    statusBar()->showMessage(tr("Opened %1").arg(path), 3000);
}

bool MainWindow::saveProject()
{
    QString target = m_project.filePath;
    if (target.isEmpty() && !m_project.rootPath.isEmpty())
        target = QDir(m_project.rootPath).filePath(".adaproj");
    if (target.isEmpty())
        return saveProjectAs();
    QString err;
    if (!m_project.save(target, &err)) {
        QMessageBox::warning(this, tr("Save project"), err);
        return false;
    }
    statusBar()->showMessage(tr("Project saved to %1").arg(target), 3000);
    return true;
}

bool MainWindow::saveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save project as"),
        m_project.rootPath.isEmpty() ? m_project.filePath : m_project.rootPath,
        tr("AdaEdit project (*.adaproj)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(".adaproj")) path += ".adaproj";
    QString err;
    if (!m_project.save(path, &err)) {
        QMessageBox::warning(this, tr("Save project"), err);
        return false;
    }
    statusBar()->showMessage(tr("Project saved to %1").arg(path), 3000);
    return true;
}

// ---- Actions (build/flash/run/monitor) -----------------------------------

CmdContext MainWindow::ctxForCurrent() const
{
    CmdContext ctx;
    if (auto *e = currentEditor()) ctx.file = editorPath(e);

    // "The project folder we are in" = the opened folder. If nothing is open as
    // a folder, fall back to the nearest project root above the current file.
    if (!m_project.rootPath.isEmpty())
        ctx.root = m_project.rootPath;
    else if (!ctx.file.isEmpty())
        ctx.root = findProjectRoot(ctx.file);
    else
        ctx.root = QDir::currentPath();

    // Repo root (launcher/tools) above the opened folder, for {repo}.
    ctx.repo = findRepoRoot(ctx.file.isEmpty() ? ctx.root : ctx.file);

    // Still provided for repo-root/./x style setups that opt back in.
    ctx.example = exampleOf(findRepoRoot(ctx.file.isEmpty() ? ctx.root : ctx.file),
                            ctx.file);
    return ctx;
}

void MainWindow::runAction(const QString &cmdTemplate, const QString &what)
{
    if (cmdTemplate.trimmed().isEmpty()) {
        statusBar()->showMessage(tr("No %1 command for this target").arg(what), 4000);
        return;
    }
    if (m_actionProc && m_actionProc->state() != QProcess::NotRunning) {
        statusBar()->showMessage(tr("A command is already running"), 3000);
        return;
    }
    const CmdContext ctx = ctxForCurrent();
    const QString cmd = TargetProfile::expand(cmdTemplate, ctx);
    const QString cwd = ctx.root.isEmpty() ? QDir::currentPath() : ctx.root;

    m_output->clear();
    m_output->appendPlainText(QStringLiteral("[%1] (%2)\n$ %3\n").arg(what, cwd, cmd));

    if (!m_actionProc) {
        m_actionProc = new QProcess(this);
        m_actionProc->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_actionProc, &QProcess::readyReadStandardOutput,
                this, &MainWindow::onActionOutput);
        connect(m_actionProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus) { onActionFinished(code); });
    }
    m_actionProc->setWorkingDirectory(cwd);
    m_actionProc->start("/bin/sh", {"-c", cmd});
}

void MainWindow::doBuild()
{
    if (const TargetProfile *t = m_project.active()) runAction(t->buildCommand, tr("Build"));
}
void MainWindow::doFlash()
{
    if (const TargetProfile *t = m_project.active()) runAction(t->flashCommand, tr("Flash"));
}
void MainWindow::doRun()
{
    if (const TargetProfile *t = m_project.active()) runAction(t->runCommand, tr("Run"));
}
void MainWindow::doMonitor()
{
    if (const TargetProfile *t = m_project.active()) runAction(t->monitorCommand, tr("Monitor"));
}

void MainWindow::onActionOutput()
{
    if (m_actionProc)
        m_output->appendPlainText(
            QString::fromLocal8Bit(m_actionProc->readAllStandardOutput()).trimmed());
}

void MainWindow::onActionFinished(int exitCode)
{
    m_output->appendPlainText(tr("\n[exit %1]").arg(exitCode));
    statusBar()->showMessage(tr("Finished (exit %1)").arg(exitCode), 5000);
}

// ---- Debug ---------------------------------------------------------------

void MainWindow::debugStart()  { startDebugSession(false); }
void MainWindow::debugAttach() { startDebugSession(true); }

void MainWindow::startDebugSession(bool attach)
{
    const TargetProfile *t = m_project.active();
    if (!t) { statusBar()->showMessage(tr("No target selected"), 3000); return; }
    if (t->gdbCommand.trimmed().isEmpty()) {
        statusBar()->showMessage(tr("Target '%1' has no GDB configured").arg(t->name), 4000);
        return;
    }
    if (auto *e = currentEditor()) maybeSave(e);
    m_debugConsole->clear();
    m_debugger->start(*t, ctxForCurrent(), attach);
}

void MainWindow::debugContinue() { m_debugger->cont(); }
void MainWindow::debugStepOver() { m_debugger->stepOver(); }
void MainWindow::debugStepInto() { m_debugger->stepInto(); }
void MainWindow::debugPause()    { m_debugger->pause(); }
void MainWindow::debugRestart()  { m_debugger->restart(); }
void MainWindow::debugStop()     { m_debugger->stop(); }

void MainWindow::toggleBreakpoint(QsciScintilla *e, int line)
{
    if (!e || line < 0) return;
    const QString path = editorPath(e);
    if (path.isEmpty()) {
        statusBar()->showMessage(tr("Save the file before setting breakpoints"), 4000);
        return;
    }
    // Markers are refreshed via Debugger::breakpointsChanged().
    const int mask = (1 << MARK_BREAKPOINT) | (1 << MARK_BP_DISABLED);
    if (e->markersAtLine(line) & mask) {
        m_debugger->removeBreakpoint(path, line + 1);   // gdb lines are 1-based
        statusBar()->showMessage(tr("Breakpoint cleared at line %1").arg(line + 1), 3000);
    } else {
        m_debugger->addBreakpoint(path, line + 1);
        statusBar()->showMessage(tr("Breakpoint set at line %1").arg(line + 1), 3000);
    }
}

void MainWindow::onMarginClicked(int margin, int line, Qt::KeyboardModifiers)
{
    if (margin != BP_MARGIN) return;
    toggleBreakpoint(qobject_cast<QsciScintilla *>(sender()), line);
}

void MainWindow::onEditorContextMenu(const QPoint &pos)
{
    auto *e = qobject_cast<QsciScintilla *>(sender());
    if (!e) return;
    int line = e->lineAt(pos);                 // 0-based; -1 past the last line
    if (line < 0) { int col; e->getCursorPosition(&line, &col); }
    const bool has = e->markersAtLine(line) & (1 << MARK_BREAKPOINT);

    // Exact click position (line + char index) for language-server requests.
    int defLine = line, defIndex = 0;
    const long p = e->SendScintilla(QsciScintilla::SCI_POSITIONFROMPOINT,
                                    (unsigned long)pos.x(), (long)pos.y());
    if (p >= 0) e->lineIndexFromPosition(p, &defLine, &defIndex);

    QMenu menu;
    const bool ada = isAdaFile(editorPath(e)) && m_lsp && m_lsp->isRunning();
    QAction *def = menu.addAction(tr("Go to definition"));
    QAction *hov = menu.addAction(tr("Quick info (hover)"));
    def->setEnabled(ada);
    hov->setEnabled(ada);
    menu.addSeparator();
    QAction *fmt = menu.addAction(e->hasSelectedText() ? tr("Format selection")
                                                       : tr("Format document"));
    fmt->setEnabled(ada);
    menu.addSeparator();
    QAction *bp = menu.addAction(has ? tr("Clear breakpoint") : tr("Set breakpoint"));
    menu.addSeparator();
    QAction *cut   = menu.addAction(tr("Cut"));
    QAction *copy  = menu.addAction(tr("Copy"));
    QAction *paste = menu.addAction(tr("Paste"));
    menu.addSeparator();
    QAction *all   = menu.addAction(tr("Select All"));

    const bool sel = e->hasSelectedText();
    cut->setEnabled(sel);
    copy->setEnabled(sel);

    QAction *chosen = menu.exec(e->mapToGlobal(pos));
    if (chosen == def)        requestDefinition(e, defLine, defIndex);
    else if (chosen == hov)   requestHover(e, defLine, defIndex, e->mapToGlobal(pos));
    else if (chosen == fmt)   requestFormat(e);
    else if (chosen == bp)    toggleBreakpoint(e, line);
    else if (chosen == cut)   e->cut();
    else if (chosen == copy)  e->copy();
    else if (chosen == paste) e->paste();
    else if (chosen == all)   e->selectAll();
}

// Markers/pane are refreshed by onBreakpointsChanged(); these only narrate.
void MainWindow::onBreakpointResolved(const QString &, int requestedLine, int actualLine)
{
    statusBar()->showMessage(
        tr("Breakpoint moved to line %1 (no code at %2)").arg(actualLine).arg(requestedLine), 4000);
}

void MainWindow::onBreakpointRejected(const QString &, int, const QString &message)
{
    statusBar()->showMessage(tr("Breakpoint rejected: %1").arg(message.trimmed()), 5000);
}

void MainWindow::onBreakpointsChanged()
{
    refreshBreakpointsPane();
    for (int i = 0; i < m_tabs->count(); ++i)
        applyBreakpointMarkers(qobject_cast<QsciScintilla *>(m_tabs->widget(i)));
}

void MainWindow::refreshBreakpointsPane()
{
    QSignalBlocker block(m_bpList);
    m_bpList->clear();
    for (const auto &b : m_debugger->breakpoints()) {
        auto *it = new QListWidgetItem(
            QStringLiteral("%1:%2").arg(QFileInfo(b.file).fileName()).arg(b.line));
        it->setToolTip(QStringLiteral("%1:%2").arg(b.file).arg(b.line));
        it->setData(Qt::UserRole, b.file);
        it->setData(Qt::UserRole + 1, b.line);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(b.enabled ? Qt::Checked : Qt::Unchecked);
        m_bpList->addItem(it);
    }
}

void MainWindow::applyBreakpointMarkers(QsciScintilla *e)
{
    if (!e) return;
    const QString path = editorPath(e);
    e->markerDeleteAll(MARK_BREAKPOINT);
    e->markerDeleteAll(MARK_BP_DISABLED);
    for (const auto &b : m_debugger->breakpoints())
        if (b.file == path)
            e->markerAdd(b.line - 1, b.enabled ? MARK_BREAKPOINT : MARK_BP_DISABLED);
}

void MainWindow::onBreakpointItemChanged(QListWidgetItem *item)
{
    if (!item) return;
    m_debugger->setBreakpointEnabled(item->data(Qt::UserRole).toString(),
                                     item->data(Qt::UserRole + 1).toInt(),
                                     item->checkState() == Qt::Checked);
}

void MainWindow::onBreakpointActivated(QListWidgetItem *item)
{
    if (!item) return;
    const QString file = item->data(Qt::UserRole).toString();
    const int line = item->data(Qt::UserRole + 1).toInt();
    if (QsciScintilla *e = openOrActivate(file)) {
        e->setCursorPosition(line - 1, 0);
        e->ensureLineVisible(line - 1);
    }
}

void MainWindow::onLocalsUpdated(const QVector<Debugger::Variable> &vars)
{
    qDeleteAll(m_localsRoot->takeChildren());
    for (const auto &v : vars)
        new QTreeWidgetItem(m_localsRoot, {v.name, v.value});
    m_localsRoot->setExpanded(true);
    m_varsTree->resizeColumnToContents(0);
}

void MainWindow::onWatchEvaluated(const QString &name, const QString &value, bool ok)
{
    for (int i = 0; i < m_watchRoot->childCount(); ++i) {
        QTreeWidgetItem *it = m_watchRoot->child(i);
        if (it->text(0) == name) {
            it->setText(1, ok ? value : QStringLiteral("⚠ %1").arg(value));
            it->setForeground(1, ok ? QBrush() : QBrush(QColor("#b00000")));
            return;
        }
    }
}

void MainWindow::onAddWatch()
{
    const QString expr = m_watchInput->text().trimmed();
    if (expr.isEmpty() || m_watches.contains(expr)) { m_watchInput->clear(); return; }
    m_watches << expr;
    new QTreeWidgetItem(m_watchRoot, {expr, QString()});
    m_watchRoot->setExpanded(true);
    m_watchInput->clear();
    if (m_debugger->state() == Debugger::Stopped)
        m_debugger->evaluate(expr, expr);
}

void MainWindow::onThreadsUpdated(const QVector<Debugger::ThreadInfo> &threads, int currentId)
{
    m_threadsList->clear();
    for (const auto &t : threads) {
        QString loc;
        if (!t.func.isEmpty()) loc += QStringLiteral(" — %1").arg(t.func);
        if (!t.file.isEmpty() && t.line > 0)
            loc += QStringLiteral(" (%1:%2)").arg(QFileInfo(t.file).fileName()).arg(t.line);
        const QString who = t.name.isEmpty() ? t.targetId : t.name;
        const bool cur = (t.id == currentId);
        auto *it = new QListWidgetItem(
            QStringLiteral("%1#%2 %3%4").arg(cur ? "▶ " : "   ").arg(t.id).arg(who, loc));
        it->setData(Qt::UserRole, t.id);
        it->setData(Qt::UserRole + 1, t.file);
        it->setData(Qt::UserRole + 2, t.line);
        if (cur) { QFont f = it->font(); f.setBold(true); it->setFont(f); }
        m_threadsList->addItem(it);
    }
}

void MainWindow::onThreadActivated(QListWidgetItem *item)
{
    if (!item) return;
    const int id = item->data(Qt::UserRole).toInt();
    const QString file = item->data(Qt::UserRole + 1).toString();
    const int line = item->data(Qt::UserRole + 2).toInt();
    m_debugger->selectThread(id);                 // refreshes threads + locals
    if (!file.isEmpty() && line > 0) showDebugLine(file, line);
    for (const QString &expr : m_watches)
        m_debugger->evaluate(expr, expr);
}

void MainWindow::onStackUpdated(const QVector<Debugger::Frame> &frames, int currentLevel)
{
    m_stackList->clear();
    for (const auto &f : frames) {
        QString loc;
        if (!f.file.isEmpty() && f.line > 0)
            loc = QStringLiteral(" (%1:%2)").arg(QFileInfo(f.file).fileName()).arg(f.line);
        auto *it = new QListWidgetItem(
            QStringLiteral("#%1 %2%3").arg(f.level).arg(f.func.isEmpty() ? "??" : f.func, loc));
        it->setData(Qt::UserRole, f.level);
        it->setData(Qt::UserRole + 1, f.file);
        it->setData(Qt::UserRole + 2, f.line);
        if (f.level == currentLevel) { QFont ft = it->font(); ft.setBold(true); it->setFont(ft); }
        m_stackList->addItem(it);
    }
}

void MainWindow::onStackActivated(QListWidgetItem *item)
{
    if (!item) return;
    const int level = item->data(Qt::UserRole).toInt();
    const QString file = item->data(Qt::UserRole + 1).toString();
    const int line = item->data(Qt::UserRole + 2).toInt();
    m_debugger->selectFrame(level);               // refreshes locals for the frame
    if (!file.isEmpty() && line > 0) showDebugLine(file, line);
    for (const QString &expr : m_watches)
        m_debugger->evaluate(expr, expr);
    // Reflect the selected frame as current.
    for (int i = 0; i < m_stackList->count(); ++i) {
        QListWidgetItem *r = m_stackList->item(i);
        QFont f = r->font();
        f.setBold(r == item);
        r->setFont(f);
    }
}

void MainWindow::onVarsContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_varsTree->itemAt(pos);
    if (!it || it->parent() != m_watchRoot) return;   // only watch rows
    QMenu menu;
    QAction *rm = menu.addAction(tr("Remove watch"));
    if (menu.exec(m_varsTree->viewport()->mapToGlobal(pos)) == rm) {
        m_watches.removeAll(it->text(0));
        delete it;
    }
}

void MainWindow::onDebugConsoleReturn()
{
    const QString cmd = m_debugInput->text().trimmed();
    if (cmd.isEmpty() || !m_debugger->isActive()) return;
    m_debugger->sendRaw(cmd);
    m_debugInput->clear();
}

void MainWindow::onDebugOutput(const QString &text)
{
    m_debugConsole->moveCursor(QTextCursor::End);
    m_debugConsole->insertPlainText(text);
    m_debugConsole->moveCursor(QTextCursor::End);
}

void MainWindow::onDebugStateChanged(int state)
{
    updateDebugActions();
    if (state == Debugger::Idle) {
        clearDebugLine();
        if (m_localsRoot) qDeleteAll(m_localsRoot->takeChildren());
        if (m_watchRoot)
            for (int i = 0; i < m_watchRoot->childCount(); ++i)
                m_watchRoot->child(i)->setText(1, QString());
        if (m_threadsList) m_threadsList->clear();
        if (m_stackList) m_stackList->clear();
    }
    const char *names[] = {"idle", "starting", "running", "stopped"};
    if (state >= 0 && state <= 3)
        statusBar()->showMessage(tr("Debugger: %1").arg(names[state]), 4000);
}

void MainWindow::onDebugStopped(const QString &fullPath, int line, const QString &reason)
{
    Q_UNUSED(reason);
    showDebugLine(fullPath, line);
    // Locals refresh automatically; re-evaluate watches for the new frame.
    for (const QString &expr : m_watches)
        m_debugger->evaluate(expr, expr);
}

void MainWindow::updateDebugActions()
{
    const int st = m_debugger ? m_debugger->state() : Debugger::Idle;
    const bool idle = (st == Debugger::Idle);
    const bool stopped = (st == Debugger::Stopped);
    const bool running = (st == Debugger::Running);
    const TargetProfile *t = m_project.active();
    const bool canStart = idle && t && !t->gdbCommand.trimmed().isEmpty();

    if (m_actStart)    m_actStart->setEnabled(canStart);
    if (m_actAttach)   m_actAttach->setEnabled(canStart);
    if (m_actContinue) m_actContinue->setEnabled(stopped);
    if (m_actStepOver) m_actStepOver->setEnabled(stopped);
    if (m_actStepInto) m_actStepInto->setEnabled(stopped);
    if (m_actPause)    m_actPause->setEnabled(running);
    if (m_actRestart)  m_actRestart->setEnabled(!idle);
    if (m_actStop)     m_actStop->setEnabled(!idle);
    if (m_actSmp) {
        QSignalBlocker block(m_actSmp);     // reflect, don't re-trigger
        m_actSmp->setEnabled(t != nullptr);
        m_actSmp->setChecked(t && t->debugSmp);
    }
}

void MainWindow::onSmpToggled(bool on)
{
    if (m_project.activeIndex < 0 || m_project.activeIndex >= m_project.targets.size())
        return;
    m_project.targets[m_project.activeIndex].debugSmp = on;
    m_project.dirty = true;
    if (m_debugger->isActive())
        statusBar()->showMessage(tr("Dual-core (SMP) applies on next Start debugging"), 4000);
    else
        statusBar()->showMessage(tr("Dual-core (SMP) %1").arg(on ? tr("on") : tr("off")), 3000);
}

void MainWindow::showDebugLine(const QString &fullPath, int line)
{
    clearDebugLine();
    QsciScintilla *e = openOrActivate(fullPath);
    if (!e) return;
    m_markerEditor = e;
    m_markerHandle = e->markerAdd(line - 1, MARK_DEBUG);
    e->setCursorPosition(line - 1, 0);
    e->ensureLineVisible(line - 1);
}

void MainWindow::clearDebugLine()
{
    if (m_markerEditor && m_markerHandle >= 0)
        m_markerEditor->markerDeleteHandle(m_markerHandle);
    m_markerEditor = nullptr;
    m_markerHandle = -1;
}

// ---- misc ----------------------------------------------------------------

void MainWindow::updateTitle()
{
    QsciScintilla *e = currentEditor();
    QString name = tr("untitled");
    if (e) {
        const QString p = editorPath(e);
        if (!p.isEmpty()) name = QFileInfo(p).fileName();
        int idx = m_tabs->indexOf(e);
        m_tabs->setTabText(idx, (e->isModified() ? "*" : "") + name);
    }
    const TargetProfile *t = m_project.active();
    setWindowTitle(tr("%1 [%2] - AdaEdit").arg(name, t ? t->name : tr("no target")));
}
