#pragma once
#include <QMainWindow>
#include <QFont>
#include <QKeySequence>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QVector>
#include "project.h"
#include "debugger.h"
#include "lspclient.h"

class QTabWidget;
class QDockWidget;
class QPlainTextEdit;
class QComboBox;
class QTreeView;
class QFileSystemModel;
class QLineEdit;
class QLabel;
class QAction;
class QListWidget;
class QListWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;
class QsciScintilla;
class LspClient;

// A user-rebindable command: a menu/toolbar QAction plus the identity and
// default key sequence used by the Keyboard Shortcuts editor and QSettings.
struct Command {
    QString id;                 // stable key, e.g. "file.save"
    QString name;              // human label (menu text, sans '&')
    QKeySequence defaultSeq;    // factory default shortcut
    QAction *action = nullptr;
};

// AdaEdit main window: a folder-rooted project explorer, tabbed Ada editors,
// configurable per-target actions (build/flash/run/monitor) and an in-editor
// GDB/MI debugger.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openPath(const QString &path);
    void openFolderPath(const QString &path);
    void restoreLastFolder();
    void restoreSession();        // reopen last folder + files + active tab
    void ensureOpenTab();         // create an untitled tab if none are open

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void saveSession();           // window geometry + dock layout + open files

private slots:
    // File / folder
    void newFile();
    void openFile();
    void openFolder();
    void onTreeActivated(const QModelIndex &index);
    bool saveFile();
    bool saveFileAs();
    void saveAll();
    bool closeTab(int index);
    void onTabContextMenu(const QPoint &pos);
    void openSettings();
    void openShortcuts();
    void formatCurrent();
    void toggleBreakpointAtCursor();
    void addWatchDialog();

    // Edit
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();

    // Search
    void find();
    void replace();
    void searchAgain();
    void gotoLine();

    // Project
    void newProject();
    void openProject();
    bool saveProject();
    bool saveProjectAs();
    void onProfileChanged(int index);
    void populatePorts();          // (re)enumerate connected serial devices
    void onPortChanged(int index); // selected device -> $ESPPORT for child processes

    // Actions (ESP32-S3 target)
    void doBuild();
    void doFlash();
    void doRun();
    void doMonitor();
    void showRuntimePath();        // echo the resolved RTS path to the output pane
    void setupDevice();            // run the one-time udev/groups installer (sudo/pkexec)
    void onActionOutput();
    void onActionFinished(int exitCode);

    // Debug
    void debugStart();
    void debugAttach();
    void debugContinue();
    void debugStepOver();
    void debugStepInto();
    void debugPause();
    void debugRestart();
    void debugStop();
    void onSmpToggled(bool on);
    void onDebugOutput(const QString &text);
    void onDebugStateChanged(int state);
    void onDebugStopped(const QString &fullPath, int line, const QString &reason);
    void onDebugConsoleReturn();
    void onMarginClicked(int margin, int line, Qt::KeyboardModifiers state);
    void onEditorContextMenu(const QPoint &pos);
    void gotoDefinitionAtCursor();
    void renameSymbolAtCursor();   // refactor: rename the symbol under the cursor (F2)
    void codeActionsAtCursor();    // refactor: list code actions at the cursor (Ctrl+.)
    void triggerCompletion();
    void onDiagnostics(const QString &path, const QVector<LspClient::Diagnostic> &diags);
    void onProblemActivated(QListWidgetItem *item);
    void onBreakpointResolved(const QString &file, int requestedLine, int actualLine);
    void onBreakpointRejected(const QString &file, int line, const QString &message);
    void onBreakpointsChanged();
    void onBreakpointItemChanged(QListWidgetItem *item);
    void onBreakpointActivated(QListWidgetItem *item);
    void onLocalsUpdated(const QVector<Debugger::Variable> &vars);
    void onWatchEvaluated(const QString &name, const QString &value, bool ok);
    void onAddWatch();
    void onVarsContextMenu(const QPoint &pos);
    void onThreadsUpdated(const QVector<Debugger::ThreadInfo> &threads, int currentId);
    void onThreadActivated(QListWidgetItem *item);
    void onStackUpdated(const QVector<Debugger::Frame> &frames, int currentLevel);
    void onStackActivated(QListWidgetItem *item);

    void updateTitle();

