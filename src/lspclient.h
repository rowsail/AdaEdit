#pragma once
#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <functional>

class QProcess;

// Minimal LSP client over stdio (Content-Length framing, JSON-RPC) for the Ada
// Language Server: initialize handshake, document sync, definition + hover.
class LspClient : public QObject
{
    Q_OBJECT
public:
    explicit LspClient(QObject *parent = nullptr);
    ~LspClient() override;

    void start(const QString &serverPath, const QString &rootPath, const QString &projectFile);
    bool isRunning() const;

    void didOpen(const QString &path, const QString &text);
    void didChange(const QString &path, const QString &text);

    struct Diagnostic {
        int startLine = 0, startChar = 0, endLine = 0, endChar = 0;
        int severity = 1;            // 1=error 2=warning 3=info 4=hint
        QString message;
    };

    // A single LSP TextEdit: replace [start,end) with newText.
    struct TextEdit {
        int startLine = 0, startChar = 0, endLine = 0, endChar = 0;
        QString newText;
    };

    // A WorkspaceEdit: edits grouped by document URI (rename spans many files).
    using WorkspaceEdit = QHash<QString, QList<TextEdit>>;

    // A textDocument/codeAction result entry (a refactoring or quick-fix). It
    // carries either an inline `edit` to apply, or a `command` to execute (the
    // server then sends workspace/applyEdit), or both.
    struct CodeAction {
        QString title;
        QString kind;
        bool hasEdit = false;
        WorkspaceEdit edit;
        QString command;            // command id, if any
        QJsonArray arguments;       // command arguments
    };

    using DefCallback = std::function<void(const QString &uri, int line, int character)>;
    using HoverCallback = std::function<void(const QString &text)>;
    using CompletionCallback = std::function<void(const QStringList &items)>;
    using FormatCallback = std::function<void(const QList<TextEdit> &edits)>;
    using RenameCallback = std::function<void(const WorkspaceEdit &edits, const QString &error)>;
    using CodeActionCallback = std::function<void(const QList<CodeAction> &actions)>;
    void definition(const QString &path, int line, int character, DefCallback cb);
    void hover(const QString &path, int line, int character, HoverCallback cb);
    void completion(const QString &path, int line, int character, CompletionCallback cb);
    void formatting(const QString &path, int tabSize, bool insertSpaces, FormatCallback cb);
    void rangeFormatting(const QString &path, int startLine, int startChar,
                         int endLine, int endChar, int tabSize, bool insertSpaces,
                         FormatCallback cb);
    void rename(const QString &path, int line, int character,
                const QString &newName, RenameCallback cb);
    void codeAction(const QString &path, int startLine, int startChar,
                    int endLine, int endChar,
                    const QVector<Diagnostic> &diags, CodeActionCallback cb);
    void executeCommand(const QString &command, const QJsonArray &arguments);

    static QString pathToUri(const QString &path);
    static QString uriToPath(const QString &uri);

signals:
    void ready();
    void log(const QString &message);
    void diagnosticsPublished(const QString &path, const QVector<LspClient::Diagnostic> &diags);
    // The server asked us to apply an edit (workspace/applyEdit) -- e.g. the result
    // of a command-based refactoring kicked off by executeCommand().
    void applyEditRequested(const LspClient::WorkspaceEdit &edits);

private slots:
    void onStdout();
    void onStderr();

private:
    int sendRequest(const QString &method, const QJsonObject &params,
                    std::function<void(const QJsonValue &)> cb, bool force = false);
    void sendNotification(const QString &method, const QJsonObject &params, bool force = false);
    void writeOrQueue(const QJsonObject &msg, bool force);
    void writeMessage(const QJsonObject &msg);
    void handleMessage(const QJsonObject &msg);
    QJsonObject docId(const QString &path) const;
    QJsonObject position(int line, int character) const;

    QProcess *m_proc = nullptr;
    QByteArray m_buf;
    int m_nextId = 1;
    bool m_ready = false;
    QHash<QString, int> m_versions;     // abs path -> document version
    QSet<QString> m_open;               // abs paths opened with the server
    QHash<int, std::function<void(const QJsonValue &)>> m_pending;
    QList<QJsonObject> m_queue;         // messages held until 'initialized'
};
