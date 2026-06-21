#pragma once
#include <QObject>
#include <QByteArray>
#include <QVector>
#include <QHash>
#include <QList>
#include "project.h"

class QProcess;

// Drives an on-chip debug session: starts the target's debug-server command
// (e.g. OpenOCD exposing a gdb server on :3333), then runs gdb in MI mode,
// connects to the server, and stops at the init breakpoint (e.g. app_main).
// Step/continue/restart are issued as GDB/MI commands; *stopped events are
// parsed to report the current source location.
class Debugger : public QObject
{
    Q_OBJECT
public:
    enum State { Idle, Starting, Running, Stopped };

    struct Breakpoint { QString file; int line; bool enabled = true; };
    struct Variable { QString name; QString value; };
    struct ThreadInfo {
        int id = 0; QString targetId; QString name;
        QString func; QString file; int line = -1;
    };
    struct Frame { int level = 0; QString func; QString file; int line = -1; };

    explicit Debugger(QObject *parent = nullptr);
    ~Debugger() override;

    // attach = post-mortem: connect + "monitor halt" (no reset, no run-to-entry),
    // freezing the running target in place to inspect it (pairs with SMP).
    void start(const TargetProfile &t, const CmdContext &ctx, bool attach = false);
    void cont();        // continue
    void stepOver();    // next
    void stepInto();    // step
    void pause();       // interrupt a running target (SIGINT to gdb)
    void restart();     // reset halt -> re-arm -> continue
    void stop();        // end session
    void selectFrame(int level);   // select a call-stack frame, refresh locals
    void sendRaw(const QString &gdbCommand);

    // Source-line breakpoints (1-based lines). Persist across the object's life
    // and are (re)inserted whenever a session starts; toggled live if running.
    void addBreakpoint(const QString &file, int line);
    void removeBreakpoint(const QString &file, int line);
    void setBreakpointEnabled(const QString &file, int line, bool enabled);
    QList<int> breakpointLines(const QString &file) const;
    QVector<Breakpoint> breakpoints() const { return m_breakpoints; }

    // Evaluate a watch expression in the current frame; result via watchEvaluated.
    void evaluate(const QString &name, const QString &expr);
    // Switch the current gdb thread (then refreshes threads + locals).
    void selectThread(int id);

    State state() const { return m_state; }
    bool isActive() const { return m_state != Idle; }

signals:
    void output(const QString &text);
    void stateChanged(int state);
    void stoppedAt(const QString &fullPath, int line, const QString &reason);
    // gdb relocated a breakpoint to the nearest code line.
    void breakpointResolved(const QString &file, int requestedLine, int actualLine);
    // gdb refused the breakpoint (e.g. no code at that line).
    void breakpointRejected(const QString &file, int line, const QString &message);
    // The breakpoint set changed (added / removed / relocated / enabled).
    void breakpointsChanged();
    // Local variables of the current frame, refreshed on each stop.
    void localsUpdated(const QVector<Debugger::Variable> &vars);
    // Result of a watch expression evaluation.
    void watchEvaluated(const QString &name, const QString &value, bool ok);
    // Thread list of the stopped target, refreshed on each stop / thread switch.
    void threadsUpdated(const QVector<Debugger::ThreadInfo> &threads, int currentId);
    // Call stack of the current thread, refreshed on each stop / thread switch.
    void stackUpdated(const QVector<Debugger::Frame> &frames, int currentLevel);

private slots:
    void onGdbStdout();
    void onGdbStderr();
    void onServerOutput();
    void onGdbFinished(int exitCode);

private:
    void launchGdb();
    void armEntryBreakpoint();
    void requestFrameState();        // refresh thread list + locals
    void insertBreakpointNow(const Breakpoint &b);
    void sendMi(const QString &cmd);
    void sendMiToken(int token, const QString &cmd);
    void handleLine(const QString &line);
    void setState(State s);
    static QString bpKey(const QString &file, int line);

    QProcess *m_server = nullptr;
    QProcess *m_gdb = nullptr;
    TargetProfile m_target;
    CmdContext m_ctx;
    State m_state = Idle;
    bool m_attach = false;            // post-mortem (halt in place) session
    bool m_pendingAttachJump = false; // jump editor to current frame once attached
    QByteArray m_gdbBuf;

    QVector<Breakpoint> m_breakpoints;        // user breakpoints (persist)
    QHash<QString, int> m_bkptNumbers;        // key -> gdb breakpoint number
    QHash<int, Breakpoint> m_bkptTokens;      // MI token -> requested bp (awaiting result)
    int m_localsToken = -1;                   // MI token for the pending locals query
    int m_threadsToken = -1;                  // MI token for the pending thread-info query
    int m_stackToken = -1;                    // MI token for the pending stack query
    QHash<int, QString> m_evalTokens;         // MI token -> watch name (awaiting value)
    int m_token = 0;
};
