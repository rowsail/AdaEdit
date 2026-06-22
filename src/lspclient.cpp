#include "lspclient.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrl>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

// Defined further down; used by handleMessage (workspace/applyEdit) above them.
static QList<LspClient::TextEdit> parseEdits(const QJsonValue &result);
static LspClient::WorkspaceEdit parseWorkspaceEdit(const QJsonValue &result);

LspClient::LspClient(QObject *parent) : QObject(parent) {}

LspClient::~LspClient()
{
    if (m_proc) {
        if (m_proc->state() == QProcess::Running) {
            writeMessage(QJsonObject{{"jsonrpc", "2.0"}, {"id", m_nextId++}, {"method", "shutdown"}});
            writeMessage(QJsonObject{{"jsonrpc", "2.0"}, {"method", "exit"}});
            if (!m_proc->waitForFinished(1500)) m_proc->kill();
        }
    }
}

bool LspClient::isRunning() const
{
    return m_proc && m_proc->state() == QProcess::Running;
}

QString LspClient::pathToUri(const QString &path)
{
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

QString LspClient::uriToPath(const QString &uri)
{
    return QUrl(uri).toLocalFile();
}

void LspClient::start(const QString &serverPath, const QString &rootPath, const QString &projectFile,
                      const QStringList &gprProjectPath)
{
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &LspClient::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, &LspClient::onStderr);
    // Give ALS GPR_PROJECT_PATH so a standalone project's by-name `with`s
    // (with "esp32s3_rts.gpr"; with "esp32s3_hal.gpr";) resolve -- without it ALS
    // can't load the project, so cross-reference (go-to-definition) fails.
    if (!gprProjectPath.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString combined = gprProjectPath.join(':');
        const QString existing = env.value("GPR_PROJECT_PATH");
        if (!existing.isEmpty()) combined += ':' + existing;
        env.insert("GPR_PROJECT_PATH", combined);
        m_proc->setProcessEnvironment(env);
    }
    m_proc->start(serverPath, QStringList{});
    if (!m_proc->waitForStarted(5000)) {
        emit log(tr("Failed to start ada_language_server"));
        return;
    }

    QJsonObject ada;
    if (!projectFile.isEmpty()) ada["projectFile"] = projectFile;
    QJsonObject initOpts;
    initOpts["ada"] = ada;

    // ALS only pushes diagnostics if the client advertises the capability.
    QJsonObject pub;
    pub["relatedInformation"] = true;

    // Completion: declare resolveSupport so ALS defers documentation/detail to a
    // lazy completionItem/resolve. Computing them eagerly crashes ALS on this
    // runtime's `abstract state` decls (gnatdoc PROGRAM_ERROR ADA_ABSTRACT_STATE_DECL).
    QJsonArray resolveProps;
    resolveProps.append("documentation");
    resolveProps.append("detail");
    QJsonObject completionItem;
    completionItem["snippetSupport"] = false;
    completionItem["resolveSupport"] = QJsonObject{{"properties", resolveProps}};
    QJsonObject completion;
    completion["completionItem"] = completionItem;

    QJsonObject tdCaps;
    tdCaps["publishDiagnostics"] = pub;
    tdCaps["completion"] = completion;
    tdCaps["formatting"] = QJsonObject{{"dynamicRegistration", false}};
    tdCaps["rangeFormatting"] = QJsonObject{{"dynamicRegistration", false}};
    tdCaps["rename"] = QJsonObject{{"dynamicRegistration", false}, {"prepareSupport", false}};
    tdCaps["codeAction"] = QJsonObject{
        {"dynamicRegistration", false},
        {"codeActionLiteralSupport", QJsonObject{{"codeActionKind", QJsonObject{
            {"valueSet", QJsonArray{"quickfix", "refactor", "refactor.extract",
                                    "refactor.inline", "refactor.rewrite", "source"}}}}}},
    };
    // Semantic highlighting: ask the server to classify identifiers (type vs
    // variable vs subprogram vs package …) -- richer than the lexer can be.  We
    // declare the standard LSP token legend; ALS replies with the subset it uses.
    tdCaps["semanticTokens"] = QJsonObject{
        {"dynamicRegistration", false},
        {"requests", QJsonObject{{"full", true}}},
        {"formats", QJsonArray{"relative"}},
        {"tokenTypes", QJsonArray{"namespace","type","class","enum","interface","struct",
            "typeParameter","parameter","variable","property","enumMember","event","function",
            "method","macro","keyword","modifier","comment","string","number","regexp",
            "operator","decorator"}},
        {"tokenModifiers", QJsonArray{"declaration","definition","readonly","static","deprecated",
            "abstract","async","modification","documentation","defaultLibrary"}},
    };
    QJsonObject caps;
    caps["textDocument"] = tdCaps;
    // We can apply server-pushed edits and run server commands (command-based
    // refactorings: executeCommand -> the server sends workspace/applyEdit back).
    caps["workspace"] = QJsonObject{
        {"applyEdit", true},
        {"executeCommand", QJsonObject{{"dynamicRegistration", false}}},
    };

    QJsonObject params;
    params["processId"] = double(QCoreApplication::applicationPid());
    params["rootUri"] = pathToUri(rootPath);
    params["capabilities"] = caps;
    params["initializationOptions"] = initOpts;

    emit log(tr("ALS: initialize (project %1)").arg(projectFile.isEmpty() ? "<auto>" : projectFile));
    sendRequest("initialize", params, [this](const QJsonValue &result) {
        // Record the server's semantic-token legend (maps a token's type index to
        // a name); empty if the server doesn't support semantic tokens.
        m_semTokenTypes.clear();
        const QJsonArray tt = result.toObject().value("capabilities").toObject()
            .value("semanticTokensProvider").toObject().value("legend").toObject()
            .value("tokenTypes").toArray();
        for (const QJsonValue &v : tt) m_semTokenTypes << v.toString();

        sendNotification("initialized", QJsonObject{}, true);
        m_ready = true;
        for (const QJsonObject &m : m_queue) writeMessage(m);
        m_queue.clear();
        emit ready();
        emit log(tr("ALS: ready"));
    }, /*force*/ true);
}

