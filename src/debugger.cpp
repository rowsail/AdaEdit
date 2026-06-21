#include "debugger.h"

#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QRegularExpression>
#include <csignal>
#include <sys/types.h>

Debugger::Debugger(QObject *parent) : QObject(parent) {}

Debugger::~Debugger() { stop(); }

void Debugger::setState(State s)
{
    if (m_state != s) { m_state = s; emit stateChanged(s); }
}

// Decode a GDB/MI C-style string ("...") into plain text.
static QString miUnquote(const QString &s)
{
    QString in = s;
    if (in.startsWith('"') && in.endsWith('"')) in = in.mid(1, in.size() - 2);
    QString out;
    for (int i = 0; i < in.size(); ++i) {
        QChar c = in[i];
        if (c == '\\' && i + 1 < in.size()) {
            QChar n = in[++i];
            switch (n.toLatin1()) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            default: out += n; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

void Debugger::start(const TargetProfile &t, const CmdContext &ctx, bool attach)
{
    if (m_state != Idle) return;
    m_target = t;
    m_ctx = ctx;
    m_attach = attach;
    setState(Starting);

    const QString serverCmd =
        TargetProfile::expand(t.debugServerCommand, ctx).trimmed();

    if (serverCmd.isEmpty()) {
        emit output(tr("[debug] no debug server command; launching gdb directly\n"));
        launchGdb();
        return;
    }

    // Run the server (OpenOCD) from the repo root, where tools/ + its relative
    // paths resolve; fall back to the project folder.
    const QString serverCwd = !ctx.repo.isEmpty() ? ctx.repo : ctx.root;

    // Clear any stale OpenOCD: an orphan from a previous session would still
    // hold :3333 (and the USB-JTAG), so we'd silently attach to it instead of
    // our own — wrong ESP_RTOS, single "Remote target" thread.
    if (QProcess::execute("pkill", {"-x", "openocd"}) == 0) {
        emit output("[debug] killed stale OpenOCD; waiting for :3333 to free\n");
        for (int i = 0; i < 20; ++i) {                 // up to ~2s
            if (QProcess::execute("pgrep", {"-x", "openocd"}) != 0) break;
            QThread::msleep(100);
        }
    }

    // Prepend ESP_RTOS=hwthread inline (exactly like `./x debug --smp`).
    QString cmd = serverCmd;
    if (t.debugSmp) {
        cmd = "env ESP_RTOS=hwthread " + cmd;
        emit output("[debug] SMP: ESP_RTOS=hwthread (both cores as gdb threads)\n");
    } else {
        emit output("[debug] SMP: off (single core; enable Debug > Dual-core)\n");
    }
    emit output(QStringLiteral("[debug] server: %1\n").arg(cmd));
    m_server = new QProcess(this);
    m_server->setProcessChannelMode(QProcess::MergedChannels);
    if (!serverCwd.isEmpty()) m_server->setWorkingDirectory(serverCwd);
    connect(m_server, &QProcess::readyReadStandardOutput,
            this, &Debugger::onServerOutput);
    // 'exec' so this QProcess becomes openocd itself (openocd.sh ends with
    // `exec openocd`) — then stop() kills it cleanly instead of orphaning it.
    m_server->start("/bin/sh", {"-c", "exec " + cmd});

    // Give the gdb server a moment to start listening, then attach gdb.
    QTimer::singleShot(1500, this, [this] {
        if (m_state == Starting) launchGdb();
    });
}

void Debugger::onServerOutput()
{
    if (m_server)
        emit output(QString::fromLocal8Bit(m_server->readAllStandardOutput()));
}

void Debugger::launchGdb()
{
    const QString gdb = m_target.gdbCommand.isEmpty()
        ? QStringLiteral("gdb")
        : TargetProfile::expand(m_target.gdbCommand, m_ctx);
    const QString prog = TargetProfile::expand(m_target.gdbProgram, m_ctx);

    QStringList args = {"-q", "-nx", "--interpreter=mi2"};
    if (!prog.isEmpty()) args << prog;

    emit output(QStringLiteral("[debug] gdb: %1 %2\n").arg(gdb, args.join(' ')));

    m_gdb = new QProcess(this);
    if (!m_ctx.root.isEmpty()) m_gdb->setWorkingDirectory(m_ctx.root);
    connect(m_gdb, &QProcess::readyReadStandardOutput, this, &Debugger::onGdbStdout);
    connect(m_gdb, &QProcess::readyReadStandardError, this, &Debugger::onGdbStderr);
    connect(m_gdb, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) { onGdbFinished(code); });
    m_gdb->start(gdb, args);

    if (!m_gdb->waitForStarted(4000)) {
        emit output(QStringLiteral("[debug] failed to start gdb '%1'\n").arg(gdb));
        stop();
        return;
    }

    // Connect to the running gdb server and run to the entry breakpoint.
    // NOTE: plain "remote" (not extended-remote) — matches the working ./x flow;
    // OpenOCD plants ESP32 flash breakpoints correctly only this way.
    const QString remote = m_target.gdbRemote.isEmpty() ? "localhost:3333"
                                                        : m_target.gdbRemote;
    sendMi(QStringLiteral("-target-select remote %1").arg(remote));

    if (m_attach) {
        // Post-mortem: freeze in place, no reset, no run-to-entry. Plant the
        // user's breakpoints (in case they resume), then inspect the halt.
        sendMi("-interpreter-exec console \"monitor halt\"");
        m_bkptNumbers.clear();
        m_bkptTokens.clear();
        for (const Breakpoint &b : m_breakpoints)
            insertBreakpointNow(b);
        setState(Stopped);
        m_pendingAttachJump = true;
        requestFrameState();          // threads + locals; jump on thread-info
    } else {
        sendMi("-interpreter-exec console \"monitor reset halt\"");
        armEntryBreakpoint();
        sendMi("-exec-continue");
    }
}

void Debugger::armEntryBreakpoint()
{
    const QString bp = m_target.initBreakpoint.isEmpty() ? "main"
                                                         : m_target.initBreakpoint;
    sendMi(QStringLiteral("-break-insert -t %1").arg(bp));

    // (Re)insert the user's source-line breakpoints for this session.
    m_bkptNumbers.clear();
    m_bkptTokens.clear();
    for (const Breakpoint &b : m_breakpoints)
        insertBreakpointNow(b);
}

QString Debugger::bpKey(const QString &file, int line)
{
    return file + ':' + QString::number(line);
}

void Debugger::insertBreakpointNow(const Breakpoint &b)
{
    const int tok = ++m_token;
    m_bkptTokens.insert(tok, b);
    sendMiToken(tok, QStringLiteral("-break-insert %1:%2").arg(b.file).arg(b.line));
}

void Debugger::addBreakpoint(const QString &file, int line)
{
    for (const Breakpoint &b : m_breakpoints)
        if (b.file == file && b.line == line) return;   // already set
    const Breakpoint nb{file, line, true};
    m_breakpoints.push_back(nb);
    if (m_gdb && m_gdb->state() == QProcess::Running)
        insertBreakpointNow(nb);
    emit breakpointsChanged();
}

void Debugger::removeBreakpoint(const QString &file, int line)
{
    for (int i = 0; i < m_breakpoints.size(); ++i)
        if (m_breakpoints[i].file == file && m_breakpoints[i].line == line) {
            m_breakpoints.remove(i);
            break;
        }
    const QString key = bpKey(file, line);
    if (m_bkptNumbers.contains(key) && m_gdb
        && m_gdb->state() == QProcess::Running)
        sendMi(QStringLiteral("-break-delete %1").arg(m_bkptNumbers.value(key)));
    m_bkptNumbers.remove(key);
    emit breakpointsChanged();
}

void Debugger::setBreakpointEnabled(const QString &file, int line, bool enabled)
{
    bool found = false;
    for (Breakpoint &b : m_breakpoints)
        if (b.file == file && b.line == line) { b.enabled = enabled; found = true; break; }
    if (!found) return;
    const QString key = bpKey(file, line);
    if (m_bkptNumbers.contains(key) && m_gdb && m_gdb->state() == QProcess::Running)
        sendMi(QStringLiteral("-break-%1 %2")
                   .arg(enabled ? "enable" : "disable").arg(m_bkptNumbers.value(key)));
    emit breakpointsChanged();
}

QList<int> Debugger::breakpointLines(const QString &file) const
{
    QList<int> lines;
    for (const Breakpoint &b : m_breakpoints)
        if (b.file == file) lines << b.line;
    return lines;
}

void Debugger::sendMi(const QString &cmd)
{
    if (!m_gdb || m_gdb->state() != QProcess::Running) return;
    emit output(QStringLiteral("(gdb) %1\n").arg(cmd));
    m_gdb->write((cmd + "\n").toUtf8());
}

void Debugger::sendMiToken(int token, const QString &cmd)
{
    if (!m_gdb || m_gdb->state() != QProcess::Running) return;
    const QString full = QString::number(token) + cmd;
    emit output(QStringLiteral("(gdb) %1\n").arg(full));
    m_gdb->write((full + "\n").toUtf8());
}

void Debugger::sendRaw(const QString &gdbCommand)
{
    // Wrap a plain gdb command so MI returns its console output.
    sendMi(QStringLiteral("-interpreter-exec console \"%1\"")
               .arg(QString(gdbCommand).replace('"', "\\\"")));
}

void Debugger::requestFrameState()
{
    if (!m_gdb || m_gdb->state() != QProcess::Running) return;
    m_threadsToken = ++m_token;
    sendMiToken(m_threadsToken, "-thread-info");
    m_stackToken = ++m_token;
    sendMiToken(m_stackToken, "-stack-list-frames");
    m_localsToken = ++m_token;
    sendMiToken(m_localsToken, "-stack-list-variables --all-values");
}

void Debugger::selectFrame(int level)
{
    if (!m_gdb || m_gdb->state() != QProcess::Running) return;
    sendMi(QStringLiteral("-stack-select-frame %1").arg(level));
    m_localsToken = ++m_token;        // locals are frame-relative
    sendMiToken(m_localsToken, "-stack-list-variables --all-values");
}

void Debugger::pause()
{
    // Sync (all-stop) mode: MI -exec-interrupt needs async (which breaks this
    // ESP target). Interrupt like Ctrl+C instead — SIGINT to gdb halts the
    // inferior and yields a *stopped event.
    if (m_gdb && m_gdb->state() == QProcess::Running && m_state == Running) {
        emit output("[debug] pause (SIGINT)\n");
        ::kill(static_cast<pid_t>(m_gdb->processId()), SIGINT);
    }
}

void Debugger::selectThread(int id)
{
    if (!m_gdb || m_gdb->state() != QProcess::Running) return;
    sendMi(QStringLiteral("-thread-select %1").arg(id));
    requestFrameState();
}

void Debugger::evaluate(const QString &name, const QString &expr)
{
    if (!m_gdb || m_gdb->state() != QProcess::Running) {
        emit watchEvaluated(name, tr("<not running>"), false);
        return;
    }
    const int tok = ++m_token;
    m_evalTokens.insert(tok, name);
    QString e = expr;
    e.replace('\\', "\\\\").replace('"', "\\\"");
    sendMiToken(tok, QStringLiteral("-data-evaluate-expression \"%1\"").arg(e));
}

void Debugger::cont()     { sendMi("-exec-continue"); }
void Debugger::stepOver() { sendMi("-exec-next"); }
void Debugger::stepInto() { sendMi("-exec-step"); }

void Debugger::restart()
{
    sendMi("-interpreter-exec console \"monitor reset halt\"");
    armEntryBreakpoint();
    sendMi("-exec-continue");
}

void Debugger::stop()
{
    if (m_gdb) {
        if (m_gdb->state() == QProcess::Running) {
            m_gdb->write("-gdb-exit\n");
            if (!m_gdb->waitForFinished(1500)) m_gdb->kill();
        }
        m_gdb->deleteLater();
        m_gdb = nullptr;
    }
    if (m_server) {
        m_server->terminate();
        if (!m_server->waitForFinished(1500)) m_server->kill();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_gdbBuf.clear();
    m_bkptNumbers.clear();    // numbers are per-session; user set persists
    m_bkptTokens.clear();
    m_evalTokens.clear();
    m_localsToken = -1;
    m_threadsToken = -1;
    m_stackToken = -1;
    m_attach = false;
    m_pendingAttachJump = false;
    setState(Idle);
}

void Debugger::onGdbStderr()
{
    if (m_gdb)
        emit output(QString::fromLocal8Bit(m_gdb->readAllStandardError()));
}

void Debugger::onGdbFinished(int exitCode)
{
    emit output(QStringLiteral("[debug] gdb exited (%1)\n").arg(exitCode));
    stop();
}

void Debugger::onGdbStdout()
{
    if (!m_gdb) return;
    m_gdbBuf += m_gdb->readAllStandardOutput();
    int nl;
    while ((nl = m_gdbBuf.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_gdbBuf.left(nl)).trimmed();
        m_gdbBuf.remove(0, nl + 1);
        if (!line.isEmpty()) handleLine(line);
    }
}

// Pragmatic GDB/MI parse: surface stream output and detect run/stop state.
void Debugger::handleLine(const QString &line)
{
    const QChar c0 = line[0];

    // Stream records: ~ console, @ target, & log -> show their text.
    if (c0 == '~' || c0 == '@' || c0 == '&') {
        emit output(miUnquote(line.mid(1)));
        return;
    }

    // Token-tagged result for a -break-insert (breakpoint resolution).
    if (c0.isDigit()) {
        if (auto td = QRegularExpression("^(\\d+)\\^(done|error)").match(line); td.hasMatch()) {
            const int tok = td.captured(1).toInt();
            const bool ok = td.captured(2) == "done";
            if (m_bkptTokens.contains(tok)) {
                const Breakpoint req = m_bkptTokens.take(tok);
                if (ok) {
                    int number = -1, actLine = req.line;
                    if (auto n = QRegularExpression("number=\"(\\d+)\"").match(line); n.hasMatch())
                        number = n.captured(1).toInt();
                    if (auto l = QRegularExpression("\\bline=\"(\\d+)\"").match(line); l.hasMatch())
                        actLine = l.captured(1).toInt();
                    // Track by the resolved line so a later delete targets it.
                    for (Breakpoint &b : m_breakpoints)
                        if (b.file == req.file && b.line == req.line) { b.line = actLine; break; }
                    if (number >= 0) {
                        m_bkptNumbers.insert(bpKey(req.file, actLine), number);
                        if (!req.enabled)   // honour a disabled bp that just bound
                            sendMi(QStringLiteral("-break-disable %1").arg(number));
                    }
                    if (actLine != req.line) {
                        emit breakpointResolved(req.file, req.line, actLine);
                        emit breakpointsChanged();
                    }
                } else {
                    QString msg = line;
                    if (auto m = QRegularExpression("msg=\"(.*)\"").match(line); m.hasMatch())
                        msg = miUnquote("\"" + m.captured(1) + "\"");
                    for (int i = 0; i < m_breakpoints.size(); ++i)
                        if (m_breakpoints[i].file == req.file && m_breakpoints[i].line == req.line) {
                            m_breakpoints.remove(i); break;
                        }
                    emit breakpointRejected(req.file, req.line, msg);
                    emit breakpointsChanged();
                }
                return;
            }
            // Locals query result.
            if (tok == m_localsToken) {
                m_localsToken = -1;
                if (ok) {
                    QVector<Variable> vars;
                    QRegularExpression re(
                        "name=\"((?:[^\"\\\\]|\\\\.)*)\",value=\"((?:[^\"\\\\]|\\\\.)*)\"");
                    auto it = re.globalMatch(line);
                    while (it.hasNext()) {
                        const auto m = it.next();
                        vars.push_back({miUnquote("\"" + m.captured(1) + "\""),
                                        miUnquote("\"" + m.captured(2) + "\"")});
                    }
                    emit localsUpdated(vars);
                }
                return;
            }
            // Thread list result.
            if (tok == m_threadsToken) {
                m_threadsToken = -1;
                emit output(QStringLiteral("[debug] thread-info: %1\n").arg(line));
                if (ok) {
                    QVector<ThreadInfo> threads;
                    int current = -1;
                    if (auto c = QRegularExpression("current-thread-id=\"(\\d+)\"").match(line);
                        c.hasMatch())
                        current = c.captured(1).toInt();
                    const int arr = line.indexOf("threads=[");
                    if (arr >= 0) {
                        auto cap = [](const QString &p, const QString &re) {
                            auto m = QRegularExpression(re).match(p);
                            return m.hasMatch() ? m.captured(1) : QString();
                        };
                        // Split on each thread's leading "{id=\"" — frame braces
                        // never contain that token, so the split is safe.
                        const QStringList parts =
                            line.mid(arr + 9).split("{id=\"", Qt::SkipEmptyParts);
                        for (const QString &p : parts) {
                            ThreadInfo ti;
                            const QString idStr = cap(p, "^(\\d+)\"");
                            if (idStr.isEmpty()) continue;
                            ti.id = idStr.toInt();
                            ti.targetId = miUnquote("\"" + cap(p, "target-id=\"((?:[^\"\\\\]|\\\\.)*)\"") + "\"");
                            ti.name = miUnquote("\"" + cap(p, "\\bname=\"((?:[^\"\\\\]|\\\\.)*)\"") + "\"");
                            ti.func = miUnquote("\"" + cap(p, "func=\"((?:[^\"\\\\]|\\\\.)*)\"") + "\"");
                            ti.file = miUnquote("\"" + cap(p, "fullname=\"((?:[^\"\\\\]|\\\\.)*)\"") + "\"");
                            if (ti.file.isEmpty())
                                ti.file = cap(p, "file=\"((?:[^\"\\\\]|\\\\.)*)\"");
                            const QString ln = cap(p, "\\bline=\"(\\d+)\"");
                            if (!ln.isEmpty()) ti.line = ln.toInt();
                            threads.push_back(ti);
                        }
                    }
                    emit output(QStringLiteral("[debug] parsed %1 thread(s), current %2\n")
                                    .arg(threads.size()).arg(current));
                    emit threadsUpdated(threads, current);
                    // On a fresh attach there's no *stopped event; drive the
                    // editor to the current thread's halt location once.
                    if (m_pendingAttachJump) {
                        m_pendingAttachJump = false;
                        for (const ThreadInfo &t : threads)
                            if (t.id == current && !t.file.isEmpty() && t.line > 0) {
                                emit stoppedAt(t.file, t.line, "halted");
                                break;
                            }
                    }
                }
                return;
            }
            // Call-stack result.
            if (tok == m_stackToken) {
                m_stackToken = -1;
                if (ok) {
                    auto cap = [](const QString &p, const QString &re) {
                        auto m = QRegularExpression(re).match(p);
                        return m.hasMatch() ? m.captured(1) : QString();
                    };
                    QVector<Frame> frames;
                    const QStringList parts = line.split("frame={");
                    for (int i = 1; i < parts.size(); ++i) {
                        const QString &p = parts[i];
                        Frame f;
                        f.level = cap(p, "level=\"(\\d+)\"").toInt();
                        f.func = miUnquote("\"" + cap(p, "func=\"((?:[^\"\\\\]|\\\\.)*)\"") + "\"");
                        f.file = miUnquote("\"" + cap(p, "fullname=\"((?:[^\"\\\\]|\\\\.)*)\"") + "\"");
                        if (f.file.isEmpty())
                            f.file = cap(p, "file=\"((?:[^\"\\\\]|\\\\.)*)\"");
                        const QString ln = cap(p, "\\bline=\"(\\d+)\"");
                        if (!ln.isEmpty()) f.line = ln.toInt();
                        frames.push_back(f);
                    }
                    emit stackUpdated(frames, 0);
                }
                return;
            }
            // Watch expression evaluation result.
            if (m_evalTokens.contains(tok)) {
                const QString name = m_evalTokens.take(tok);
                QString text;
                const char *field = ok ? "value" : "msg";
                if (auto m = QRegularExpression(
                        QStringLiteral("%1=\"((?:[^\"\\\\]|\\\\.)*)\"").arg(field)).match(line);
                    m.hasMatch())
                    text = miUnquote("\"" + m.captured(1) + "\"");
                emit watchEvaluated(name, text, ok);
                return;
            }
            return;
        }
    }

    if (line.startsWith("*running")) { setState(Running); return; }

    if (line.startsWith("*stopped")) {
        setState(Stopped);
        QString reason;
        if (auto m = QRegularExpression("reason=\"([^\"]*)\"").match(line); m.hasMatch())
            reason = m.captured(1);

        QString fullpath;
        if (auto m = QRegularExpression("fullname=\"([^\"]*)\"").match(line); m.hasMatch())
            fullpath = miUnquote("\"" + m.captured(1) + "\"");
        else if (auto m2 = QRegularExpression("file=\"([^\"]*)\"").match(line); m2.hasMatch())
            fullpath = m2.captured(1);

        int lineNo = -1;
        if (auto m = QRegularExpression("line=\"(\\d+)\"").match(line); m.hasMatch())
            lineNo = m.captured(1).toInt();

        emit output(QStringLiteral("[debug] stopped: %1 (%2:%3)\n")
                        .arg(reason, fullpath).arg(lineNo));
        if (!fullpath.isEmpty() && lineNo > 0)
            emit stoppedAt(fullpath, lineNo, reason);

        // Refresh threads + locals for the (new) current frame.
        requestFrameState();
        return;
    }

    if (line.startsWith("^error")) {
        QString msg;
        if (auto m = QRegularExpression("msg=\"(.*)\"").match(line); m.hasMatch())
            msg = miUnquote("\"" + m.captured(1) + "\"");
        emit output(QStringLiteral("[debug] error: %1\n").arg(msg));
        return;
    }

    if (line.startsWith("^connected"))
        emit output(tr("[debug] connected to target\n"));
}
