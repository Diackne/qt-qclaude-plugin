#include "claudeprocess.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>

namespace QClaude::Internal {

ClaudeProcess::ClaudeProcess(QObject *parent)
    : QObject(parent)
{
    m_executable = findClaudeExecutable();
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);

    connect(&m_proc, &QProcess::readyReadStandardOutput,
            this, &ClaudeProcess::onReadyReadStdout);
    connect(&m_proc, &QProcess::readyReadStandardError,
            this, &ClaudeProcess::onReadyReadStderr);
    connect(&m_proc, &QProcess::finished,
            this, &ClaudeProcess::onProcessFinished);
    connect(&m_proc, &QProcess::errorOccurred,
            this, &ClaudeProcess::onProcessErrorOccurred);
}

ClaudeProcess::~ClaudeProcess()
{
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }
}

QString ClaudeProcess::findClaudeExecutable()
{
    QString p = QStandardPaths::findExecutable("claude");
    if (!p.isEmpty())
        return p;
    const QString home = QDir::homePath();
    const QStringList candidates = {
        home + QStringLiteral("/.local/bin/claude"),
        home + QStringLiteral("/.claude/local/claude"),
        QStringLiteral("/usr/local/bin/claude"),
        QStringLiteral("/usr/bin/claude")
    };
    for (const QString &cand : candidates) {
        if (QFileInfo::exists(cand))
            return cand;
    }
    return QStringLiteral("claude");
}

void ClaudeProcess::setExecutablePath(const QString &path)
{
    if (!path.isEmpty())
        m_executable = path;
}

void ClaudeProcess::setWorkingDirectory(const QString &dir)
{
    m_workdir = dir;
}

bool ClaudeProcess::isRunning() const
{
    return m_proc.state() != QProcess::NotRunning;
}

void ClaudeProcess::send(const QString &prompt)
{
    if (isRunning()) {
        emit errorOccurred(tr("Claude is already running. Stop it first."));
        return;
    }

    m_buffer.clear();
    m_stderrBuffer.clear();
    m_authErrorEmitted = false;
    m_finishedEmitted = false;

    QStringList args;
    args << "-p"
         << "--output-format" << "stream-json"
         << "--verbose"
         << "--include-partial-messages";
    if (!m_sessionId.isEmpty())
        args << "--resume" << m_sessionId;
    if (!m_permissionMode.isEmpty())
        args << "--permission-mode" << m_permissionMode;
    if (!m_model.isEmpty())
        args << "--model" << m_model;
    if (!m_mcpConfigFile.isEmpty())
        args << "--mcp-config" << m_mcpConfigFile;
    if (!m_permissionPromptTool.isEmpty())
        args << "--permission-prompt-tool" << m_permissionPromptTool;

    m_streamedTextThisTurn = false;

    if (!m_workdir.isEmpty())
        m_proc.setWorkingDirectory(m_workdir);

    m_proc.start(m_executable, args);
    if (!m_proc.waitForStarted(5000)) {
        emit errorOccurred(tr("Failed to start '%1': %2")
                               .arg(m_executable, m_proc.errorString()));
        return;
    }
    m_proc.write(prompt.toUtf8());
    m_proc.closeWriteChannel();
}

void ClaudeProcess::stop()
{
    if (m_proc.state() == QProcess::NotRunning)
        return;
    m_proc.terminate();
    if (!m_proc.waitForFinished(2000))
        m_proc.kill();
}

void ClaudeProcess::onReadyReadStdout()
{
    m_buffer += m_proc.readAllStandardOutput();
    while (true) {
        const int nl = m_buffer.indexOf('\n');
        if (nl < 0)
            break;
        QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (!line.isEmpty())
            handleEvent(line);
    }
}

void ClaudeProcess::onReadyReadStderr()
{
    const QByteArray chunk = m_proc.readAllStandardError();
    m_stderrBuffer += chunk;
    const QString text = QString::fromUtf8(m_stderrBuffer).toLower();
    if (!m_authErrorEmitted
        && (text.contains("not authenticated")
            || text.contains("please run /login")
            || text.contains("invalid api key")
            || text.contains("authentication required")
            || text.contains("login required"))) {
        m_authErrorEmitted = true;
        emit authError(QString::fromUtf8(m_stderrBuffer).trimmed());
    }
}