// ---- framing -------------------------------------------------------------

void LspClient::writeMessage(const QJsonObject &msg)
{
    if (!m_proc) return;
    const QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    m_proc->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
    m_proc->write(body);
}

void LspClient::writeOrQueue(const QJsonObject &msg, bool force)
{
    if (m_ready || force) writeMessage(msg);
    else m_queue.append(msg);
}

void LspClient::onStderr()
{
    if (m_proc) emit log(QString::fromUtf8(m_proc->readAllStandardError()).trimmed());
}

void LspClient::onStdout()
{
    if (!m_proc) return;
    m_buf += m_proc->readAllStandardOutput();
    forever {
        const int headerEnd = m_buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray header = m_buf.left(headerEnd);
        int length = -1;
        for (const QByteArray &line : header.split('\n')) {
            const QByteArray l = line.trimmed();
            if (l.startsWith("Content-Length:"))
                length = l.mid(15).trimmed().toInt();
        }
        if (length < 0) { m_buf.remove(0, headerEnd + 4); continue; }
        const int bodyStart = headerEnd + 4;
        if (m_buf.size() < bodyStart + length) return;   // wait for the rest
        const QByteArray body = m_buf.mid(bodyStart, length);
        m_buf.remove(0, bodyStart + length);
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) handleMessage(doc.object());
    }
}

