#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

QT_BEGIN_NAMESPACE
class QLocalServer;
class QLocalSocket;
QT_END_NAMESPACE

namespace QClaude::Internal {

// Plugin-side endpoint for the qclaude_mcp_permission helper. Each
// `claude -p` turn is launched with `--mcp-config <file>` and
// `--permission-prompt-tool mcp__qclaude_perm__permission_prompt`; the
// helper connects here and forwards every permission request — including
// MCP tool calls — that needs the user's Allow/Deny decision.
//
// The server stays running for the lifetime of the panel once it's been
// started, so the same socket name + MCP config file are reused
// turn-to-turn. Stop() drops the server and removes the config file.
class HookServer : public QObject
{
    Q_OBJECT

public:
    explicit HookServer(QObject *parent = nullptr);
    ~HookServer() override;

    // Start listening (idempotent). Pass the path to the
    // qclaude_mcp_permission executable so we can synthesize an MCP
    // config file for `--mcp-config`.
    bool start(const QString &mcpHelperExecutable);
    void stop();

    bool isRunning() const;

    // Path of the generated MCP config file. Empty if not started yet.
    // Pair with `permissionPromptToolName()` when launching claude.
    QString mcpConfigFilePath() const { return m_mcpConfigFilePath; }

    // Fully-qualified prompt-tool name to pass to `--permission-prompt-tool`
    // (`mcp__<server>__<tool>`). Constant for now; exposed as a method so
    // callers don't hard-code the server name.
    static QString permissionPromptToolName();

    // Reply to a previously-emitted permissionRequested. `decision` is
    // "allow" / "deny" / "ask". Safe to call with an unknown id (e.g. if
    // the bridge already disconnected because of a timeout).
    void respond(const QString &id, const QString &decision, const QString &reason);

signals:
    // Fired once per pre-flight tool call. Until `respond()` is called for
    // this id, Claude is paused waiting on the hook subprocess.
    void permissionRequested(const QString &id,
                             const QString &toolName,
                             const QJsonObject &toolInput,
                             const QString &cwd);

    // The bridge for `id` disconnected before we could answer (e.g. user
    // killed claude). Listeners should drop any pending UI for it.
    void requestAbandoned(const QString &id);

private:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

    bool writeMcpConfig(const QString &mcpHelperExecutable);

    QLocalServer *m_server = nullptr;
    QString       m_serverName;          // socket / pipe name
    QString       m_mcpConfigFilePath;   // generated mcp-config.json
    // Map of pending request id → socket. We hold weak pointers because the
    // socket may go away before we respond (Claude killed mid-turn).
    QHash<QString, QPointer<QLocalSocket>> m_pending;
    // Reverse map so onSocketDisconnected can clean up without re-scanning.
    QHash<QLocalSocket *, QString> m_socketIds;
};

} // namespace QClaude::Internal
