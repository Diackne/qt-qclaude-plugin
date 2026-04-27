#pragma once

#include <coreplugin/dialogs/ioptionspage.h>

#include <QObject>
#include <QString>

namespace QClaude::Internal {

class QClaudeSettings : public QObject
{
    Q_OBJECT

public:
    static QClaudeSettings &instance();

    QString executablePath() const { return m_executablePath; }
    QString defaultModel() const { return m_defaultModel; }
    QString defaultPermissionMode() const { return m_defaultPermissionMode; }
    QString autocompleteModel() const { return m_autocompleteModel; }
    bool    autocompleteEnabled() const { return m_autocompleteEnabled; }

    void setExecutablePath(const QString &v);
    void setDefaultModel(const QString &v);
    void setDefaultPermissionMode(const QString &v);
    void setAutocompleteModel(const QString &v);
    void setAutocompleteEnabled(bool v);

    // Resolved model for the autocomplete engine: returns the explicit
    // autocomplete override if set, otherwise the chat default model, and
    // finally Haiku 4.5 if neither is configured — Haiku is the snappiest
    // option and the right default for inline completions.
    QString resolvedAutocompleteModel() const {
        if (!m_autocompleteModel.isEmpty())
            return m_autocompleteModel;
        if (!m_defaultModel.isEmpty())
            return m_defaultModel;
        return QStringLiteral("claude-haiku-4-5");
    }

    void load();
    void save() const;

    QString resolvedExecutable() const;

    // Per-project session persistence.
    QString sessionForProject(const QString &projectKey) const;
    void setSessionForProject(const QString &projectKey, const QString &sessionId);
    void clearSessionForProject(const QString &projectKey);

    // Per-project last-known context-usage snapshot. Lets the header usage
    // badge reappear with the correct % on Qt Creator launch instead of
    // staying empty until the next chat turn fires a usageReport.
    struct UsageSnapshot {
        bool   valid = false;
        int    contextTokens = 0;
        int    maxContext    = 0;
        qint64 resetEpochSec = 0;
    };
    UsageSnapshot lastUsageForProject(const QString &projectKey) const;
    void setLastUsageForProject(const QString &projectKey,
                                int contextTokens,
                                int maxContext,
                                qint64 resetEpochSec);
    void clearLastUsageForProject(const QString &projectKey);

signals:
    void changed();

private:
    QClaudeSettings();

    QString m_executablePath;
    QString m_defaultModel;
    QString m_defaultPermissionMode = QStringLiteral("default");
    QString m_autocompleteModel; // empty = inherit from m_defaultModel
    bool    m_autocompleteEnabled = false;
};

class QClaudeOptionsPage final : public Core::IOptionsPage
{
public:
    QClaudeOptionsPage();
};

} // namespace QClaude::Internal