void LspClient::handleMessage(const QJsonObject &msg)
{
    if (msg.contains("id") && (msg.contains("result") || msg.contains("error"))) {
        const int id = msg.value("id").toInt();
        auto cb = m_pending.take(id);
        if (cb) {
            if (msg.contains("error"))
                emit log(tr("ALS error: %1").arg(msg.value("error").toObject().value("message").toString()));
            cb(msg.value("result"));
        }
        return;
    }
    if (msg.contains("method")) {
        const QString method = msg.value("method").toString();
        if (method == "textDocument/publishDiagnostics") {
            const QJsonObject p = msg.value("params").toObject();
            const QString path = uriToPath(p.value("uri").toString());
            QVector<Diagnostic> diags;
            for (const QJsonValue &v : p.value("diagnostics").toArray()) {
                const QJsonObject o = v.toObject();
                const QJsonObject r = o.value("range").toObject();
                const QJsonObject s = r.value("start").toObject();
                const QJsonObject e = r.value("end").toObject();
                Diagnostic d;
                d.startLine = s.value("line").toInt();
                d.startChar = s.value("character").toInt();
                d.endLine = e.value("line").toInt();
                d.endChar = e.value("character").toInt();
                d.severity = o.value("severity").toInt(1);
                d.message = o.value("message").toString();
                diags.push_back(d);
            }
            emit diagnosticsPublished(path, diags);
            return;
        }
        if (method == "workspace/applyEdit") {
            // The server (a command-based refactoring) asks us to apply an edit.
            const QJsonObject edit = msg.value("params").toObject().value("edit").toObject();
            emit applyEditRequested(parseWorkspaceEdit(edit));
            if (msg.contains("id"))
                writeMessage(QJsonObject{{"jsonrpc", "2.0"}, {"id", msg.value("id")},
                                         {"result", QJsonObject{{"applied", true}}}});
            return;
        }
        // Other server-initiated requests need a reply so the server doesn't stall.
        if (msg.contains("id"))
            writeMessage(QJsonObject{{"jsonrpc", "2.0"}, {"id", msg.value("id")}, {"result", QJsonValue::Null}});
    }
}

// ---- requests ------------------------------------------------------------

int LspClient::sendRequest(const QString &method, const QJsonObject &params,
                           std::function<void(const QJsonValue &)> cb, bool force)
{
    const int id = m_nextId++;
    m_pending.insert(id, std::move(cb));
    writeOrQueue(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}, force);
    return id;
}

void LspClient::sendNotification(const QString &method, const QJsonObject &params, bool force)
{
    writeOrQueue(QJsonObject{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}, force);
}

QJsonObject LspClient::docId(const QString &path) const
{
    return QJsonObject{{"uri", pathToUri(path)}};
}

QJsonObject LspClient::position(int line, int character) const
{
    return QJsonObject{{"line", line}, {"character", character}};
}

// ---- document sync -------------------------------------------------------

void LspClient::didOpen(const QString &path, const QString &text)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (m_open.contains(abs)) return;
    m_open.insert(abs);
    m_versions[abs] = 1;
    QJsonObject td = docId(path);
    td["languageId"] = "ada";
    td["version"] = 1;
    td["text"] = text;
    sendNotification("textDocument/didOpen", QJsonObject{{"textDocument", td}});
}

void LspClient::didChange(const QString &path, const QString &text)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (!m_open.contains(abs)) { didOpen(path, text); return; }
    const int version = ++m_versions[abs];
    QJsonObject td = docId(path);
    td["version"] = version;
    QJsonArray changes;
    changes.append(QJsonObject{{"text", text}});   // full-document sync
    sendNotification("textDocument/didChange",
                     QJsonObject{{"textDocument", td}, {"contentChanges", changes}});
}

// ---- features ------------------------------------------------------------

void LspClient::definition(const QString &path, int line, int character, DefCallback cb)
{
    QJsonObject params{{"textDocument", docId(path)}, {"position", position(line, character)}};
    sendRequest("textDocument/definition", params, [cb](const QJsonValue &result) {
        QJsonObject loc;
        if (result.isArray()) {
            const QJsonArray a = result.toArray();
            if (a.isEmpty()) { cb(QString(), -1, -1); return; }
            loc = a.first().toObject();
        } else if (result.isObject()) {
            loc = result.toObject();
        } else {
            cb(QString(), -1, -1);
            return;
        }
        const QString uri = loc.contains("uri") ? loc.value("uri").toString()
                                                : loc.value("targetUri").toString();
        const QJsonObject range = loc.contains("range")
            ? loc.value("range").toObject()
            : loc.value("targetSelectionRange").toObject();
        const QJsonObject start = range.value("start").toObject();
        cb(uri, start.value("line").toInt(), start.value("character").toInt());
    });
}

