#include "qclaudesettings.h"

#include "claudeprocess.h"
#include "qclaude_compat.h"
#include "qclaudetr.h"

#include <coreplugin/coreconstants.h>
#include <coreplugin/icore.h>

#include <utils/qtcsettings.h>

#include <QComboBox>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace QClaude::Internal {

namespace {
constexpr char kGroup[] = "QClaude";
constexpr char kKeyExecutable[]          = "ExecutablePath";
constexpr char kKeyDefaultModel[]        = "DefaultModel";
constexpr char kKeyPermissionMode[]      = "DefaultPermissionMode";
constexpr char kKeyAutocompleteModel[]   = "AutocompleteModel";
constexpr char kKeyAutocompleteEnabled[] = "AutocompleteEnabled";
constexpr char kSessionsGroup[]          = "QClaude/Sessions";
constexpr char kUsageGroup[]             = "QClaude/Usage";
constexpr char kUsageContextTokens[]     = "ContextTokens";
constexpr char kUsageMaxContext[]        = "MaxContext";
constexpr char kUsageResetEpoch[]        = "RateLimitResetAt";

QString hashedKey(const QString &projectKey)
{
    // QSettings keys can't contain '/' freely; use a hash for stability.
    return QString::fromLatin1(
        QCryptographicHash::hash(projectKey.toUtf8(), QCryptographicHash::Sha1).toHex());
}
} // namespace

QClaudeSettings &QClaudeSettings::instance()
{
    static QClaudeSettings s;
    return s;
}

QClaudeSettings::QClaudeSettings()
{
    load();
}

void QClaudeSettings::load()
{
    auto *s = Core::ICore::settings();
    s->beginGroup(kGroup);
    m_executablePath        = s->value(kKeyExecutable).toString();
    m_defaultModel          = s->value(kKeyDefaultModel).toString();
    m_defaultPermissionMode = s->value(kKeyPermissionMode,
                                       QStringLiteral("default")).toString();

    // Default the autocomplete model to Haiku 4.5 — fastest of the lineup
    // and the right pick for inline completions. Only the first launch
    // (when the key has never been written) gets this seed; once the user
    // touches the setting their choice persists, including "Use chat model"
    // (empty string).
    const QVariant rawAcm = s->value(kKeyAutocompleteModel);
    bool seededAutocomplete = false;
    if (rawAcm.isValid()) {
        m_autocompleteModel = rawAcm.toString();
    } else {
        m_autocompleteModel = QStringLiteral("claude-haiku-4-5");
        seededAutocomplete = true;
    }
    m_autocompleteEnabled = s->value(kKeyAutocompleteEnabled, false).toBool();
    s->endGroup();

    // Persist the seed so the value shows up in the IOptionsPage and the
    // `/` model submenu without requiring the user to interact first.
    if (seededAutocomplete)
        save();
}

void QClaudeSettings::save() const
{
    auto *s = Core::ICore::settings();
    s->beginGroup(kGroup);
    s->setValue(kKeyExecutable, m_executablePath);
    s->setValue(kKeyDefaultModel, m_defaultModel);
    s->setValue(kKeyPermissionMode, m_defaultPermissionMode);
    s->setValue(kKeyAutocompleteModel, m_autocompleteModel);
    s->setValue(kKeyAutocompleteEnabled, m_autocompleteEnabled);
    s->endGroup();
}

void QClaudeSettings::setExecutablePath(const QString &v)
{
    if (m_executablePath == v)
        return;
    m_executablePath = v;
    save();
    emit changed();
}

void QClaudeSettings::setDefaultModel(const QString &v)
{
    if (m_defaultModel == v)
        return;
    m_defaultModel = v;
    save();
    emit changed();
}

void QClaudeSettings::setDefaultPermissionMode(const QString &v)
{
    if (m_defaultPermissionMode == v)
        return;
    m_defaultPermissionMode = v;
    save();
    emit changed();
}

void QClaudeSettings::setAutocompleteModel(const QString &v)
{
    if (m_autocompleteModel == v)
        return;
    m_autocompleteModel = v;
    save();
    emit changed();
}

void QClaudeSettings::setAutocompleteEnabled(bool v)
{
    if (m_autocompleteEnabled == v)
        return;
    m_autocompleteEnabled = v;
    save();
    emit changed();
}

