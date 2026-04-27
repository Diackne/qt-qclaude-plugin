#include "qclaudepanel.h"
#include "qclaudesettings.h"
#include "qclaudetr.h"

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/icontext.h>
#include <coreplugin/rightpane.h>
#include <coreplugin/statusbarmanager.h>

#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectnodes.h>
#include <projectexplorer/projecttree.h>

#include <texteditor/texteditor.h>
#include <texteditor/texteditorconstants.h>

#include <extensionsystem/iplugin.h>

#include <utils/filepath.h>
#include <utils/theme/theme.h>

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QStyle>
#include <QSvgRenderer>
#include <QTextCursor>
#include <QToolButton>

#include <memory>

// Q_INIT_RESOURCE expands to a call to a free function declared via `extern`.
// When invoked from inside a namespace, that extern declaration lands in the
// namespace too, leaving the linker hunting for a namespaced symbol. Keep the
// invocation in a tiny free function at global scope instead.
static void qclaudeInitResources()
{
    Q_INIT_RESOURCE(qclaude);
}

namespace QClaude::Internal {

// Render the SVG to a pixmap and tint every visible pixel with `color`,
// keeping the alpha mask. Produces a crisp result at the requested DPR.
static QPixmap renderTintedSvg(const QString &resourcePath,
                               const QSize &size,
                               const QColor &color,
                               qreal dpr)
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid())
        return {};

    QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(size)));
    }
    {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), color);
    }
    return QPixmap::fromImage(img);
}

static QIcon makeThemedClaudeIcon(const QSize &size)
{
    QColor color = QApplication::palette().color(QPalette::WindowText);
    if (auto *theme = Utils::creatorTheme()) {
        const QColor themed = theme->color(Utils::Theme::IconsBaseColor);
        if (themed.isValid())
            color = themed;
    }

    QIcon icon;
    icon.addPixmap(renderTintedSvg(QStringLiteral(":/qclaude/icon.svg"),
                                   size, color, 1.0));
    icon.addPixmap(renderTintedSvg(QStringLiteral(":/qclaude/icon.svg"),
                                   size, color, 2.0));
    return icon;
}


class QClaudePlugin final : public ExtensionSystem::IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QtCreatorPlugin" FILE "QClaude.json")

public:
    QClaudePlugin() = default;

    ~QClaudePlugin() final
    {
        if (m_button) {
            Core::StatusBarManager::destroyStatusBarWidget(m_button);
            m_button = nullptr;
        }
        if (m_panel && !m_panel->parent())
            delete m_panel;
    }

    void initialize() final
    {
        ::qclaudeInitResources();

        m_optionsPage = std::make_unique<QClaudeOptionsPage>();

        m_button = new QToolButton;
        const QSize iconSize(18, 18);
        QIcon icon = makeThemedClaudeIcon(iconSize);
        if (icon.isNull())
            icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxQuestion);
        m_button->setIcon(icon);
        m_button->setIconSize(iconSize);
        m_button->setToolTip(Tr::tr("Toggle Claude panel"));
        m_button->setCheckable(true);
        m_button->setAutoRaise(true);
        connect(m_button, &QToolButton::clicked, this, &QClaudePlugin::onToggle);

        Core::StatusBarManager::addStatusBarWidget(m_button,
                                                   Core::StatusBarManager::RightCorner);

        registerContextActions();
    }

    void extensionsInitialized() final {}

    ShutdownFlag aboutToShutdown() final { return SynchronousShutdown; }

private:
    void onToggle(bool checked)
    {
        auto *rp = Core::RightPaneWidget::instance();
        if (!rp)
            return;

        if (checked) {
            if (!m_panel)
                m_panel = new QClaudePanel;
            rp->setWidget(m_panel);
            rp->setShown(true);
            m_panel->setFocus();
        } else {
            rp->setShown(false);
        }
    }

    void showPanel()
    {
        if (!m_panel)
            m_panel = new QClaudePanel;
        auto *rp = Core::RightPaneWidget::instance();
        if (rp) {
            rp->setWidget(m_panel);
            rp->setShown(true);
        }
        if (m_button)
            m_button->setChecked(true);
    }

    void askFromEditor()
    {
        auto *editor = TextEditor::BaseTextEditor::currentTextEditor();
        if (!editor)
            return;

        QString filePath;
        if (auto *doc = editor->document())
            filePath = doc->filePath().toFSPathString();
        if (filePath.isEmpty())
            return;

        const QString selection = editor->selectedText();
        const QTextCursor cursor = editor->textCursor();
        int startLine = -1;
        int endLine = -1;
        if (cursor.hasSelection()) {
            QTextCursor c = cursor;
            c.setPosition(cursor.selectionStart());
            startLine = c.blockNumber() + 1;
            c.setPosition(cursor.selectionEnd());
            endLine = c.blockNumber() + 1;
        } else {
            startLine = cursor.blockNumber() + 1;
        }

        showPanel();
        if (m_panel)
            m_panel->askAboutFile(filePath, selection, startLine, endLine);
    }

    void askFromProjectTree()
    {
        ProjectExplorer::Node *node = ProjectExplorer::ProjectTree::currentNode();
        if (!node)
            return;
        const Utils::FilePath fp = node->filePath();
        if (fp.isEmpty())
            return;

        showPanel();
        if (m_panel)
            m_panel->askAboutFile(fp.toFSPathString());
    }

    void registerContextActions()
    {
        Core::ActionBuilder editorAction(this, "QClaude.AskAboutSelection");
        editorAction.setText(Tr::tr("Ask Claude about selection / file"));
        editorAction.setContext(Core::Constants::C_GLOBAL);
        editorAction.addToContainer(TextEditor::Constants::M_STANDARDCONTEXTMENU,
                                    /*group*/ Utils::Id(), /*needsToExist*/ false);
        editorAction.addOnTriggered(this, [this]() { askFromEditor(); });

        Core::ActionBuilder treeAction(this, "QClaude.AskAboutFile");
        treeAction.setText(Tr::tr("Ask Claude about this file"));
        treeAction.setContext(Core::Constants::C_GLOBAL);
        treeAction.addToContainer(ProjectExplorer::Constants::M_FILECONTEXT,
                                  Utils::Id(), false);
        treeAction.addToContainer(ProjectExplorer::Constants::M_FOLDERCONTEXT,
                                  Utils::Id(), false);
        treeAction.addOnTriggered(this, [this]() { askFromProjectTree(); });

        // Inline AI completion in the editor: Alt+\ triggers a Claude
        // completion at the caret. The panel itself owns the on/off state
        // via its "✨ AI Complete" toggle; if the toggle is off, the action
        // creates the panel just so the user can enable it, then asks them
        // to press the shortcut again.
        Core::ActionBuilder completeAction(this, "QClaude.TriggerCompletion");
        completeAction.setText(Tr::tr("Insert Claude completion at caret"));
        completeAction.setContext(Core::Constants::C_GLOBAL);
        completeAction.setDefaultKeySequence(QKeySequence(QStringLiteral("Alt+\\")));
        completeAction.addOnTriggered(this, [this]() {
            if (!m_panel)
                m_panel = new QClaudePanel;
            if (!m_panel->isAutocompleteEnabled()) {
                showPanel();
                return;
            }
            m_panel->triggerAutocompletion();
        });
    }

    QPointer<QToolButton> m_button;
    QPointer<QClaudePanel> m_panel;
    std::unique_ptr<QClaudeOptionsPage> m_optionsPage;
};

} // namespace QClaude::Internal

#include "qclaudeplugin.moc"
