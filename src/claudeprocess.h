#pragma once

#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QJsonValue>

namespace QClaude::Internal {

class ClaudeProcess : public QObject
{
    Q_OBJECT

public:
    explicit ClaudeProcess(QObject *parent = nullptr);
    ~ClaudeProcess() override;

    void setExecutablePath(const QString &path);
    QString executablePath() const { return m_executable; }

    void setWorkingDirectory(const QString &dir);
    QString workingDirectory() const { return m_workdir; }

    void setSessionId(const QString &sid) { m_sessionId = sid; }
    QString sessionId() const { return m_sessionId; }

    void setPermissionMode(const QString &mode) { m_permissionMode = mode; }
    QString permissionMode() const { return m_permissionMode; }

    void setModel(const QString &model) { m_model = model; }
    QString model() const { return m_model; }

    // Optional path to an MCP config file (consumed by claude `--mcp-config`).
    // Pair with `setPermissionPromptTool` to wire the pre-flight permission
    // gate; either left empty disables it for this turn.
    void setMcpConfigFile(const QString &path) { m_mcpConfigFile = path; }
    QString mcpConfigFile() const { return m_mcpConfigFile; }

    // Fully-qualified MCP tool name (`mcp__server__tool`) passed to
    // `--permission-prompt-tool`. Empty = no permission prompt tool.
    void setPermissionPromptTool(const QString &name) { m_permissionPromptTool = name; }
    QString permissionPromptTool() const { return m_permissionPromptTool; }

    bool isRunning() const;

    void send(const QString &prompt);
    void stop();

    static QString findClaudeExecutable();

signals:
    void systemInit(const QString &sessionId, const QString &cwd, const QString &model);
    void assistantText(const QString &text);
    void thinking(const QString &text);
    void toolUse(const QString &toolName, const QString &summary);
    void toolResult(const QString &summary, bool isError);
    void finished(const QString &finalText, double costUsd, qint64 durationMs, bool success);
    void authError(const QString &detail);
    void errorOccurred(const QString &message);

    // Edit / Write tool calls — emitted alongside toolUse so the panel can
    // offer a diff/revert affordance. `oldContent` is empty for Write of a
    // newly created file; we snapshot it from disk on a best-effort basis.
    void editToolApplied(const QString &toolName,
                         const QString &filePath,
                         const QString &oldContent,
                         const QString &newContent,
                         const QString &editOldString,
                         const QString &editNewString);

    // Token-usage report from the latest turn's `result` event. The
    // `contextTokens` total covers everything that counts against the model's
    // context window (this turn's input + previously cached + newly cached).
    void usageReport(int contextTokens, int outputTokens, double costUsd);

    // Rate-limit reset hint, when the CLI surfaces one (e.g. session window
    // for Claude Pro/Max). Epoch seconds, UTC. The panel folds this into the
    // header usage label as "resets HH:MM" when present.
    void rateLimitInfo(qint64 resetEpochSec);

private:
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessErrorOccurred(QProcess::ProcessError error);
    void handleEvent(const QByteArray &json);
    QString summarizeToolInput(const QString &name, const QJsonObject &input) const;
    QString summarizeToolResult(const QJsonValue &content) const;

    QProcess m_proc;
    QString m_executable;
    QString m_workdir;
    QString m_sessionId;
    QString m_permissionMode;
    QString m_model;
    QString m_mcpConfigFile;
    QString m_permissionPromptTool;
    QByteArray m_buffer;
    QByteArray m_stderrBuffer;
    bool m_authErrorEmitted = false;
    bool m_finishedEmitted = false;
    bool m_streamedTextThisTurn = false;
};

} // namespace QClaude::Internal