QString QClaudeSettings::resolvedExecutable() const
{
    if (!m_executablePath.isEmpty())
        return m_executablePath;
    return ClaudeProcess::findClaudeExecutable();
}

QString QClaudeSettings::sessionForProject(const QString &projectKey) const
{
    if (projectKey.isEmpty())
        return {};
    auto *s = Core::ICore::settings();
    s->beginGroup(kSessionsGroup);
    const QString v = s->value(hashedKey(projectKey).toLatin1()).toString();
    s->endGroup();
    return v;
}

void QClaudeSettings::setSessionForProject(const QString &projectKey, const QString &sessionId)
{
    if (projectKey.isEmpty() || sessionId.isEmpty())
        return;
    auto *s = Core::ICore::settings();
    s->beginGroup(kSessionsGroup);
    s->setValue(hashedKey(projectKey).toLatin1(), sessionId);
    s->endGroup();
}

void QClaudeSettings::clearSessionForProject(const QString &projectKey)
{
    if (projectKey.isEmpty())
        return;
    auto *s = Core::ICore::settings();
    s->beginGroup(kSessionsGroup);
    s->remove(hashedKey(projectKey).toLatin1());
    s->endGroup();
}

QClaudeSettings::UsageSnapshot
QClaudeSettings::lastUsageForProject(const QString &projectKey) const
{
    UsageSnapshot snap;
    if (projectKey.isEmpty())
        return snap;
    auto *s = Core::ICore::settings();
    s->beginGroup(kUsageGroup);
    s->beginGroup(hashedKey(projectKey).toLatin1());
    if (s->contains(kUsageContextTokens) || s->contains(kUsageMaxContext)) {
        snap.valid = true;
        snap.contextTokens = s->value(kUsageContextTokens, 0).toInt();
        snap.maxContext    = s->value(kUsageMaxContext, 0).toInt();
        snap.resetEpochSec = s->value(kUsageResetEpoch, 0).toLongLong();
    }
    s->endGroup();
    s->endGroup();
    return snap;
}

void QClaudeSettings::setLastUsageForProject(const QString &projectKey,
                                             int contextTokens,
                                             int maxContext,
                                             qint64 resetEpochSec)
{
    if (projectKey.isEmpty())
        return;
    auto *s = Core::ICore::settings();
    s->beginGroup(kUsageGroup);
    s->beginGroup(hashedKey(projectKey).toLatin1());
    s->setValue(kUsageContextTokens, contextTokens);
    s->setValue(kUsageMaxContext,    maxContext);
    s->setValue(kUsageResetEpoch,    resetEpochSec);
    s->endGroup();
    s->endGroup();
}

void QClaudeSettings::clearLastUsageForProject(const QString &projectKey)
{
    if (projectKey.isEmpty())
        return;
    auto *s = Core::ICore::settings();
    s->beginGroup(kUsageGroup);
    s->remove(hashedKey(projectKey).toLatin1());
    s->endGroup();
}

// ---- Options page ----

