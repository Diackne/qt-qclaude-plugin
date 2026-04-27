#include "qclaudeautocomplete.h"

#include "claudeprocess.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace QClaude::Internal {

AutocompleteEngine::AutocompleteEngine(QObject *parent)
    : QObject(parent)
{
    m_exe = ClaudeProcess::findClaudeExecutable();
    m_cache.setMaxCost(64);
}

AutocompleteEngine::~AutocompleteEngine()
{
    cancel();
    discardWarm();
}

void AutocompleteEngine::setExecutable(const QString &exe)
{
    if (exe.isEmpty() || m_exe == exe)
        return;
    m_exe = exe;
    // Stale binary path → drop the warm worker so the next request spawns
    // against the new executable.
    discardWarm();
}

void AutocompleteEngine::setModel(const QString &m)
{
    if (m_model == m)
        return;
    m_model = m;
    // Model swap → cached outputs no longer reflect what the engine will
    // produce, and the warm worker was started with the old --model flag.
    m_cache.clear();
    discardWarm();
}

void AutocompleteEngine::cancel()
{
    if (m_active) {
        QProcess *p = m_active;
        m_active = nullptr;
        p->disconnect(this);
        p->kill();
        p->waitForFinished(1500);
        p->deleteLater();
    }
    m_pendingKey.clear();
    m_outBuffer.clear();
    m_streamedText.clear();
    m_finalResultText.clear();
}

void AutocompleteEngine::discardWarm()
{
    if (!m_warm)
        return;
    QProcess *p = m_warm;
    m_warm = nullptr;
    p->disconnect(this);
    if (p->state() != QProcess::NotRunning) {
        p->kill();
        p->waitForFinished(500);
    }
    p->deleteLater();
}

QStringList AutocompleteEngine::buildArgs() const
{
    QStringList args;
    // stream-json + --include-partial-messages so we get per-token text_delta
    // events on stdout. Lets the panel grow ghost text as the model types
    // instead of waiting for the full result. Same flags the chat process
    // uses, so behavior matches.
    args << QStringLiteral("-p")
         << QStringLiteral("--output-format") << QStringLiteral("stream-json")
         << QStringLiteral("--verbose")
         << QStringLiteral("--include-partial-messages");
    if (!m_model.isEmpty())
        args << QStringLiteral("--model") << m_model;
    args << QStringLiteral("--permission-mode") << QStringLiteral("bypassPermissions");
    args << QStringLiteral("--max-turns") << QStringLiteral("1");
    args << QStringLiteral("--disallowed-tools")
         << QStringLiteral("Bash,Edit,Write,Read,Glob,Grep,WebFetch,"
                           "WebSearch,Task,NotebookEdit");
    return args;
}

QProcess *AutocompleteEngine::spawnProcess()
{
    auto *p = new QProcess(this);
    // Use lambdas that capture the QProcess identity so we can dispatch
    // signals to the right path (active vs warm) without sender() guessing.
    connect(p, &QProcess::finished, this,
            [this, p](int code, QProcess::ExitStatus st) {
        if (p == m_active) {
            onActiveFinished(code, st);
        } else if (p == m_warm) {
            // Warm worker exited before we could use it (auth failure, OOM,
            // etc.). Drop it; a new one is spawned on the next request.
            QProcess *w = m_warm;
            m_warm = nullptr;
            w->deleteLater();
        }
    });
    connect(p, &QProcess::errorOccurred, this,
            [this, p](QProcess::ProcessError err) {
        if (p == m_active) {
            onActiveErrorOccurred(err);
        } else if (p == m_warm && err == QProcess::FailedToStart) {
            QProcess *w = m_warm;
            m_warm = nullptr;
            w->deleteLater();
        }
    });
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p]() {
        if (p == m_active)
            onActiveReadyReadStdout();
        // Warm worker shouldn't produce stdout (it's blocked on stdin).
        // If it does, we drain it on promotion via readAllStandardOutput().
    });
    p->start(m_exe, buildArgs());
    return p;
}

void AutocompleteEngine::prewarm()
{
    if (m_warm)
        return;
    if (m_exe.isEmpty())
        return;
    m_warm = spawnProcess();
}

QString AutocompleteEngine::cacheKey(const QString &lang,
                                     const QString &prefix,
                                     const QString &suffix) const
{
    static const QString sep = QStringLiteral("\x1F");
    return lang + sep + m_model + sep + prefix + sep + suffix;
}

void AutocompleteEngine::requestCompletion(const QString &filePath,
                                           const QString &prefix,
                                           const QString &suffix)
{
    if (isRunning())
        cancel();

    const QString lang = QFileInfo(filePath).suffix();

    // Cache lookup: identical (lang, model, prefix, suffix) → reuse the
    // earlier answer instead of paying ~1–3s of subprocess + model latency.
    const QString key = cacheKey(lang, prefix, suffix);
    if (const QString *cached = m_cache.object(key)) {
        const QString text = *cached;
        // Emit on the next event-loop tick so callers see the same async
        // shape they'd get from a real subprocess request.
        QMetaObject::invokeMethod(this, [this, text]() {
            emit completion(text);
        }, Qt::QueuedConnection);
        return;
    }

    m_pendingKey = key;
    m_outBuffer.clear();
    m_streamedText.clear();
    m_finalResultText.clear();

    // Tighter prompt — fewer tokens means faster time-to-first-byte and a
    // smaller surface for the model to wander into prose.
    const QString prompt = QStringLiteral(
        "Inline code completion. Output the code to insert at <CURSOR>, "
        "nothing else. No fences, no prose. Stop after 1–5 lines.\n"
        "Lang: %1\n"
        "%2<CURSOR>%3\n")
        .arg(lang, prefix, suffix);

    // Prefer the warm worker if we have one. It may still be in Starting
    // state — Qt buffers writes until the process is ready, but we wait
    // briefly so a hard start failure surfaces as `failed()` rather than
    // a silent stall.
    if (m_warm && m_warm->state() != QProcess::NotRunning) {
        m_active = m_warm;
        m_warm = nullptr;
        // Drain any startup chatter the warm worker may have written so the
        // first event we parse belongs to *this* request, not pre-prompt
        // banners.
        m_active->readAllStandardOutput();
    } else {
        if (m_warm) {
            // Warm worker exists but already exited (state NotRunning).
            // Reap it before going cold.
            QProcess *dead = m_warm;
            m_warm = nullptr;
            dead->deleteLater();
        }
        m_active = spawnProcess();
    }

    if (m_active->state() == QProcess::Starting
        && !m_active->waitForStarted(2000)) {
        const QString errText = m_active->errorString();
        QProcess *p = m_active;
        m_active = nullptr;
        p->disconnect(this);
        p->kill();
        p->deleteLater();
        emit failed(tr("Could not start claude for autocomplete: %1").arg(errText));
        return;
    }

    m_active->write(prompt.toUtf8());
    m_active->closeWriteChannel();
}