void LspClient::completion(const QString &path, int line, int character, CompletionCallback cb)
{
    QJsonObject params{{"textDocument", docId(path)}, {"position", position(line, character)}};
    sendRequest("textDocument/completion", params, [cb](const QJsonValue &result) {
        QJsonArray items;
        if (result.isArray()) items = result.toArray();
        else if (result.isObject()) items = result.toObject().value("items").toArray();

        static const QRegularExpression idRe("^[A-Za-z_]\\w*");
        QStringList out;
        QSet<QString> seen;
        for (const QJsonValue &v : items) {
            const QJsonObject o = v.toObject();
            QString cand = o.value("insertText").toString();
            if (cand.isEmpty()) cand = o.value("label").toString();
            cand = cand.trimmed();
            const auto m = idRe.match(cand);     // keep just the identifier token
            const QString id = m.hasMatch() ? m.captured(0) : cand;
            if (id.isEmpty() || seen.contains(id)) continue;
            seen.insert(id);
            out << id;
            if (out.size() >= 300) break;
        }
        cb(out);
    });
}

void LspClient::hover(const QString &path, int line, int character, HoverCallback cb)
{
    QJsonObject params{{"textDocument", docId(path)}, {"position", position(line, character)}};
    sendRequest("textDocument/hover", params, [cb](const QJsonValue &result) {
        const QJsonObject o = result.toObject();
        const QJsonValue contents = o.value("contents");
        QString text;
        if (contents.isString()) {
            text = contents.toString();
        } else if (contents.isObject()) {
            text = contents.toObject().value("value").toString();   // MarkupContent
        } else if (contents.isArray()) {
            for (const QJsonValue &v : contents.toArray()) {
                if (v.isString()) text += v.toString() + "\n";
                else if (v.isObject()) text += v.toObject().value("value").toString() + "\n";
            }
        }
        cb(text.trimmed());
    });
}

// Parse a textDocument/formatting result (an array of TextEdit) into our struct.
static QList<LspClient::TextEdit> parseEdits(const QJsonValue &result)
{
    QList<LspClient::TextEdit> edits;
    if (!result.isArray()) return edits;
    for (const QJsonValue &v : result.toArray()) {
        const QJsonObject o = v.toObject();
        const QJsonObject r = o.value("range").toObject();
        const QJsonObject s = r.value("start").toObject();
        const QJsonObject e = r.value("end").toObject();
        LspClient::TextEdit te;
        te.startLine = s.value("line").toInt();
        te.startChar = s.value("character").toInt();
        te.endLine = e.value("line").toInt();
        te.endChar = e.value("character").toInt();
        te.newText = o.value("newText").toString();
        edits.push_back(te);
    }
    return edits;
}

void LspClient::formatting(const QString &path, int tabSize, bool insertSpaces, FormatCallback cb)
{
    QJsonObject options{{"tabSize", tabSize}, {"insertSpaces", insertSpaces}};
    QJsonObject params{{"textDocument", docId(path)}, {"options", options}};
    sendRequest("textDocument/formatting", params,
                [cb](const QJsonValue &result) { cb(parseEdits(result)); });
}

void LspClient::rangeFormatting(const QString &path, int startLine, int startChar,
                                int endLine, int endChar, int tabSize, bool insertSpaces,
                                FormatCallback cb)
{
    QJsonObject range{{"start", position(startLine, startChar)},
                      {"end", position(endLine, endChar)}};
    QJsonObject options{{"tabSize", tabSize}, {"insertSpaces", insertSpaces}};
    QJsonObject params{{"textDocument", docId(path)}, {"range", range}, {"options", options}};
    sendRequest("textDocument/rangeFormatting", params,
                [cb](const QJsonValue &result) { cb(parseEdits(result)); });
}

// A WorkspaceEdit has either `documentChanges` (array of {textDocument, edits})
// or `changes` (object uri -> TextEdit[]).  ALS uses documentChanges for rename.
static LspClient::WorkspaceEdit parseWorkspaceEdit(const QJsonValue &result)
{
    LspClient::WorkspaceEdit out;
    const QJsonObject we = result.toObject();
    if (we.contains("documentChanges")) {
        for (const QJsonValue &v : we.value("documentChanges").toArray()) {
            const QJsonObject dc = v.toObject();
            if (!dc.contains("edits")) continue;       // skip create/rename/delete file ops
            const QString uri = dc.value("textDocument").toObject().value("uri").toString();
            out[uri] += parseEdits(dc.value("edits"));
        }
    } else if (we.contains("changes")) {
        const QJsonObject ch = we.value("changes").toObject();
        for (auto it = ch.begin(); it != ch.end(); ++it)
            out[it.key()] += parseEdits(it.value());
    }
    return out;
}