private:
    QsciScintilla *newEditor();
    QsciScintilla *currentEditor() const;
    QsciScintilla *openOrActivate(const QString &path);
    QsciScintilla *editorForPath(const QString &path) const;
    void toggleBreakpoint(QsciScintilla *e, int line);   // line is 0-based
    void refreshBreakpointsPane();
    void applyBreakpointMarkers(QsciScintilla *e);
    void ensureLsp(const QString &file);
    void requestDefinition(QsciScintilla *e, int line, int index);
    void requestHover(QsciScintilla *e, int line, int index, const QPoint &anchorGlobal);
    void requestFormat(QsciScintilla *e);
    void applyTextEdits(QsciScintilla *e, const QList<LspClient::TextEdit> &edits);
    void renameSymbol(QsciScintilla *e, int line, int index);   // LSP rename (refactor)
    void applyWorkspaceEdit(const LspClient::WorkspaceEdit &edits);
    void requestCodeActions(QsciScintilla *e);                  // LSP code actions (refactor menu)
    void applyCodeAction(const LspClient::CodeAction &action);
    void applyDiagnostics(QsciScintilla *e);
    void refreshProblems();
    void showHoverPopup(const QString &text, const QPoint &globalPos);
    bool maybeSave(QsciScintilla *e);
    bool writeToFile(QsciScintilla *e, const QString &path);
    void closeOtherTabs(int index);
    void closeTabsToLeft(int index);
    void closeTabsToRight(int index);
    void setEditorPath(QsciScintilla *e, const QString &path);
    QString editorPath(QsciScintilla *e) const;

    void createMenus();
    // Register a menu/toolbar action as a rebindable command and apply its
    // stored-or-default shortcut.
    void registerCmd(QAction *a, const QString &id,
                     const QKeySequence &def = QKeySequence());
    QKeySequence shortcutFor(const QString &id, const QKeySequence &def) const;
    void applyTheme();                       // app palette + all editors
    void applyEditorTheme(QsciScintilla *e); // QScintilla colours (not palette-driven)
    void applyFonts();                        // interface font + editor/dock font
    void createActionBar();
    void createDebugBar();
    void createDocks();
    void updateDebugActions();
    void startDebugSession(bool attach);

    CmdContext ctxForCurrent() const;
    QString sdkTool(const QString &rel) const;   // resolve an SDK file (bundle/env/repo)
    void revealSerial();                          // show/raise the serial-monitor dock
    void closeSerial();                           // release the serial port (for flashing)
    void runAction(const QString &cmdTemplate, const QString &what);
    void showDebugLine(const QString &fullPath, int line);
    void clearDebugLine();

    QTabWidget *m_tabs = nullptr;
    QList<QDockWidget *> m_docks;         // all docks, for the View menu toggles
    QList<Command> m_commands;            // all rebindable commands (keyboard map)
    QComboBox *m_profileCombo = nullptr;  // Ada runtime profile selector
    QString m_profile;                    // selected profile -> ./x --profile {profile}
    QComboBox *m_portCombo = nullptr;     // target serial device (multi-board)
    QString m_port;                       // selected /dev/tty* -> $ESPPORT (flash/debug/monitor)
    QPlainTextEdit *m_output = nullptr;
    // (no target picker: this editor is ESP32-S3-only)
    QPlainTextEdit *m_debugConsole = nullptr;
    QLineEdit *m_debugInput = nullptr;
    QListWidget *m_bpList = nullptr;
    QTreeWidget *m_varsTree = nullptr;
    QTreeWidgetItem *m_localsRoot = nullptr;
    QTreeWidgetItem *m_watchRoot = nullptr;
    QLineEdit *m_watchInput = nullptr;
    QLabel *m_hoverPopup = nullptr;
    QStringList m_watches;
    QListWidget *m_threadsList = nullptr;
    QListWidget *m_stackList = nullptr;
    QListWidget *m_problemsList = nullptr;
    class SerialMonitor *m_serial = nullptr;   // native QSerialPort console
    QHash<QString, QVector<LspClient::Diagnostic>> m_diagnostics;
    QTreeView *m_tree = nullptr;
    QFileSystemModel *m_fsModel = nullptr;
    class QProcess *m_actionProc = nullptr;
    QString m_actionLog;            // accumulated action output (for device-error detection)
    QString m_actionWhat;           // label of the running action ("Flash", "Run", …)
    bool m_monitorAfterAction = false;   // open the serial monitor when this action succeeds
    Debugger *m_debugger = nullptr;
    LspClient *m_lsp = nullptr;

    // Debug current-line marker.
    QsciScintilla *m_markerEditor = nullptr;
    int m_markerHandle = -1;

    // Debug actions (toggled by session state).
    QAction *m_actStart = nullptr;
    QAction *m_actContinue = nullptr;
    QAction *m_actStepOver = nullptr;
    QAction *m_actStepInto = nullptr;
    QAction *m_actPause = nullptr;
    QAction *m_actRestart = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actAttach = nullptr;
    QAction *m_actSmp = nullptr;

    QString m_lastSearch;
    Project m_project;

    // Appearance. Captured at startup so light mode can be restored exactly.
    bool m_darkMode = false;
    QString m_defaultStyle;
    QPalette m_lightPalette;

    // Fonts. The interface font drives the app chrome (menus, titles, dialogs);
    // the editor font (monospaced by default) is forced onto the editors and the
    // dock content widgets so they stay fixed-width regardless of interface font.
    // The defaults are captured at startup so Settings can reset to them.
    QFont m_defaultInterfaceFont;
    QFont m_defaultEditorFont;
    QFont m_interfaceFont;
    QFont m_editorFont;
};