void AutocompleteEngine::onActiveReadyReadStdout()
{
    if (!m_active)
        return;
    m_outBuffer += m_active->readAllStandardOutput();

    while (true) {
        const int nl = m_outBuffer.indexOf('\n');
        if (nl < 0)
            break;
        const QByteArray line = m_outBuffer.left(nl).trimmed();
        m_outBuffer.remove(0, nl + 1);
        if (line.isEmpty())
            continue;

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        const QJsonObject obj = doc.object();
        const QString type = obj.value(QStringLiteral("type")).toString();

        if (type == QStringLiteral("stream_event")) {
            const QJsonObject ev = obj.value(QStringLiteral("event")).toObject();
            if (ev.value(QStringLiteral("type")).toString()
                == QStringLiteral("content_block_delta")) {
                const QJsonObject delta = ev.value(QStringLiteral("delta")).toObject();
                if (delta.value(QStringLiteral("type")).toString()
                    == QStringLiteral("text_delta")) {
                    const QString t = delta.value(QStringLiteral("text")).toString();
                    if (!t.isEmpty()) {
                        m_streamedText += t;
                        emit partial(t);
                    }
                }
            }
        } else if (type == QStringLiteral("result")) {
            // Final result event. Capture the post-processed `result` field
            // (preferred over the delta concatenation since the CLI sometimes
            // trims whitespace) and report usage.
            m_finalResultText = obj.value(QStringLiteral("result")).toString();

            const QJsonObject usage = obj.value(QStringLiteral("usage")).toObject();
            const int inputTokens   = usage.value(QStringLiteral("input_tokens")).toInt();
            const int cacheCreation = usage.value(QStringLiteral("cache_creation_input_tokens")).toInt();
            const int cacheRead     = usage.value(QStringLiteral("cache_read_input_tokens")).toInt();
            const int outputTokens  = usage.value(QStringLiteral("output_tokens")).toInt();
            const double cost       = obj.value(QStringLiteral("total_cost_usd")).toDouble(0.0);
            const int contextTokens = inputTokens + cacheCreation + cacheRead;
            if (contextTokens > 0 || outputTokens > 0 || cost > 0.0)
                emit usageReport(contextTokens, outputTokens, cost);
        }
        // Other event types (system init, assistant, etc.) intentionally
        // ignored — only deltas and the result event matter for autocomplete.
    }
}

void AutocompleteEngine::onActiveFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *p = m_active;

    // Flush whatever's left in the pipe before tearing down.
    onActiveReadyReadStdout();

    m_active = nullptr;
    const QByteArray err = p->readAllStandardError();
    const QString key = m_pendingKey;
    m_pendingKey.clear();

    p->deleteLater();

    // Spawn the next warm worker while we parse — by the time the user types
    // their next pause-trigger, this one is already booted.
    if (!m_warm)
        prewarm();

    if (status != QProcess::NormalExit || exitCode != 0) {
        m_outBuffer.clear();
        m_streamedText.clear();
        m_finalResultText.clear();
        emit failed(tr("claude exited with code %1: %2")
                        .arg(QString::number(exitCode),
                             QString::fromUtf8(err).trimmed()));
        return;
    }

    // Prefer the result event's `result` field (post-processed by the CLI)
    // over our raw delta concatenation. Fall back to streamed text if the
    // CLI never sent a `result` event for some reason.
    QString text = !m_finalResultText.isEmpty() ? m_finalResultText : m_streamedText;
    m_outBuffer.clear();
    m_streamedText.clear();
    m_finalResultText.clear();

    // Strip a single leading/trailing markdown code fence if present.
    if (text.startsWith(QStringLiteral("```"))) {
        const int firstNl = text.indexOf(QLatin1Char('\n'));
        if (firstNl > 0)
            text.remove(0, firstNl + 1);
    }
    if (text.endsWith(QStringLiteral("```\n")))
        text.chop(4);
    else if (text.endsWith(QStringLiteral("```")))
        text.chop(3);

    while (text.endsWith(QLatin1Char('\n')))
        text.chop(1);

    // Cache empty results too — "no suggestion here" is a valid answer and
    // re-asking the same context just burns latency.
    if (!key.isEmpty())
        m_cache.insert(key, new QString(text), 1);

    emit completion(text);
}

void AutocompleteEngine::onActiveErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart)
        emit failed(tr("Could not start '%1' for autocomplete.").arg(m_exe));
}

} // namespace QClaude::Internal