void LspClient::rename(const QString &path, int line, int character,
                       const QString &newName, RenameCallback cb)
{
    QJsonObject params{{"textDocument", docId(path)},
                       {"position", position(line, character)},
                       {"newName", newName}};
    sendRequest("textDocument/rename", params, [cb](const QJsonValue &result) {
        if (!result.isObject()) { cb(WorkspaceEdit(), tr("rename not available here")); return; }
        const WorkspaceEdit edits = parseWorkspaceEdit(result);
        if (edits.isEmpty()) { cb(edits, tr("no occurrences to rename")); return; }
        cb(edits, QString());
    });
}

void LspClient::codeAction(const QString &path, int startLine, int startChar,
                           int endLine, int endChar,
                           const QVector<Diagnostic> &diags, CodeActionCallback cb)
{
    QJsonArray diagArr;
    for (const Diagnostic &d : diags)
        diagArr.append(QJsonObject{
            {"range", QJsonObject{{"start", position(d.startLine, d.startChar)},
                                  {"end", position(d.endLine, d.endChar)}}},
            {"severity", d.severity}, {"message", d.message}});

    QJsonObject range{{"start", position(startLine, startChar)},
                      {"end", position(endLine, endChar)}};
    QJsonObject params{{"textDocument", docId(path)}, {"range", range},
                       {"context", QJsonObject{{"diagnostics", diagArr}}}};
    sendRequest("textDocument/codeAction", params, [cb](const QJsonValue &result) {
        QList<CodeAction> actions;
        for (const QJsonValue &v : result.toArray()) {
            const QJsonObject o = v.toObject();
            CodeAction a;
            a.title = o.value("title").toString();
            a.kind  = o.value("kind").toString();
            if (o.contains("edit")) { a.hasEdit = true; a.edit = parseWorkspaceEdit(o.value("edit")); }
            // Both CodeAction.command (an object) and the bare Command form (this
            // object IS the command) are possible.
            const QJsonValue cmd = o.contains("command") ? o.value("command") : v;
            if (cmd.isObject() && cmd.toObject().contains("command")) {
                a.command   = cmd.toObject().value("command").toString();
                a.arguments = cmd.toObject().value("arguments").toArray();
            }
            if (!a.title.isEmpty()) actions.push_back(a);
        }
        cb(actions);
    });
}

void LspClient::executeCommand(const QString &command, const QJsonArray &arguments)
{
    sendRequest("workspace/executeCommand",
                QJsonObject{{"command", command}, {"arguments", arguments}},
                [](const QJsonValue &) {});   // effect arrives via workspace/applyEdit
}

void LspClient::semanticTokens(const QString &path, SemanticTokensCallback cb)
{
    if (m_semTokenTypes.isEmpty()) { cb({}); return; }   // server has no legend
    QJsonObject params{{"textDocument", docId(path)}};
    sendRequest("textDocument/semanticTokens/full", params, [this, cb](const QJsonValue &result) {
        // `data` is a flat int array, 5 per token (LSP "relative" encoding):
        // deltaLine, deltaStartChar, length, tokenTypeIndex, tokenModifiers.
        QList<SemToken> out;
        const QJsonArray data = result.toObject().value("data").toArray();
        int line = 0, ch = 0;
        for (int i = 0; i + 5 <= data.size(); i += 5) {
            const int dLine = data[i].toInt();
            const int dCh   = data[i + 1].toInt();
            if (dLine == 0) { ch += dCh; } else { line += dLine; ch = dCh; }
            SemToken t;
            t.line = line; t.startChar = ch; t.length = data[i + 2].toInt();
            const int ix = data[i + 3].toInt();
            if (ix >= 0 && ix < m_semTokenTypes.size()) t.type = m_semTokenTypes[ix];
            out.push_back(t);
        }
        cb(out);
    });
}
