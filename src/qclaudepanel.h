#pragma once

#include <QWidget>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QStringList>

#include <utils/plaintextedit/plaintextedit.h>

QT_BEGIN_NAMESPACE
class QPlainTextEdit;
class QProgressBar;
class QTextBrowser;
class QTimer;
class QToolButton;
class QPushButton;
class QLabel;
class QMenu;
class QUrl;
class QJsonObject;
QT_END_NAMESPACE

namespace Utils {
class FilePath;
}

namespace QClaude::Internal {

class ClaudeProcess;
class AutocompleteEngine;
class FilePicker;
class HookServer;

struct ChatHistoryEntry
{
    QString sessionId;
    QString title;
};

struct EditCardEntry
{
    QString tool;        // "Edit" or "Write"
    QString filePath;
    QString oldContent;  // pre-tool snapshot of the file (best-effort)
    QString newContent;  // for Write: the full input "content"
    QString editOld;     // for Edit
    QString editNew;     // for Edit
    bool hadFileBefore = true;
    bool reverted = false;
};

class QClaudePanel : public QWidget
{
    Q_OBJECT

public:
    explicit QClaudePanel(QWidget *parent = nullptr);
    ~QClaudePanel() override;

    // Pre-fill the input with a file reference and optional selection block.
    // `line` is only used for display when there is no selection.
    void askAboutFile(const QString &filePath,
                      const QString &selection = QString(),
                      int startLine = -1,
                      int endLine = -1);

    // Trigger an inline AI completion at the current text editor's caret.
    // No-op when the autocomplete toggle is off or there is no current editor.
    bool triggerAutocompletion();
    bool isAutocompleteEnabled() const;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void onSendClicked();
    void onStopClicked();
    void onNewChatClicked();
    void onLoginClicked();
    void onContextChanged();
    void onAddClicked();
    // Inline `@`-mention fuzzy file picker. The composer watches its own
    // text changes for an unfinished @-token and pops the picker; arrow
    // keys navigate, Tab/Enter inserts the chosen `@<rel-path>`.
    void updateMentionPicker();
    void closeMentionPicker();
    void acceptMentionPick(const Utils::FilePath &filePath);
    void openMentionPickerExplicit();

    // Pre-flight permission gate. Wired to the qclaude_hook_bridge helper
    // via QLocalServer; when the chip is on, every Edit/Write call from
    // claude pauses on a hook and we render a non-modal card with Allow /
    // Deny anchors. Clicking either calls back into HookServer to release
    // the bridge subprocess.
    void onPermissionRequested(const QString &id,
                               const QString &toolName,
                               const QJsonObject &toolInput,
                               const QString &cwd);
    void onPermissionAbandoned(const QString &id);
    void respondToPermission(const QString &id, bool allow);
    void onCommandsClicked();
    void onAskBeforeEditsToggled(bool checked);
    void onHistoryClicked();

    void onSystemInit(const QString &sessionId, const QString &cwd, const QString &model);
    void onAssistantText(const QString &text);
    void onThinking(const QString &text);
    void onToolUse(const QString &name, const QString &summary);
    void onToolResult(const QString &summary, bool isError);
    void onFinished(const QString &finalText, double costUsd, qint64 durationMs, bool success);
    void onAuthError(const QString &detail);
    void onErrorOccurred(const QString &message);
    void onEditToolApplied(const QString &tool,
                           const QString &filePath,
                           const QString &oldContent,
                           const QString &newContent,
                           const QString &editOld,
                           const QString &editNew);
    void onAnchorClicked(const QUrl &url);

    void onActiveEditorChanged();
    void onEditorTextChanged();
    void onAutocompleteTimerFired();
    void refreshUsageTooltip();
    void refreshUsageLabel();