void ClaudeProcess::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_buffer.isEmpty()) {
        for (const QByteArray &line : m_buffer.split('\n')) {
            const QByteArray t = line.trimmed();
            if (!t.isEmpty())
                handleEvent(t);
        }
        m_buffer.clear();
    }

    const bool ok = (status == QProcess::NormalExit && exitCode == 0);
    if (!m_finishedEmitted) {
        m_finishedEmitted = true;
        if (!ok && !m_authErrorEmitted) {
            const QString err = QString::fromUtf8(m_stderrBuffer).trimmed();
            emit errorOccurred(err.isEmpty()
                                   ? tr("claude exited with code %1").arg(exitCode)
                                   : err);
        }
        emit finished(QString(), 0.0, 0, ok);
    }
}

void ClaudeProcess::onProcessErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart)
        emit errorOccurred(tr("Could not start '%1'. Is Claude Code installed and on PATH?")
                               .arg(m_executable));
}

void ClaudeProcess::handleEvent(const QByteArray &json)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();

    // Look for a rate-limit reset hint anywhere in the payload. Field naming
    // varies across CLI versions, so we accept several common shapes.
    auto extractResetEpoch = [](const QJsonObject &o) -> qint64 {
        const QStringList keys = {
            QStringLiteral("rate_limit_reset_at"),
            QStringLiteral("rate_limit_resets_at"),
            QStringLiteral("reset_at"),
            QStringLiteral("resets_at")
        };
        for (const QString &k : keys) {
            const QJsonValue v = o.value(k);
            if (v.isDouble())
                return static_cast<qint64>(v.toDouble());
            if (v.isString()) {
                const QString s = v.toString();
                bool ok = false;
                const qint64 n = s.toLongLong(&ok);
                if (ok)
                    return n;
                const QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
                if (dt.isValid())
                    return dt.toSecsSinceEpoch();
            }
        }
        return 0;
    };

    if (type == "system") {
        if (obj.value("subtype").toString() == "init") {
            const QString sid = obj.value("session_id").toString();
            if (!sid.isEmpty())
                m_sessionId = sid;
            emit systemInit(m_sessionId,
                            obj.value("cwd").toString(),
                            obj.value("model").toString());
        }
        const qint64 reset = extractResetEpoch(obj);
        if (reset > 0)
            emit rateLimitInfo(reset);
        return;
    }

    // --include-partial-messages: each Anthropic streaming event is wrapped
    // as {"type":"stream_event","event":{...}}. We forward text deltas as
    // assistantText() so the panel renders character-by-character.
    if (type == "stream_event") {
        const QJsonObject ev = obj.value("event").toObject();
        const QString evType = ev.value("type").toString();
        if (evType == "content_block_delta") {
            const QJsonObject delta = ev.value("delta").toObject();
            const QString dtype = delta.value("type").toString();
            if (dtype == "text_delta") {
                const QString t = delta.value("text").toString();
                if (!t.isEmpty()) {
                    m_streamedTextThisTurn = true;
                    emit assistantText(t);
                }
            }
            // thinking_delta and input_json_delta intentionally ignored —
            // we render thinking on the consolidated `assistant` event and
            // tool_use input is summarised once it's complete.
        }
        return;
    }

    if (type == "assistant") {
        const QJsonObject msg = obj.value("message").toObject();
        const QJsonArray content = msg.value("content").toArray();
        for (const QJsonValue &v : content) {
            const QJsonObject part = v.toObject();
            const QString partType = part.value("type").toString();
            if (partType == "text") {
                if (m_streamedTextThisTurn)
                    continue; // already streamed via stream_event deltas
                const QString t = part.value("text").toString();
                if (!t.isEmpty())
                    emit assistantText(t);
            } else if (partType == "thinking") {
                const QString t = part.value("thinking").toString();
                if (!t.isEmpty())
                    emit thinking(t);
            } else if (partType == "tool_use") {
                const QString name = part.value("name").toString();
                const QJsonObject input = part.value("input").toObject();
                emit toolUse(name, summarizeToolInput(name, input));

                if (name == "Edit" || name == "Write") {
                    const QString filePath = input.value("file_path").toString();
                    if (!filePath.isEmpty()) {
                        // Best-effort snapshot of pre-tool content from disk.
                        // For Write of a brand-new file this is empty.
                        QString oldContent;
                        QFile f(filePath);
                        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            oldContent = QString::fromUtf8(f.readAll());
                            f.close();
                        }

                        QString newContent;
                        QString editOld;
                        QString editNew;
                        if (name == "Write") {
                            newContent = input.value("content").toString();
                        } else { // Edit
                            editOld = input.value("old_string").toString();
                            editNew = input.value("new_string").toString();
                        }

                        emit editToolApplied(name, filePath, oldContent,
                                             newContent, editOld, editNew);
                    }
                }
            }
        }
        return;
    }

    if (type == "user") {
        const QJsonObject msg = obj.value("message").toObject();
        const QJsonArray content = msg.value("content").toArray();
        for (const QJsonValue &v : content) {
            const QJsonObject part = v.toObject();
            if (part.value("type").toString() == "tool_result") {
                const bool isError = part.value("is_error").toBool(false);
                emit toolResult(summarizeToolResult(part.value("content")), isError);
            }
        }
        return;
    }

    if (type == "result") {
        const QString sid = obj.value("session_id").toString();
        if (!sid.isEmpty())
            m_sessionId = sid;
        const QString finalText = obj.value("result").toString();
        const double cost = obj.value("total_cost_usd").toDouble(0.0);
        const qint64 dur = static_cast<qint64>(obj.value("duration_ms").toDouble(0.0));
        const bool success = obj.value("subtype").toString() == "success";

        // Token usage: with prompt caching, the latest turn's input portions
        // already represent the entire conversation context, so we don't need
        // to accumulate across turns.
        const QJsonObject usage = obj.value("usage").toObject();
        const int inputTokens   = usage.value("input_tokens").toInt();
        const int cacheCreation = usage.value("cache_creation_input_tokens").toInt();
        const int cacheRead     = usage.value("cache_read_input_tokens").toInt();
        const int outputTokens  = usage.value("output_tokens").toInt();
        const int contextTokens = inputTokens + cacheCreation + cacheRead;
        if (contextTokens > 0 || outputTokens > 0)
            emit usageReport(contextTokens, outputTokens, cost);

        const qint64 reset = qMax(extractResetEpoch(obj),
                                  extractResetEpoch(usage));
        if (reset > 0)
            emit rateLimitInfo(reset);

        m_finishedEmitted = true;
        m_streamedTextThisTurn = false;
        emit finished(finalText, cost, dur, success);
        return;
    }
}

