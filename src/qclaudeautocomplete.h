#pragma once

#include <QCache>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>

namespace QClaude::Internal {

// One-shot Claude completion: spawns a `claude -p` subprocess per request
// (independent of the chat session), captures stdout, strips code fences,
// and emits the result. Designed for inline editor autocomplete.
//
// Speed strategy: a single pre-warmed `claude -p` worker is kept hot and
// blocked on stdin. When a request arrives, we promote that warm worker to
// the active request (skipping ~500ms–2s of Node + CLI cold start), feed it
// the prompt, and spawn the next warm worker in the background while waiting
// for the result. The first request after launch / model change still pays
// cold start; everything after lands in ~300ms TT-first-byte territory.
class AutocompleteEngine : public QObject
{
    Q_OBJECT

public:
    explicit AutocompleteEngine(QObject *parent = nullptr);
    ~AutocompleteEngine() override;

    void setExecutable(const QString &exe);
    void setModel(const QString &m);

    bool isRunning() const { return m_active != nullptr; }

    void cancel();
    void clearCache() { m_cache.clear(); }

    // Pre-spawn a `claude -p` worker so the next request skips cold start.
    // Safe to call repeatedly — no-op if a warm worker already exists.
    // Typically wired to "AI Complete" being toggled on.
    void prewarm();

    // The engine treats `prefix` as everything before the caret and `suffix`
    // as everything after, in the same file. The completion is supposed to be
    // inserted between them.
    void requestCompletion(const QString &filePath,
                           const QString &prefix,
                           const QString &suffix);

signals:
    // Incremental text chunk as the model streams. The panel grows the
    // ghost-text span on each chunk so the user sees the first tokens within
    // ~300ms of triggering, instead of waiting for the full result.
    void partial(const QString &chunk);

    // Final cleaned text (fences stripped, trailing newlines trimmed).
    // Always fires once per request, even when the result was streamed via
    // `partial` — callers should treat this as the canonical end-of-stream.
    void completion(const QString &text);
    void failed(const QString &message);

    // Token usage reported by the `result` event, so the panel's usage bar
    // can fold AI Complete activity into its tooltip.
    void usageReport(int contextTokens, int outputTokens, double costUsd);

private:
    void onActiveFinished(int exitCode, QProcess::ExitStatus status);
    void onActiveErrorOccurred(QProcess::ProcessError error);
    void onActiveReadyReadStdout();
    void discardWarm();

    QStringList buildArgs() const;
    QProcess *spawnProcess();

    QString cacheKey(const QString &lang,
                     const QString &prefix,
                     const QString &suffix) const;

    QString m_exe;
    QString m_model;

    QPointer<QProcess> m_active;  // current in-flight request, or null
    QPointer<QProcess> m_warm;    // pre-spawned, blocked on stdin, or null

    // LRU of completions keyed by lang+model+prefix+suffix. The model's
    // output for the same context is stable enough that re-asking is just
    // wasted latency — backspace/retype loops at the same caret position
    // are the common-case win.
    QCache<QString, QString> m_cache;
    QString m_pendingKey; // key for the in-flight request

    // Streaming state for the active request. `m_outBuffer` line-buffers
    // stream-json events from stdout; `m_streamedText` accumulates emitted
    // deltas; `m_finalResultText` holds the result event's `result` field
    // when the CLI sends one (preferred over the delta concatenation since
    // the CLI sometimes post-processes whitespace).
    QByteArray m_outBuffer;
    QString    m_streamedText;
    QString    m_finalResultText;
};

} // namespace QClaude::Internal