    void insertGhostText(Utils::PlainTextEdit *editor, const QString &text);
    // Append `chunk` to the existing ghost-text span. Used to grow the
    // suggestion as stream-json deltas arrive, so the user sees tokens
    // appear within ~300ms instead of waiting for the full result.
    void growGhostText(const QString &chunk);
    void acceptGhostText();
    void removeGhostText();
    // Drop any in-flight ghost text *and* cancel the autocomplete stream.
    // Called when the user dismisses the suggestion (Escape, keystroke,
    // mouse click) so we don't keep streaming tokens we'll throw away.
    void dismissAutocomplete();
    bool hasGhostText() const { return m_ghostEditor && m_ghostStart >= 0; }

    void setBusy(bool busy);
    void appendBlock(const QString &html);
    void appendUserMessage(const QString &markdown);
    QString welcomeBlock() const;
    void rerenderTrailingAssistant();
    void insertRefsIntoInput(const QStringList &refs,
                             const QString &fencedSelection = QString());
    void updateCurrentFileChip();
    QString currentWorkingDir() const;
    QString currentProjectKey() const;
    void updateContextLabel();
    void rememberCurrentSessionInHistory();
    void persistCurrentSession();
    void resumeSavedSessionForCurrentProject();
    void applyUsageSnapshotForCurrentProject();
    void applyTitle(const QString &title);

    QLabel *m_titleLabel = nullptr;
    QLabel *m_contextLabel = nullptr;
    QTextBrowser *m_view = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QToolButton *m_sendButton = nullptr;
    QToolButton *m_stopButton = nullptr;
    QPushButton *m_loginButton = nullptr;
    QLabel *m_statusLabel = nullptr;

    QProgressBar *m_usageBar = nullptr;
    QLabel *m_usageLabel = nullptr;
    int m_usageMaxContext = 200000;     // approximate cap; refined per model
    int m_lastContextTokens = 0;
    qint64 m_rateLimitResetEpoch = 0;   // 0 = unknown
    // Cumulative AI Complete usage for this panel session.
    int m_acRequestCount = 0;
    int m_acTotalTokens  = 0;
    double m_acTotalCost = 0.0;
    double m_lastChatCost = 0.0;
    QTimer *m_usageRefreshTimer = nullptr;
    QToolButton *m_historyTb = nullptr;
    QToolButton *m_newChatTb = nullptr;
    QToolButton *m_addTb = nullptr;
    QToolButton *m_commandsTb = nullptr;
    QToolButton *m_askEditsTb = nullptr;
    QToolButton *m_currentFileChip = nullptr;
    QToolButton *m_autocompleteTb = nullptr;

    ClaudeProcess *m_proc = nullptr;
    AutocompleteEngine *m_autocomplete = nullptr;
    QString m_pendingCompletionFile;

    FilePicker *m_filePicker = nullptr;
    HookServer *m_hookServer = nullptr;
    // Outstanding pre-flight requests still showing Allow/Deny anchors.
    // Tracked so we can grey the card out when claude/the bridge goes away
    // before the user clicks. Maps request id → tool name (for the abandon
    // log line — we don't need the full input again once the card is up).
    QHash<QString, QString> m_pendingPermissions;
    // Cursor position in the composer just after the active `@` trigger,
    // i.e. the start of the current query. -1 when the picker is closed.
    // We store an absolute position rather than a QTextCursor because the
    // input is small and edits don't move blocks around.
    int m_mentionAnchorPos = -1;

    // Auto-suggest debouncer + ghost-text state.
    QTimer *m_autocompleteTimer = nullptr;
    QPointer<Utils::PlainTextEdit> m_attachedEditor;
    QPointer<Utils::PlainTextEdit> m_ghostEditor;
    int m_ghostStart = -1;
    int m_ghostEnd = -1;
    int m_pendingCaretPos = -1;
    QString m_assistantBuffer;
    QString m_currentTitle;
    bool m_assistantOpen = false;
    bool m_busy = false;
    bool m_askBeforeEdits = true;
    bool m_userInteracted = false;
    QString m_activeProjectKey;

    QList<ChatHistoryEntry> m_history;
    QHash<QString, EditCardEntry> m_edits;
    int m_nextEditId = 1;
};

} // namespace QClaude::Internal