QString ClaudeProcess::summarizeToolInput(const QString &name, const QJsonObject &input) const
{
    auto str = [&](const QString &k) { return input.value(k).toString(); };

    if (name == "Read" || name == "Edit" || name == "Write" || name == "NotebookEdit")
        return str("file_path");
    if (name == "Bash") {
        QString cmd = str("command");
        cmd.replace('\n', QChar(0x21B5));
        if (cmd.size() > 160)
            cmd = cmd.left(160) + QStringLiteral("…");
        return cmd;
    }
    if (name == "Glob")  return str("pattern");
    if (name == "Grep")  return str("pattern");
    if (name == "WebFetch" || name == "WebSearch") return str("url") + str("query");
    if (name == "Task") {
        const QString d = str("description");
        return d.isEmpty() ? str("subagent_type") : d;
    }
    return QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)).left(160);
}

QString ClaudeProcess::summarizeToolResult(const QJsonValue &content) const
{
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else if (content.isArray()) {
        for (const QJsonValue &v : content.toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value("type").toString() == "text")
                text += o.value("text").toString();
        }
    }
    text = text.trimmed();
    const int firstNl = text.indexOf('\n');
    QString head = (firstNl >= 0) ? text.left(firstNl) : text;
    if (head.size() > 160)
        head = head.left(160) + QStringLiteral("…");
    return head;
}

} // namespace QClaude::Internal