namespace {

class QClaudeOptionsWidget : public Core::IOptionsPageWidget
{
public:
    QClaudeOptionsWidget()
    {
        auto *form = new QFormLayout(this);
        form->setContentsMargins(12, 12, 12, 12);
        form->setSpacing(8);

        auto &s = QClaudeSettings::instance();

        // Top-of-page notice. Mirrors the kind of disclaimer Qt Creator's
        // built-in Copilot integration shows, adapted for Claude.
        auto *notice = new QLabel(this);
        notice->setTextFormat(Qt::RichText);
        notice->setWordWrap(true);
        notice->setOpenExternalLinks(true);
        notice->setTextInteractionFlags(Qt::TextBrowserInteraction);
        notice->setStyleSheet(
            "QLabel { background: rgba(217,119,87,0.10); "
            "         border: 1px solid rgba(217,119,87,0.35); "
            "         border-radius: 6px; padding: 10px 12px; "
            "         color: palette(text); }");
        notice->setText(Tr::tr(
            "<b>Notice.</b> Enabling Claude is subject to your agreement and "
            "abidance with your applicable Anthropic / Claude terms. It is your "
            "responsibility to know and accept the requirements and parameters "
            "of using tools like Claude. This may include, but is not limited "
            "to, ensuring you have the rights to allow Claude access to your "
            "code, as well as understanding any implications of your use of "
            "Claude and suggestions produced (like copyright, accuracy, etc.)."));
        form->addRow(notice);

        // Setup / install help block. Plain text + a couple of links to the
        // canonical Anthropic docs.
        auto *howto = new QLabel(this);
        howto->setTextFormat(Qt::RichText);
        howto->setWordWrap(true);
        howto->setOpenExternalLinks(true);
        howto->setTextInteractionFlags(Qt::TextBrowserInteraction);
        howto->setStyleSheet(
            "QLabel { background: palette(alternate-base); "
            "         border: 1px solid rgba(127,127,127,0.22); "
            "         border-radius: 6px; padding: 10px 12px; "
            "         color: palette(text); font-size: 12px; }");
        howto->setText(Tr::tr(
            "<b>Getting started</b>"
            "<ol style='margin: 6px 0 0 16px; padding: 0;'>"
            "<li>Install the Claude Code CLI:<br>"
            "<code>npm install -g @anthropic-ai/claude-code</code></li>"
            "<li>Authenticate from a terminal:<br>"
            "<code>claude</code> &nbsp;(or use the <i>Log in to Claude</i> "
            "button in the panel when prompted).</li>"
            "<li>If <code>claude</code> isn't on your <code>PATH</code>, set "
            "the <i>Executable</i> field above explicitly.</li>"
            "</ol>"
            "<p style='margin: 8px 0 0 0;'>"
            "<b>Tips</b>"
            "</p>"
            "<ul style='margin: 4px 0 0 16px; padding: 0;'>"
            "<li><b>Default model</b> applies to chat turns. <b>Autocomplete "
            "model</b> applies only to inline AI Complete — Haiku is the "
            "snappiest pick.</li>"
            "<li><b>Permission mode</b> controls whether Claude pauses for "
            "approval before editing files. <i>Ask before edits</i> is the "
            "safest default.</li>"
            "<li>Inline AI Complete is toggled per-panel via the ✨ chip; the "
            "setting persists across restarts.</li>"
            "</ul>"
            "<p style='margin: 8px 0 0 0;'>"
            "Docs: <a href='https://docs.claude.com/en/docs/claude-code/overview'>"
            "docs.claude.com/claude-code</a> &nbsp;·&nbsp; "
            "Source: <a href='https://github.com/anthropics/claude-code'>"
            "github.com/anthropics/claude-code</a>"
            "</p>"));
        form->addRow(howto);

        // Visual divider before the actual settings rows.
        auto *divider = new QFrame(this);
        divider->setFrameShape(QFrame::HLine);
        divider->setFrameShadow(QFrame::Sunken);
        divider->setStyleSheet("color: rgba(127,127,127,0.25);");
        form->addRow(divider);

        // Executable path row
        auto *pathRow = new QWidget(this);
        auto *pathLayout = new QHBoxLayout(pathRow);
        pathLayout->setContentsMargins(0, 0, 0, 0);
        pathLayout->setSpacing(4);
        m_pathEdit = new QLineEdit(s.executablePath(), pathRow);
        m_pathEdit->setPlaceholderText(
            Tr::tr("Auto-detect (PATH, ~/.local/bin/claude, ~/.claude/local/claude, …)"));
        m_browseBtn = new QToolButton(pathRow);
        m_browseBtn->setText(Tr::tr("Browse…"));
        connect(m_browseBtn, &QToolButton::clicked, this, [this]() {
            const QString p = QFileDialog::getOpenFileName(
                this, Tr::tr("Locate the claude executable"), m_pathEdit->text());
            if (!p.isEmpty())
                m_pathEdit->setText(p);
        });
        pathLayout->addWidget(m_pathEdit, 1);
        pathLayout->addWidget(m_browseBtn);

        m_resolvedLabel = new QLabel(this);
        m_resolvedLabel->setStyleSheet("color: palette(mid); font-size: 11px;");
        auto refreshResolved = [this]() {
            QString exe = m_pathEdit->text().trimmed();
            if (exe.isEmpty())
                exe = ClaudeProcess::findClaudeExecutable();
            m_resolvedLabel->setText(Tr::tr("Resolved: %1").arg(exe));
        };
        connect(m_pathEdit, &QLineEdit::textChanged, this, refreshResolved);
        refreshResolved();

        form->addRow(Tr::tr("Executable:"), pathRow);
        form->addRow(QString(), m_resolvedLabel);

        // Default model
        m_modelEdit = new QLineEdit(s.defaultModel(), this);
        m_modelEdit->setPlaceholderText(
            Tr::tr("(leave empty to use the CLI default — e.g. claude-sonnet-4-6)"));
        form->addRow(Tr::tr("Default model:"), m_modelEdit);

        // Default permission mode
        m_permCombo = new QComboBox(this);
        m_permCombo->addItem(Tr::tr("Ask before edits (default)"),       QStringLiteral("default"));
        m_permCombo->addItem(Tr::tr("Auto-accept edits (acceptEdits)"),  QStringLiteral("acceptEdits"));
        m_permCombo->addItem(Tr::tr("Plan only (plan)"),                  QStringLiteral("plan"));
        m_permCombo->addItem(Tr::tr("Bypass all prompts (bypassPermissions)"),
                             QStringLiteral("bypassPermissions"));
        const int idx = m_permCombo->findData(s.defaultPermissionMode());
        m_permCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        form->addRow(Tr::tr("Default permission mode:"), m_permCombo);

        // Autocomplete model — independent of the chat model.
        m_autocompleteModelCombo = new QComboBox(this);
        m_autocompleteModelCombo->setEditable(true);
        m_autocompleteModelCombo->addItem(Tr::tr("Use chat model"),       QString());
        m_autocompleteModelCombo->addItem(Tr::tr("Haiku 4.5  (fastest)"), QStringLiteral("claude-haiku-4-5"));
        m_autocompleteModelCombo->addItem(Tr::tr("Sonnet 4.6"),           QStringLiteral("claude-sonnet-4-6"));
        m_autocompleteModelCombo->addItem(Tr::tr("Opus 4.7  (1M context)"), QStringLiteral("claude-opus-4-7"));
        const QString acm = s.autocompleteModel();
        const int acIdx = m_autocompleteModelCombo->findData(acm);
        if (acIdx >= 0) {
            m_autocompleteModelCombo->setCurrentIndex(acIdx);
        } else if (!acm.isEmpty()) {
            // Custom model id typed by the user previously.
            m_autocompleteModelCombo->setEditText(acm);
        }
        form->addRow(Tr::tr("Autocomplete model:"), m_autocompleteModelCombo);

        setOnApply([this]() {
            auto &s = QClaudeSettings::instance();
            s.setExecutablePath(m_pathEdit->text().trimmed());
            s.setDefaultModel(m_modelEdit->text().trimmed());
            s.setDefaultPermissionMode(m_permCombo->currentData().toString());
            // For the autocomplete combo, prefer the dropdown's data if the
            // user picked a preset; otherwise fall back to the (possibly
            // hand-edited) text.
            QString acm = m_autocompleteModelCombo->currentData().toString();
            const QString typed = m_autocompleteModelCombo->currentText().trimmed();
            // If the typed text matches a preset's display, prefer the data
            // (which is the canonical model id). Otherwise use the typed
            // value verbatim — supports custom model ids.
            if (acm.isEmpty()
                && typed != m_autocompleteModelCombo->itemText(0).trimmed()) {
                acm = typed;
            }
            s.setAutocompleteModel(acm);
        });
    }

private:
    QLineEdit *m_pathEdit = nullptr;
    QToolButton *m_browseBtn = nullptr;
    QLabel *m_resolvedLabel = nullptr;
    QLineEdit *m_modelEdit = nullptr;
    QComboBox *m_permCombo = nullptr;
    QComboBox *m_autocompleteModelCombo = nullptr;
};

} // namespace

QClaudeOptionsPage::QClaudeOptionsPage()
{
    setId("QClaude.Settings");
    setDisplayName(Tr::tr("Claude"));
    setCategory(QClaude::Compat::settingsCategory());
    setWidgetCreator([]() { return new QClaudeOptionsWidget; });
}

} // namespace QClaude::Internal
