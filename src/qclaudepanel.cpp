#include "qclaudepanel.h"

#include "claudeprocess.h"
#include "qclaudeautocomplete.h"
#include "qclaudeconstants.h"
#include "qclaudediff.h"
#include "qclaudefilepicker.h"
#include "qclaudehookserver.h"
#include "qclaudesettings.h"
#include "qclaudetr.h"

#include <extensionsystem/pluginmanager.h>
#include <extensionsystem/pluginspec.h>

#include <coreplugin/editormanager/documentmodel.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/icore.h>
#include <coreplugin/idocument.h>

#include <texteditor/texteditor.h>

#include <projectexplorer/project.h>
#include <projectexplorer/projecttree.h>

#include <utils/differ.h>
#include <utils/filepath.h>
#include <utils/id.h>
#include <utils/plaintextedit/plaintextedit.h>
#include <utils/utilsicons.h>

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QTextBrowser>
#include <QTextCursor>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTextCharFormat>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <utils/theme/theme.h>

namespace QClaude::Internal {

static QString escape(const QString &s)
{
    return s.toHtmlEscaped();
}

// Pick the `--permission-mode` value to pass claude given the user's
// configured mode and whether the pre-flight gate (Ask-before-edits) is
// armed. `plan` and `bypassPermissions` are forwarded verbatim — the
// gate doesn't apply. Otherwise the chip flips between `default` (gate
// armed → claude routes through `--permission-prompt-tool`) and
// `acceptEdits` (gate off → claude auto-accepts file modifications).
static QString resolveRunMode(const QString &settingsMode, bool gateArmed)
{
    if (settingsMode == QStringLiteral("plan")
        || settingsMode == QStringLiteral("bypassPermissions"))
        return settingsMode;
    return gateArmed ? QStringLiteral("default")
                     : QStringLiteral("acceptEdits");
}

// Forward decl — defined further down in the file.
static QString makeFileRef(const QString &filePath,
                           const QString &cwd,
                           int startLine = -1,
                           int endLine = -1);

// Forward decl — defined further down. Renders a compact line-by-line diff
// hunk as HTML for inclusion in the chat under an Edit/Write card.
static QString renderInlineDiff(const QString &before,
                                const QString &after,
                                int maxLines = 24);

// Render the bundled SVG into an alpha-tinted QImage for embedding in the
// chat document via QTextDocument::addResource(QTextDocument::ImageResource).
static QImage renderLogoImage(int sidePx, qreal dpr)
{
    QSvgRenderer renderer(QStringLiteral(":/qclaude/icon.svg"));
    if (!renderer.isValid())
        return {};

    QImage img(QSize(sidePx, sidePx) * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&p, QRectF(0, 0, sidePx, sidePx));
    }
    QColor tint = QColor(217, 119, 87); // Claude warm accent
    if (auto *theme = Utils::creatorTheme()) {
        const QColor c = theme->color(Utils::Theme::IconsBaseColor);
        if (c.isValid())
            tint = c;
    }
    {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), tint);
    }
    return img;
}

// A "rail row": a 2-column table where the first cell holds the bullet
// (or a blank spacer) and a continuous left border that visually merges with
// the next row's border to form a vertical timeline rail.
//
//   railRow("dot-gray", "●", "<b>Thinking</b>") =>
//     │ ● Thinking
//     │
static QString railRow(const QString &dotClass,
                       const QString &dotGlyph,
                       const QString &innerHtml)
{
    return QStringLiteral(
        "<table cellpadding='0' cellspacing='0' border='0' width='100%' "
        "       style='margin:0; border-collapse:collapse;'>"
        "  <tr>"
        "    <td valign='top' width='22' "
        "        style='border-left: 1px solid rgba(160,160,160,0.22); "
        "               padding: 4px 0 4px 6px;'>"
        "      <span class='%1'>%2</span>"
        "    </td>"
        "    <td valign='top' style='padding: 4px 0 4px 12px;'>%3</td>"
        "  </tr>"
        "</table>")
        .arg(dotClass, dotGlyph, innerHtml);
}

// Same shape but with no visible bullet — used for free-flowing assistant /
// user / system text that should still sit on the rail.
static QString railRowEmpty(const QString &innerHtml)
{
    return railRow(QStringLiteral("dot-gray"),
                   QStringLiteral("&nbsp;"),
                   innerHtml);
}

// Renders a small unified-style diff between `before` and `after` as a
// monospace HTML table: green-tinted rows for additions, red-tinted for
// deletions, muted for unchanged context. Caps output at `maxLines` and
// appends a "+N more" footer.
static QString renderInlineDiff(const QString &before,
                                const QString &after,
                                int maxLines)
{
    Utils::Differ differ;
    differ.setDiffMode(Utils::Differ::LineMode);
    const QList<Utils::Diff> diffs = differ.diff(before, after);

    struct DiffLine { QChar mark; QString text; };
    QList<DiffLine> lines;

    for (const Utils::Diff &d : diffs) {
        const QChar mark =
            (d.command == Utils::Diff::Insert) ? QLatin1Char('+') :
            (d.command == Utils::Diff::Delete) ? QLatin1Char('-') :
                                                  QLatin1Char(' ');
        QStringList parts = d.text.split(QLatin1Char('\n'));
        // A trailing newline produces an empty last element — drop it; the
        // newline is implicit in the table-row break.
        if (!parts.isEmpty() && parts.last().isEmpty())
            parts.removeLast();
        for (const QString &p : parts)
            lines.append({mark, p});
    }

    int truncated = 0;
    if (maxLines > 0 && lines.size() > maxLines) {
        truncated = lines.size() - maxLines;
        lines = lines.mid(0, maxLines);
    }
    if (lines.isEmpty())
        return {};

    QString html;
    html += QStringLiteral(
        "<table cellpadding='0' cellspacing='0' width='100%' "
        "       style='border-collapse:collapse; "
        "              background: palette(alternate-base); "
        "              border-radius:6px; margin:4px 0;'>");

    for (const DiffLine &l : lines) {
        QString bg, color;
        if (l.mark == QLatin1Char('+')) {
            bg    = QStringLiteral("rgba(76,175,80,0.20)");
            color = QStringLiteral("palette(text)");
        } else if (l.mark == QLatin1Char('-')) {
            bg    = QStringLiteral("rgba(239,83,80,0.20)");
            color = QStringLiteral("palette(text)");
        } else {
            bg    = QStringLiteral("transparent");
            color = QStringLiteral("palette(mid)");
        }
        html += QStringLiteral(
            "<tr><td style='background-color:%1; color:%2; "
            "               font-family:Menlo,Consolas,monospace; "
            "               font-size:12px; padding:1px 8px; "
            "               white-space:pre;'>%3 %4</td></tr>")
            .arg(bg,
                 color,
                 QString(l.mark),
                 l.text.toHtmlEscaped());
    }
    html += QStringLiteral("</table>");

    if (truncated > 0) {
        html += QStringLiteral(
            "<div style='color: palette(mid); font-size:11px; "
            "            padding:2px 8px;'>… %1 more line%2</div>")
            .arg(QString::number(truncated),
                 truncated == 1 ? QString() : QStringLiteral("s"));
    }
    return html;
}

static QToolButton *makeIconToolButton(const QString &glyph, const QString &tooltip)
{
    auto *tb = new QToolButton;
    tb->setText(glyph);
    tb->setToolTip(tooltip);
    tb->setAutoRaise(true);
    tb->setCursor(Qt::PointingHandCursor);
    QFont f = tb->font();
    f.setPointSize(f.pointSize() + 2);
    tb->setFont(f);
    return tb;
}

QClaudePanel::QClaudePanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("QClaude.Panel");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- Header bar (title + context unified) ----
    auto *header = new QWidget(this);
    header->setObjectName("QClaude.Header");
    header->setStyleSheet(
        "#QClaude\\.Header { border-bottom: 1px solid rgba(127,127,127,0.18); }");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 10, 8, 10);
    headerLayout->setSpacing(8);

    auto *titleCol = new QVBoxLayout();
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(2);

    m_titleLabel = new QLabel(Tr::tr("New chat"), header);
    QFont titleFont = m_titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(qMax(8, titleFont.pointSize() - 1));
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleCol->addWidget(m_titleLabel);

    m_contextLabel = new QLabel(header);
    m_contextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_contextLabel->setWordWrap(false);
    m_contextLabel->setStyleSheet("color: palette(mid); font-size: 11px;");
    titleCol->addWidget(m_contextLabel);

    headerLayout->addLayout(titleCol, 1);

    // Session usage badge (context %, plus "resets HH:MM" when the CLI tells
    // us). Hidden until we have data. Sits to the right of the title column,
    // just before the History/New buttons.
    m_usageLabel = new QLabel(header);
    m_usageLabel->setStyleSheet("color: palette(mid); font-size: 11px; padding: 0 4px;");
    m_usageLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_usageLabel->setVisible(false);
    headerLayout->addWidget(m_usageLabel);

    m_historyTb = makeIconToolButton(QStringLiteral("⏱"), Tr::tr("Session history"));
    connect(m_historyTb, &QToolButton::clicked, this, &QClaudePanel::onHistoryClicked);
    headerLayout->addWidget(m_historyTb);

    m_newChatTb = makeIconToolButton(QStringLiteral("+"), Tr::tr("New chat"));
    connect(m_newChatTb, &QToolButton::clicked, this, &QClaudePanel::onNewChatClicked);
    headerLayout->addWidget(m_newChatTb);

    root->addWidget(header);

    // ---- Context-usage bar ----
    // Indeterminate "Claude is working" progress strip. Only shown while a
    // turn is in flight; the cumulative usage % lives in the header label.
    m_usageBar = new QProgressBar(this);
    m_usageBar->setRange(0, 0);
    m_usageBar->setTextVisible(false);
    m_usageBar->setFixedHeight(4);
    m_usageBar->setVisible(false);
    m_usageBar->setStyleSheet(
        "QProgressBar { background: rgba(127,127,127,0.12); border: none; }"
        "QProgressBar::chunk { background: #d97757; }");
    root->addWidget(m_usageBar);

    // ---- Chat view ----
    m_view = new QTextBrowser(this);
    m_view->setOpenLinks(false);
    m_view->setOpenExternalLinks(false);
    m_view->setFrameShape(QFrame::NoFrame);
    connect(m_view, &QTextBrowser::anchorClicked, this, &QClaudePanel::onAnchorClicked);

    // Make the themed logo addressable via <img src='qclaude://qclaude'> in
    // the welcome block.
    {
        const QImage logo = renderLogoImage(56, qApp->devicePixelRatio());
        if (!logo.isNull()) {
            m_view->document()->addResource(QTextDocument::ImageResource,
                                            QUrl(QStringLiteral("qclaude://qclaude")),
                                            QVariant(logo));
        }
    }
    m_view->document()->setDefaultStyleSheet(
        "body { font-size: 13px; line-height: 1.5; }"
        // Section headers in markdown — give them air.
        "h1 { font-size: 16px; font-weight: 600; margin: 14px 0 6px 0; }"
        "h2 { font-size: 14px; font-weight: 600; margin: 12px 0 6px 0; }"
        "h3 { font-size: 13px; font-weight: 600; margin: 10px 0 4px 0; "
        "     color: palette(mid); text-transform: uppercase; "
        "     letter-spacing: 0.5px; }"
        "p  { margin: 4px 0; }"
        "ul, ol { margin: 4px 0 4px 18px; }"
        "li { margin: 2px 0; }"
        "hr { border: none; border-top: 1px solid rgba(127,127,127,0.18); "
        "     margin: 10px 0; }"
        // Inline code chips and code blocks ----------------------------------
        "code { font-family: Menlo, Consolas, monospace; font-size: 12px; "
        "       background: rgba(127,127,127,0.14); padding: 1px 5px; "
        "       border-radius: 3px; }"
        "pre { font-family: Menlo, Consolas, monospace; font-size: 12px; "
        "      background: palette(alternate-base); padding: 10px 12px; "
        "      border-radius: 8px; line-height: 1.45; "
        "      border: 1px solid rgba(127,127,127,0.14); margin: 6px 0; }"
        "pre code { background: transparent; padding: 0; }"
        // Rail wrappers ------------------------------------------------------
        ".rail-cell { padding: 6px 0 6px 6px; }"
        ".rail-content { padding: 6px 0 6px 12px; }"
        ".dot-gray  { color: rgba(160,160,160,0.7); font-size: 14px; }"
        ".dot-green { color: #4caf50; font-size: 14px; }"
        ".dot-red   { color: #ef5350; font-size: 14px; }"
        ".dot-blue  { color: #4a90e2; font-size: 14px; }"
        ".label  { font-weight: bold; }"
        ".caret  { color: rgba(160,160,160,0.6); }"
        ".muted  { color: palette(mid); }"
        ".mono   { font-family: Menlo, Consolas, monospace; font-size: 12px; }"
        ".tool-sub { color: palette(mid); font-size: 11px; "
        "            margin-top: 2px; }"
        ".kv-key { color: palette(mid); font-size: 11px; "
        "          text-transform: uppercase; letter-spacing: 0.4px; "
        "          margin-right: 6px; }"
        // Tool cards ---------------------------------------------------------
        ".tool-card { background: palette(alternate-base); border-radius: 8px; "
        "             padding: 8px 10px; margin: 6px 0; "
        "             border: 1px solid rgba(127,127,127,0.14); "
        "             font-family: Menlo, Consolas, monospace; font-size: 12px; }"
        ".badge { background: rgba(127,127,127,0.22); color: palette(mid); "
        "         border-radius: 3px; padding: 1px 5px; font-size: 10px; "
        "         font-family: Menlo, Consolas, monospace; margin-right: 8px; }"
        // Edit cards ---------------------------------------------------------
        ".edit-stats-add { color: #4caf50; }"
        ".edit-stats-del { color: #ef5350; }"
        "a { color: #d97757; text-decoration: none; }"
        ".reverted { color: palette(mid); font-style: italic; }"
        // System / error / thinking variants --------------------------------
        ".sys-text   { color: palette(mid); font-size: 11px; }"
        ".err-text   { color: #ef5350; }"
        ".err-strong { color: #ef5350; font-weight: 600; }"
        ".think-text { color: palette(mid); font-style: italic; font-size: 12px; }"
        // Welcome empty-state ------------------------------------------------
        ".welcome { color: rgba(170,170,170,0.95); }"
        ".welcome .headline { font-size: 16px; font-weight: 600; "
        "                     color: palette(text); }"
        ".welcome .lede { font-size: 13px; line-height: 1.55; "
        "                 color: palette(mid); }"
        ".welcome .chips a { display: inline; padding: 6px 12px; "
        "                    border-radius: 14px; "
        "                    background: rgba(217,119,87,0.12); "
        "                    color: #d97757; font-size: 12px; "
        "                    text-decoration: none; margin: 0 4px; }"
        ".welcome .chips a:hover { background: rgba(217,119,87,0.22); }");
    root->addWidget(m_view, /*stretch*/ 1);

    // ---- Input area (text input + bottom toolbar) ----
    auto *inputBox = new QFrame(this);
    inputBox->setObjectName("QClaude.InputBox");
    inputBox->setFrameShape(QFrame::StyledPanel);
    // Rounded panel with a soft accent glow when the input has focus.
    inputBox->setStyleSheet(
        "#QClaude\\.InputBox {"
        "  border: 1px solid rgba(127,127,127,0.28);"
        "  border-radius: 12px;"
        "  background: palette(base);"
        "}"
        "#QClaude\\.InputBox[focused='true'] {"
        "  border: 1px solid rgba(217,119,87,0.7);"
        "}");
    auto *inputBoxLayout = new QVBoxLayout(inputBox);
    inputBoxLayout->setContentsMargins(12, 10, 12, 10);
    inputBoxLayout->setSpacing(8);

    m_input = new QPlainTextEdit(inputBox);
    m_input->setPlaceholderText(Tr::tr("Ask Claude…   ⌘/Ctrl-Enter to send"));
    m_input->setFrameShape(QFrame::NoFrame);
    m_input->setTabChangesFocus(true);
    QFontMetrics fm(m_input->font());
    m_input->setFixedHeight(fm.lineSpacing() * 3 + 8);
    m_input->installEventFilter(this);
    m_input->setStyleSheet("QPlainTextEdit { background: transparent; }");
    inputBoxLayout->addWidget(m_input);

    // Inline @-mention fuzzy file picker. The picker is a frameless popup
    // that appears above the composer when the user types `@` (or chooses
    // *Search files…* from the + menu). Its parent is the panel so it can
    // outlive the input box layout, and its initial project is set after
    // the rest of the panel's project hooks fire.
    m_filePicker = new FilePicker(this);
    connect(m_filePicker, &FilePicker::accepted,
            this, &QClaudePanel::acceptMentionPick);
    connect(m_input, &QPlainTextEdit::textChanged,
            this, &QClaudePanel::updateMentionPicker);

    auto *bottomBar = new QHBoxLayout();
    bottomBar->setContentsMargins(0, 0, 0, 0);
    bottomBar->setSpacing(4);

    m_addTb = makeIconToolButton(QStringLiteral("+"), Tr::tr("Add file or context"));
    connect(m_addTb, &QToolButton::clicked, this, &QClaudePanel::onAddClicked);
    bottomBar->addWidget(m_addTb);

    m_commandsTb = makeIconToolButton(QStringLiteral("/"), Tr::tr("Show command menu"));
    connect(m_commandsTb, &QToolButton::clicked, this, &QClaudePanel::onCommandsClicked);
    bottomBar->addWidget(m_commandsTb);

    // ---- Current-file chip (auto-included as @file in every prompt) ----
    m_currentFileChip = new QToolButton(inputBox);
    m_currentFileChip->setIcon(QApplication::style()->standardIcon(QStyle::SP_FileIcon));
    m_currentFileChip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_currentFileChip->setCheckable(true);
    m_currentFileChip->setChecked(true);
    m_currentFileChip->setAutoRaise(false);
    m_currentFileChip->setCursor(Qt::PointingHandCursor);
    m_currentFileChip->setStyleSheet(
        "QToolButton { background: rgba(127,127,127,0.16); border: none; "
        "              border-radius: 10px; padding: 2px 10px 2px 8px; "
        "              color: palette(text); }"
        "QToolButton:hover { background: rgba(127,127,127,0.26); }"
        "QToolButton:!checked { color: palette(mid); "
        "                       background: rgba(127,127,127,0.08); }");
    m_currentFileChip->setVisible(false);
    connect(m_currentFileChip, &QToolButton::toggled, this, [this](bool on) {
        m_currentFileChip->setToolTip(on
            ? Tr::tr("Current file is included in the next prompt. Click to detach.")
            : Tr::tr("Current file is detached. Click to include it again."));
    });
    bottomBar->addWidget(m_currentFileChip);

    bottomBar->addStretch(1);

    // ---- Autocomplete chip (auto-suggest while typing in editor) ----
    m_autocompleteTb = new QToolButton(inputBox);
    m_autocompleteTb->setText(QStringLiteral("✨"));
    m_autocompleteTb->setProperty("collapsedText", QStringLiteral("✨"));
    m_autocompleteTb->setProperty("expandedText",  Tr::tr("✨  AI Complete"));
    m_autocompleteTb->setToolTip(Tr::tr(
        "Toggle AI autocomplete in the editor.\n"
        "When on, type in any code editor and pause to see a Claude suggestion. "
        "Tab to accept, Esc to dismiss. Alt+\\ triggers manually.\n"
        "Pick the autocomplete model under / → Model → AI Complete model."));
    m_autocompleteTb->setCheckable(true);
    // Restore the on/off state from settings — persists across chats and
    // across Qt Creator restarts.
    m_autocompleteTb->setChecked(QClaudeSettings::instance().autocompleteEnabled());
    m_autocompleteTb->setAutoRaise(true);
    m_autocompleteTb->setCursor(Qt::PointingHandCursor);
    m_autocompleteTb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_autocompleteTb->setStyleSheet(
        "QToolButton { background: rgba(127,127,127,0.10); border: none; "
        "              border-radius: 10px; padding: 2px 10px; "
        "              color: palette(mid); }"
        "QToolButton:hover { background: rgba(127,127,127,0.22); }"
        "QToolButton:checked { background: rgba(217,119,87,0.22); "
        "                      color: palette(text); }"
        "QToolButton:checked:hover { background: rgba(217,119,87,0.32); }");
    m_autocompleteTb->installEventFilter(this);
    // Persist every flip to the chip so reopening the panel/Qt Creator restores it.
    connect(m_autocompleteTb, &QToolButton::toggled, this, [](bool on) {
        QClaudeSettings::instance().setAutocompleteEnabled(on);
    });
    bottomBar->addWidget(m_autocompleteTb);

    m_askEditsTb = new QToolButton(inputBox);
    m_askEditsTb->setText(QStringLiteral("✋"));
    m_askEditsTb->setProperty("collapsedText", QStringLiteral("✋"));
    m_askEditsTb->setProperty("expandedText",  Tr::tr("✋  Ask before edits"));
    m_askEditsTb->setToolTip(Tr::tr("Toggle between asking before edits "
                                    "(default permission mode) and auto-accepting them"));
    m_askEditsTb->setCheckable(true);
    m_askEditsTb->setChecked(true);
    m_askEditsTb->setAutoRaise(true);
    m_askEditsTb->setCursor(Qt::PointingHandCursor);
    m_askEditsTb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_askEditsTb->setStyleSheet(
        "QToolButton { background: rgba(127,127,127,0.10); border: none; "
        "              border-radius: 10px; padding: 2px 10px; "
        "              color: palette(mid); }"
        "QToolButton:hover { background: rgba(127,127,127,0.22); }"
        "QToolButton:checked { background: rgba(217,119,87,0.22); "
        "                      color: palette(text); }"
        "QToolButton:checked:hover { background: rgba(217,119,87,0.32); }");
    m_askEditsTb->installEventFilter(this);
    connect(m_askEditsTb, &QToolButton::toggled, this, &QClaudePanel::onAskBeforeEditsToggled);
    bottomBar->addWidget(m_askEditsTb);

    m_sendButton = makeIconToolButton(QStringLiteral("↑"), Tr::tr("Send (Ctrl+Enter)"));
    m_sendButton->setMinimumSize(32, 28);
    m_sendButton->setStyleSheet(
        "QToolButton { background: #d97757; color: white; border: none; "
        "              border-radius: 6px; padding: 4px 10px; "
        "              font-weight: 600; font-size: 14px; }"
        "QToolButton:hover { background: #c2654a; }"
        "QToolButton:pressed { background: #b35a3f; }"
        "QToolButton:disabled { background: rgba(127,127,127,0.25); "
        "                       color: palette(mid); }");
    connect(m_sendButton, &QToolButton::clicked, this, &QClaudePanel::onSendClicked);
    bottomBar->addWidget(m_sendButton);

    m_stopButton = makeIconToolButton(QStringLiteral("■"), Tr::tr("Stop"));
    m_stopButton->setMinimumSize(32, 28);
    m_stopButton->setStyleSheet(
        "QToolButton { background: #b91c1c; color: white; border: none; "
        "              border-radius: 6px; padding: 4px 10px; "
        "              font-weight: 600; }"
        "QToolButton:hover { background: #991b1b; }");
    m_stopButton->setVisible(false);
    connect(m_stopButton, &QToolButton::clicked, this, &QClaudePanel::onStopClicked);
    bottomBar->addWidget(m_stopButton);

    inputBoxLayout->addLayout(bottomBar);

    auto *inputWrap = new QWidget(this);
    auto *inputWrapLayout = new QVBoxLayout(inputWrap);
    inputWrapLayout->setContentsMargins(14, 8, 14, 12);
    inputWrapLayout->addWidget(inputBox);
    root->addWidget(inputWrap);

    // ---- Login + status ----
    m_loginButton = new QPushButton(Tr::tr("Log in to Claude"), this);
    m_loginButton->setVisible(false);
    auto *loginWrap = new QWidget(this);
    auto *loginLayout = new QVBoxLayout(loginWrap);
    loginLayout->setContentsMargins(8, 0, 8, 4);
    loginLayout->addWidget(m_loginButton);
    root->addWidget(loginWrap);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: palette(mid); font-size: 11px; padding: 0 8px 4px 8px;");
    root->addWidget(m_statusLabel);

    // ---- Process ----
    m_proc = new ClaudeProcess(this);
    {
        auto &cfg = QClaudeSettings::instance();
        if (!cfg.executablePath().isEmpty())
            m_proc->setExecutablePath(cfg.executablePath());
        m_proc->setModel(cfg.defaultModel());

        // Headless `-p` mode has no terminal to interactively prompt. The
        // chip drives whether claude runs with `--permission-prompt-tool`
        // (gate active → mode `default`) or `acceptEdits` (gate off →
        // claude auto-accepts file modifications and never asks). `plan`
        // and `bypassPermissions` are forwarded verbatim — the gate is
        // not applicable for those.
        const QString settingsMode = cfg.defaultPermissionMode();
        m_askBeforeEdits = (settingsMode.isEmpty()
                            || settingsMode == QStringLiteral("default"));

        // The actual gate wiring (locating the helper, starting the hook
        // server, setting mcp-config / permission-prompt-tool) only
        // happens once the panel is fully constructed and the toggle is
        // flipped on — see onAskBeforeEditsToggled. Initial run-mode is
        // chosen pessimistically: if the chip will be on, use `default`
        // so the prompt tool fires when it's wired up.
        const QString runMode = resolveRunMode(settingsMode, m_askBeforeEdits);
        m_proc->setPermissionMode(runMode);

        QSignalBlocker blocker(m_askEditsTb);
        m_askEditsTb->setChecked(m_askBeforeEdits);
        m_askEditsTb->setText(m_askBeforeEdits ? Tr::tr("✋  Ask before edits")
                                               : Tr::tr("⚡  Auto-accept edits"));

        connect(&cfg, &QClaudeSettings::changed, this, [this]() {
            auto &c = QClaudeSettings::instance();
            if (!c.executablePath().isEmpty())
                m_proc->setExecutablePath(c.executablePath());
            m_proc->setModel(c.defaultModel());
            m_proc->setPermissionMode(
                resolveRunMode(c.defaultPermissionMode(), m_askBeforeEdits));
        });
    }
    connect(m_proc, &ClaudeProcess::systemInit,       this, &QClaudePanel::onSystemInit);
    connect(m_proc, &ClaudeProcess::assistantText,    this, &QClaudePanel::onAssistantText);
    connect(m_proc, &ClaudeProcess::thinking,         this, &QClaudePanel::onThinking);
    connect(m_proc, &ClaudeProcess::toolUse,          this, &QClaudePanel::onToolUse);
    connect(m_proc, &ClaudeProcess::toolResult,       this, &QClaudePanel::onToolResult);
    connect(m_proc, &ClaudeProcess::finished,         this, &QClaudePanel::onFinished);
    connect(m_proc, &ClaudeProcess::authError,        this, &QClaudePanel::onAuthError);
    connect(m_proc, &ClaudeProcess::errorOccurred,    this, &QClaudePanel::onErrorOccurred);
    connect(m_proc, &ClaudeProcess::editToolApplied,  this, &QClaudePanel::onEditToolApplied);
    connect(m_proc, &ClaudeProcess::usageReport, this,
            [this](int contextTokens, int outputTokens, double costUsd) {
        Q_UNUSED(outputTokens);
        m_lastContextTokens = contextTokens;
        m_lastChatCost = costUsd;
        if (contextTokens > m_usageMaxContext)
            m_usageMaxContext = qMax(contextTokens + contextTokens / 4, 200000);
        refreshUsageTooltip();
        refreshUsageLabel();
        QClaudeSettings::instance().setLastUsageForProject(
            m_activeProjectKey, m_lastContextTokens, m_usageMaxContext,
            m_rateLimitResetEpoch);
    });
    connect(m_proc, &ClaudeProcess::rateLimitInfo, this,
            [this](qint64 resetEpochSec) {
        m_rateLimitResetEpoch = resetEpochSec;
        refreshUsageLabel();
        QClaudeSettings::instance().setLastUsageForProject(
            m_activeProjectKey, m_lastContextTokens, m_usageMaxContext,
            m_rateLimitResetEpoch);
    });

    // ---- Pre-flight permission gate (MCP --permission-prompt-tool) ----
    // The hook server is constructed eagerly so the chip toggle can flip
    // start/stop without doing setup work each time. Whether it's actually
    // listening depends on `m_askBeforeEdits` — see onAskBeforeEditsToggled.
    m_hookServer = new HookServer(this);
    connect(m_hookServer, &HookServer::permissionRequested,
            this, &QClaudePanel::onPermissionRequested);
    connect(m_hookServer, &HookServer::requestAbandoned,
            this, &QClaudePanel::onPermissionAbandoned);

    // Honour the initial chip state. onAskBeforeEditsToggled wires the
    // claude process up to --mcp-config / --permission-prompt-tool when
    // the server starts.
    if (m_askBeforeEdits)
        onAskBeforeEditsToggled(true);

    // ---- Autocomplete engine ----
    m_autocomplete = new AutocompleteEngine(this);
    {
        auto &cfg = QClaudeSettings::instance();
        if (!cfg.executablePath().isEmpty())
            m_autocomplete->setExecutable(cfg.executablePath());
        m_autocomplete->setModel(cfg.resolvedAutocompleteModel());
        connect(&cfg, &QClaudeSettings::changed, m_autocomplete, [this]() {
            auto &c = QClaudeSettings::instance();
            if (!c.executablePath().isEmpty())
                m_autocomplete->setExecutable(c.executablePath());
            m_autocomplete->setModel(c.resolvedAutocompleteModel());
        });
    }
    // Debounce timer for auto-suggest. 600ms hits a sweet spot between
    // "fires after every keystroke" and "feels laggy".
    m_autocompleteTimer = new QTimer(this);
    m_autocompleteTimer->setSingleShot(true);
    m_autocompleteTimer->setInterval(600);
    connect(m_autocompleteTimer, &QTimer::timeout,
            this, &QClaudePanel::onAutocompleteTimerFired);

    // Re-attach textChanged listener whenever the active editor changes.
    if (auto *em = Core::EditorManager::instance()) {
        connect(em, &Core::EditorManager::currentEditorChanged,
                this, [this](Core::IEditor *) { onActiveEditorChanged(); });
    }
    onActiveEditorChanged();

    // Disable the debouncer when the chip is off; turn back on when on.
    // Toggling on also pre-spawns a warm `claude -p` worker so the first
    // suggestion skips Node/CLI cold start.
    connect(m_autocompleteTb, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            if (m_autocomplete)
                m_autocomplete->prewarm();
        } else {
            m_autocompleteTimer->stop();
            removeGhostText();
        }
    });
    if (m_autocompleteTb && m_autocompleteTb->isChecked() && m_autocomplete)
        m_autocomplete->prewarm();

    auto restoreAutocompleteIdle = [this]() {
        m_autocompleteTb->setProperty("collapsedText", QStringLiteral("✨"));
        m_autocompleteTb->setProperty("expandedText",  Tr::tr("✨  AI Complete"));
        m_autocompleteTb->setText(m_autocompleteTb->underMouse()
                                      ? Tr::tr("✨  AI Complete")
                                      : QStringLiteral("✨"));
    };
    connect(m_autocomplete, &AutocompleteEngine::partial, this,
            [this](const QString &chunk) {
        if (chunk.isEmpty())
            return;
        // Ignore deltas that arrive after the user moved on — same gating as
        // the completion path, so we never grow ghost text in a stale spot.
        auto *editor = TextEditor::BaseTextEditor::currentTextEditor();
        if (!editor)
            return;
        if (auto *doc = editor->document()) {
            if (!m_pendingCompletionFile.isEmpty()
                && doc->filePath().toFSPathString() != m_pendingCompletionFile)
                return;
        }
        auto *widget = editor->editorWidget();
        if (!widget)
            return;

        if (hasGhostText()) {
            // Continuation of the stream we already started inserting.
            if (m_ghostEditor != widget)
                return;
            growGhostText(chunk);
        } else {
            // First chunk — only insert if the caret is still where the
            // user triggered. After insertGhostText runs, the caret is
            // restored to the trigger position so subsequent partial gating
            // continues to pass.
            if (m_pendingCaretPos >= 0
                && widget->textCursor().position() != m_pendingCaretPos)
                return;
            insertGhostText(widget, chunk);
        }
    });
    connect(m_autocomplete, &AutocompleteEngine::completion, this,
            [this, restoreAutocompleteIdle](const QString &text) {
        restoreAutocompleteIdle();
        if (text.isEmpty()) {
            // A streamed-in suggestion may have been entirely fences/prose
            // that the engine stripped out — drop whatever we accumulated
            // so the editor doesn't keep stale ghost text.
            if (hasGhostText())
                removeGhostText();
            m_pendingCompletionFile.clear();
            return;
        }
        auto *editor = TextEditor::BaseTextEditor::currentTextEditor();
        if (!editor) {
            if (hasGhostText())
                removeGhostText();
            m_pendingCompletionFile.clear();
            return;
        }
        if (auto *doc = editor->document()) {
            if (!m_pendingCompletionFile.isEmpty()
                && doc->filePath().toFSPathString() != m_pendingCompletionFile) {
                if (hasGhostText())
                    removeGhostText();
                m_pendingCompletionFile.clear();
                return;
            }
        }
        auto *widget = editor->editorWidget();
        if (!widget) {
            if (hasGhostText())
                removeGhostText();
            m_pendingCompletionFile.clear();
            return;
        }
        // Discard if the caret moved while we were waiting — the suggestion
        // would be inserted at the wrong place.
        if (m_pendingCaretPos >= 0
            && widget->textCursor().position() != m_pendingCaretPos) {
            if (hasGhostText())
                removeGhostText();
            m_pendingCompletionFile.clear();
            return;
        }
        // Clean replace: insertGhostText() removes any partial-streamed ghost
        // first, then inserts the post-processed final text (fences stripped,
        // trailing newlines trimmed). This corrects whatever the deltas
        // approximated mid-stream.
        insertGhostText(widget, text);
        m_pendingCompletionFile.clear();
    });
    connect(m_autocomplete, &AutocompleteEngine::failed, this,
            [this, restoreAutocompleteIdle](const QString &message) {
        restoreAutocompleteIdle();
        appendBlock(railRow(QStringLiteral("dot-red"),
                            QStringLiteral("●"),
                            QStringLiteral("<span class='err-text'>%1</span>")
                                .arg(escape(Tr::tr("Autocomplete failed: %1").arg(message)))));
        m_pendingCompletionFile.clear();
    });
    connect(m_autocomplete, &AutocompleteEngine::usageReport, this,
            [this](int contextTokens, int outputTokens, double costUsd) {
        ++m_acRequestCount;
        m_acTotalTokens += contextTokens + outputTokens;
        m_acTotalCost   += costUsd;
        refreshUsageTooltip();
        refreshUsageLabel();
        // Activity-driven refresh: keep the 5s tick alive whenever AI Complete
        // is producing usage, even if the user later toggles the chip off — so
        // a "resets HH:MM" countdown stays live while completions are flowing.
        if (m_usageRefreshTimer && !m_usageRefreshTimer->isActive())
            m_usageRefreshTimer->start();
    });

    // Periodic refresh of the usage tooltip while AI Complete is enabled —
    // keeps the AC totals visibly fresh even between chat turns.
    m_usageRefreshTimer = new QTimer(this);
    m_usageRefreshTimer->setInterval(5000);
    connect(m_usageRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshUsageTooltip();
        refreshUsageLabel();
    });
    if (m_autocompleteTb && m_autocompleteTb->isChecked())
        m_usageRefreshTimer->start();
    connect(m_autocompleteTb, &QToolButton::toggled, this, [this](bool on) {
        if (on)
            m_usageRefreshTimer->start();
        else
            m_usageRefreshTimer->stop();
        refreshUsageTooltip();
    });

    connect(m_loginButton, &QPushButton::clicked, this, &QClaudePanel::onLoginClicked);

    if (auto *tree = ProjectExplorer::ProjectTree::instance()) {
        connect(tree, &ProjectExplorer::ProjectTree::currentProjectChanged,
                this, &QClaudePanel::onContextChanged);
    }
    if (auto *em = Core::EditorManager::instance()) {
        connect(em, &Core::EditorManager::currentEditorChanged,
                this, &QClaudePanel::onContextChanged);
    }

    updateContextLabel();
    updateCurrentFileChip();
    if (m_filePicker)
        m_filePicker->setProject(ProjectExplorer::ProjectTree::currentProject());

    // Subtle "using <path>" affordance lives in the title's tooltip so it
    // doesn't clutter the welcome state.
    m_titleLabel->setToolTip(Tr::tr("Using: %1").arg(m_proc->executablePath()));

    appendBlock(welcomeBlock());

    m_activeProjectKey = currentProjectKey();
    resumeSavedSessionForCurrentProject();

    // Project tree often resolves *after* the panel is built (session restore
    // races with widget construction). Schedule a re-apply once the event
    // loop has had a chance to settle so the badge picks up the right key.
    QTimer::singleShot(0, this, [this]() {
        const QString k = currentProjectKey();
        if (!k.isEmpty() && k != m_activeProjectKey) {
            m_activeProjectKey = k;
        }
        applyUsageSnapshotForCurrentProject();
    });
}

QClaudePanel::~QClaudePanel() = default;

QString QClaudePanel::welcomeBlock() const
{
    // Centered "empty state" with a themed logo, a bold headline, and a row
    // of suggestion chips that prefill the composer when clicked. Mirrors
    // the Claude Code reference UI but adapted for our chat document.
    return QStringLiteral(
        "<div class='welcome' align='center' "
        "     style='margin: 44px 16px 16px 16px;'>"
        "  <p><img src='qclaude://qclaude' width='56' height='56'/></p>"
        "  <p class='headline' style='margin: 14px 0 6px 0;'>%1</p>"
        "  <p class='lede' style='margin: 0 24px 18px 24px;'>%2</p>"
        "  <p class='chips' style='margin: 8px 0;'>"
        "    <a href='qclaude://prompt/Explain%20what%20this%20file%20does.'>%3</a>"
        "    <a href='qclaude://prompt/Find%20bugs%20or%20edge%20cases%20in%20this%20file.'>%4</a>"
        "    <a href='qclaude://prompt/Add%20unit%20tests%20for%20this%20file.'>%5</a>"
        "    <a href='qclaude://prompt/Refactor%20this%20file%20for%20clarity.'>%6</a>"
        "  </p>"
        "</div>")
        .arg(escape(Tr::tr("How can I help today?")),
             escape(Tr::tr("Use Claude Code in the terminal to configure "
                            "MCP servers. They'll work here, too!")),
             escape(Tr::tr("Explain this file")),
             escape(Tr::tr("Find bugs")),
             escape(Tr::tr("Add tests")),
             escape(Tr::tr("Refactor")));
}

void QClaudePanel::askAboutFile(const QString &filePath,
                                const QString &selection,
                                int startLine,
                                int endLine)
{
    if (filePath.isEmpty())
        return;

    // Build a relative @ref when the file is inside the working dir.
    QString ref = filePath;
    const QString cwd = currentWorkingDir();
    if (!cwd.isEmpty() && filePath.startsWith(cwd)) {
        ref = filePath.mid(cwd.size());
        if (ref.startsWith(QDir::separator()))
            ref.remove(0, 1);
    }
    if (ref.isEmpty())
        ref = filePath;

    QString fileTag = QStringLiteral("@%1").arg(ref);
    if (startLine > 0 && selection.isEmpty()) {
        if (endLine > 0 && endLine != startLine)
            fileTag += QStringLiteral("#L%1-%2").arg(startLine).arg(endLine);
        else
            fileTag += QStringLiteral("#L%1").arg(startLine);
    }

    QString existing = m_input->toPlainText();
    QString prefill;
    if (!existing.isEmpty() && !existing.endsWith(QLatin1Char('\n')))
        prefill = QStringLiteral("\n\n");
    prefill += fileTag + QLatin1Char(' ');

    if (!selection.trimmed().isEmpty()) {
        QString trimmed = selection;
        if (trimmed.endsWith(QLatin1Char('\n')))
            trimmed.chop(1);
        prefill += QStringLiteral("\n\n```\n%1\n```\n\n").arg(trimmed);
    }

    QTextCursor cur = m_input->textCursor();
    cur.movePosition(QTextCursor::End);
    cur.insertText(prefill);
    m_input->setTextCursor(cur);
    m_input->setFocus();
}

bool QClaudePanel::eventFilter(QObject *obj, QEvent *event)
{
    // Ghost-text controls on the active editor: Tab accepts, anything else
    // (key press or mouse click) dismisses the suggestion.
    if (hasGhostText() && obj == m_ghostEditor) {
        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_Tab && ke->modifiers() == Qt::NoModifier) {
                acceptGhostText();
                return true;
            }
            if (ke->key() == Qt::Key_Escape) {
                dismissAutocomplete();
                return true;
            }
            // Any other key: drop the ghost so the user's keystroke applies
            // to the underlying buffer. Also cancels any in-flight stream so
            // we don't keep generating tokens the user has moved past.
            dismissAutocomplete();
            // fall through — let the original key continue.
        } else if (event->type() == QEvent::MouseButtonPress) {
            dismissAutocomplete();
        }
    }

    if (obj == m_input) {
        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            const bool isEnter = (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter);

            // While the @-mention picker is visible, intercept navigation
            // keys so the user can drive it from the composer without
            // re-focusing.
            if (m_filePicker && m_filePicker->isVisible()) {
                if (ke->key() == Qt::Key_Down) {
                    m_filePicker->moveSelection(+1);
                    return true;
                }
                if (ke->key() == Qt::Key_Up) {
                    m_filePicker->moveSelection(-1);
                    return true;
                }
                if (ke->key() == Qt::Key_Escape) {
                    closeMentionPicker();
                    return true;
                }
                if ((ke->key() == Qt::Key_Tab || isEnter)
                    && !(ke->modifiers() & Qt::ControlModifier)
                    && m_filePicker->hasMatches()) {
                    m_filePicker->acceptCurrent();
                    return true;
                }
            }

            if (isEnter && (ke->modifiers() & Qt::ControlModifier)) {
                onSendClicked();
                return true;
            }
        } else if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            QWidget *box = m_input->parentWidget();
            if (box && box->objectName() == QStringLiteral("QClaude.InputBox")) {
                box->setProperty("focused", event->type() == QEvent::FocusIn);
                box->style()->unpolish(box);
                box->style()->polish(box);
            }
        }
    }

    // Icon-only chips that expand to text on hover. Each chip stashes its
    // collapsed/expanded text via dynamic properties at construction.
    if ((obj == m_autocompleteTb || obj == m_askEditsTb) && obj) {
        auto *btn = qobject_cast<QToolButton *>(obj);
        if (btn) {
            if (event->type() == QEvent::Enter) {
                const QString expanded = btn->property("expandedText").toString();
                if (!expanded.isEmpty())
                    btn->setText(expanded);
            } else if (event->type() == QEvent::Leave) {
                const QString collapsed = btn->property("collapsedText").toString();
                if (!collapsed.isEmpty())
                    btn->setText(collapsed);
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

QString QClaudePanel::currentProjectKey() const
{
    if (auto *p = ProjectExplorer::ProjectTree::currentProject()) {
        const Utils::FilePath fp = p->projectDirectory();
        if (!fp.isEmpty())
            return fp.toFSPathString();
    }
    return currentWorkingDir();
}

void QClaudePanel::resumeSavedSessionForCurrentProject()
{
    applyUsageSnapshotForCurrentProject();

    const QString sid = QClaudeSettings::instance().sessionForProject(m_activeProjectKey);
    if (sid.isEmpty())
        return;
    m_proc->setSessionId(sid);
    appendBlock(QStringLiteral("<div class='sys'>%1</div>")
                    .arg(escape(Tr::tr("Resuming previous session for this project (%1).").arg(sid))));
}

void QClaudePanel::applyUsageSnapshotForCurrentProject()
{
    // Idempotent loader: pulls the per-project usage snapshot from settings
    // and refreshes the header badge. Safe to call repeatedly — used at
    // construction time, on context changes, and on the post-boot singleShot
    // since project resolution often races with panel construction.
    const auto snap =
        QClaudeSettings::instance().lastUsageForProject(m_activeProjectKey);
    if (!snap.valid)
        return;
    m_lastContextTokens = snap.contextTokens;
    if (snap.maxContext > 0)
        m_usageMaxContext = snap.maxContext;
    m_rateLimitResetEpoch = snap.resetEpochSec;
    refreshUsageLabel();
    refreshUsageTooltip();
}

void QClaudePanel::persistCurrentSession()
{
    if (m_activeProjectKey.isEmpty())
        return;
    const QString sid = m_proc->sessionId();
    if (sid.isEmpty())
        return;
    QClaudeSettings::instance().setSessionForProject(m_activeProjectKey, sid);
}

QString QClaudePanel::currentWorkingDir() const
{
    if (auto *p = ProjectExplorer::ProjectTree::currentProject()) {
        const Utils::FilePath fp = p->projectDirectory();
        if (!fp.isEmpty())
            return fp.toFSPathString();
    }
    if (auto *doc = Core::EditorManager::currentDocument()) {
        const Utils::FilePath fp = doc->filePath();
        if (!fp.isEmpty()) {
            QFileInfo fi(fp.toFSPathString());
            return fi.absolutePath();
        }
    }
    return QDir::homePath();
}

void QClaudePanel::updateContextLabel()
{
    QString proj = Tr::tr("(no project)");
    if (auto *p = ProjectExplorer::ProjectTree::currentProject())
        proj = p->displayName();

    QString file;
    if (auto *doc = Core::EditorManager::currentDocument()) {
        const Utils::FilePath fp = doc->filePath();
        if (!fp.isEmpty())
            file = fp.fileName();
    }

    const QString cwd = currentWorkingDir();
    QString line = QStringLiteral("<b>%1</b>").arg(escape(proj));
    if (!file.isEmpty())
        line += QStringLiteral(" · %1").arg(escape(file));
    line += QStringLiteral("<br><span style='color:palette(mid)'>%1</span>").arg(escape(cwd));
    m_contextLabel->setText(line);
}

void QClaudePanel::updateCurrentFileChip()
{
    QString filePath;
    if (auto *doc = Core::EditorManager::currentDocument()) {
        const Utils::FilePath fp = doc->filePath();
        if (!fp.isEmpty())
            filePath = fp.toFSPathString();
    }
    if (filePath.isEmpty()) {
        m_currentFileChip->setVisible(false);
        m_currentFileChip->setProperty("filePath", QString());
        return;
    }
    const QString fileName = QFileInfo(filePath).fileName();
    m_currentFileChip->setText(fileName);
    m_currentFileChip->setProperty("filePath", filePath);
    m_currentFileChip->setVisible(true);
    m_currentFileChip->setToolTip(m_currentFileChip->isChecked()
        ? Tr::tr("Current file is included in the next prompt. Click to detach.\n%1").arg(filePath)
        : Tr::tr("Current file is detached. Click to include it again.\n%1").arg(filePath));
}

void QClaudePanel::onContextChanged()
{
    updateContextLabel();
    updateCurrentFileChip();
    if (m_filePicker)
        m_filePicker->setProject(ProjectExplorer::ProjectTree::currentProject());

    const QString newKey = currentProjectKey();
    if (newKey == m_activeProjectKey) {
        // Same key — but the snapshot may not have been visible yet (e.g.
        // first onContextChanged fires before the panel finished setup).
        applyUsageSnapshotForCurrentProject();
        return;
    }

    // If the user already started chatting in the current session, leave it alone —
    // they can switch via the history menu or "new chat" button.
    if (m_userInteracted) {
        m_activeProjectKey = newKey;
        return;
    }

    // Fresh panel: swap to the saved session for the new project.
    m_activeProjectKey = newKey;
    m_proc->setSessionId(QString());
    m_view->clear();
    m_assistantBuffer.clear();
    m_assistantOpen = false;
    applyTitle(QString());
    // Reset in-memory usage so the badge reflects the new project, not the
    // stale numbers from the previous one. resumeSavedSessionForCurrentProject
    // will refill from persisted state if there's a snapshot.
    m_lastContextTokens   = 0;
    m_rateLimitResetEpoch = 0;
    refreshUsageLabel();
    resumeSavedSessionForCurrentProject();
}

void QClaudePanel::applyTitle(const QString &title)
{
    m_currentTitle = title.trimmed();
    if (m_currentTitle.isEmpty())
        m_titleLabel->setText(Tr::tr("New chat"));
    else
        m_titleLabel->setText(m_currentTitle);
}

void QClaudePanel::setBusy(bool busy)
{
    m_busy = busy;
    m_sendButton->setVisible(!busy);
    m_stopButton->setVisible(busy);
    m_input->setReadOnly(busy);
    m_statusLabel->setText(busy ? Tr::tr("Claude is working…") : QString());

    if (m_usageBar) {
        // Indeterminate-only while busy. Hide entirely when idle so the strip
        // doesn't linger as static "progress" after the turn ends.
        if (busy)
            m_usageBar->setRange(0, 0);
        m_usageBar->setVisible(busy);
    }
}

void QClaudePanel::appendBlock(const QString &html)
{
    QTextCursor cur(m_view->document());
    cur.movePosition(QTextCursor::End);
    cur.insertHtml(html);
    if (auto *bar = m_view->verticalScrollBar())
        bar->setValue(bar->maximum());
}

void QClaudePanel::appendUserMessage(const QString &markdown)
{
    QString body = escape(markdown);
    body.replace('\n', "<br>");
    const QString inner = QStringLiteral(
        "<span class='label'>%1</span><br>%2")
        .arg(escape(Tr::tr("You")), body);
    appendBlock(railRow(QStringLiteral("dot-blue"),
                        QStringLiteral("●"),
                        inner));
}

void QClaudePanel::rerenderTrailingAssistant()
{
    QTextCursor cur(m_view->document());
    cur.movePosition(QTextCursor::End);
    QTextDocument tmp;
    tmp.setMarkdown(m_assistantBuffer);
    const QString rendered = tmp.toHtml();
    const QString inner = QStringLiteral(
        "<div data-role='asst-stream'>%1</div>").arg(rendered);
    const QString block = railRowEmpty(inner);
    if (m_assistantOpen) {
        QString html = m_view->toHtml();
        const QString marker = QStringLiteral("data-role=\"asst-stream\"");
        const int idx = html.lastIndexOf(marker);
        if (idx >= 0) {
            // We replace the whole containing rail-row table so the rail
            // stays in sync after the markdown content grows / shrinks.
            const int tableStart = html.lastIndexOf("<table", idx);
            const int tableEnd = html.indexOf("</table>", idx);
            if (tableStart >= 0 && tableEnd >= 0) {
                html.replace(tableStart, tableEnd + 8 - tableStart, block);
                m_view->setHtml(html);
                if (auto *bar = m_view->verticalScrollBar())
                    bar->setValue(bar->maximum());
                return;
            }
        }
    }
    appendBlock(block);
    m_assistantOpen = true;
}

void QClaudePanel::rememberCurrentSessionInHistory()
{
    const QString sid = m_proc->sessionId();
    if (sid.isEmpty())
        return;
    const QString title = m_currentTitle.isEmpty() ? Tr::tr("Untitled chat") : m_currentTitle;
    for (auto &e : m_history) {
        if (e.sessionId == sid) {
            e.title = title;
            return;
        }
    }
    m_history.prepend({sid, title});
    while (m_history.size() > 20)
        m_history.removeLast();
}

void QClaudePanel::onSendClicked()
{
    if (m_busy)
        return;
    QString prompt = m_input->toPlainText().trimmed();
    if (prompt.isEmpty())
        return;

    // Auto-include the current file as @<rel-path> at the top of the prompt
    // when the chip is checked, *unless* the user already mentioned that
    // file with an @ref in the body.
    QString attachedFileLabel;
    if (m_currentFileChip && m_currentFileChip->isVisible() && m_currentFileChip->isChecked()) {
        const QString fp = m_currentFileChip->property("filePath").toString();
        if (!fp.isEmpty()) {
            const QString cwd = currentWorkingDir();
            const QString ref = makeFileRef(fp, cwd);
            if (!prompt.contains(ref) && !prompt.contains(fp)) {
                prompt = ref + QStringLiteral("\n\n") + prompt;
                attachedFileLabel = ref;
            }
        }
    }

    if (m_currentTitle.isEmpty()) {
        QString title = m_input->toPlainText().trimmed();
        const int nl = title.indexOf('\n');
        if (nl >= 0)
            title = title.left(nl);
        if (title.size() > 60)
            title = title.left(60) + QStringLiteral("…");
        applyTitle(title);
    }

    m_input->clear();
    appendUserMessage(prompt);
    if (!attachedFileLabel.isEmpty()) {
        appendBlock(railRowEmpty(QStringLiteral(
            "<span class='sys-text'>%1</span>")
                .arg(escape(Tr::tr("Attached %1 from current editor.").arg(attachedFileLabel)))));
    }
    m_assistantBuffer.clear();
    m_assistantOpen = false;
    m_userInteracted = true;

    m_proc->setWorkingDirectory(currentWorkingDir());
    setBusy(true);
    m_proc->send(prompt);
}

void QClaudePanel::onStopClicked()
{
    m_proc->stop();
}

void QClaudePanel::onNewChatClicked()
{
    if (m_busy)
        m_proc->stop();
    rememberCurrentSessionInHistory();
    QClaudeSettings::instance().clearSessionForProject(m_activeProjectKey);
    QClaudeSettings::instance().clearLastUsageForProject(m_activeProjectKey);
    m_proc->setSessionId(QString());
    m_view->clear();
    m_assistantBuffer.clear();
    m_assistantOpen = false;
    m_userInteracted = false;
    applyTitle(QString());
    appendBlock(welcomeBlock());

    // Reset the usage bar — fresh chat, no tokens yet.
    m_lastContextTokens = 0;
    m_lastChatCost = 0.0;
    m_acRequestCount = 0;
    m_acTotalTokens = 0;
    m_acTotalCost = 0.0;
    m_rateLimitResetEpoch = 0;
    if (m_usageBar)
        m_usageBar->setVisible(false);
    refreshUsageTooltip();
    refreshUsageLabel();
}

void QClaudePanel::onLoginClicked()
{
    appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                    .arg(escape(Tr::tr("Opening browser for Claude login… "
                                        "complete the flow in the terminal that just opened, "
                                        "then come back here.")))));
    QProcess *p = new QProcess(this);
    p->setProgram(m_proc->executablePath());
    p->setArguments({ "auth", "login" });
    connect(p, &QProcess::finished, p, &QProcess::deleteLater);
    p->startDetached();
    m_loginButton->setVisible(false);
}

// Build a relative @ref for filePath, optionally appending a #L line range.
static QString makeFileRef(const QString &filePath,
                           const QString &cwd,
                           int startLine,
                           int endLine)
{
    QString rel = filePath;
    if (!cwd.isEmpty() && filePath.startsWith(cwd)) {
        rel = filePath.mid(cwd.size());
        if (rel.startsWith(QDir::separator()))
            rel.remove(0, 1);
    }
    if (rel.isEmpty())
        rel = filePath;

    QString ref = QStringLiteral("@") + rel;
    if (startLine > 0) {
        if (endLine > 0 && endLine != startLine)
            ref += QStringLiteral("#L%1-%2").arg(startLine).arg(endLine);
        else
            ref += QStringLiteral("#L%1").arg(startLine);
    }
    return ref;
}

void QClaudePanel::insertRefsIntoInput(const QStringList &refs,
                                       const QString &fencedSelection)
{
    if (refs.isEmpty() && fencedSelection.isEmpty())
        return;

    QTextCursor cur = m_input->textCursor();
    cur.movePosition(QTextCursor::End);
    const QString existing = m_input->toPlainText();
    if (!existing.isEmpty() && !existing.endsWith(QLatin1Char(' '))
        && !existing.endsWith(QLatin1Char('\n')))
        cur.insertText(QStringLiteral(" "));

    if (!refs.isEmpty())
        cur.insertText(refs.join(QLatin1Char(' ')) + QLatin1Char(' '));

    if (!fencedSelection.isEmpty()) {
        QString trimmed = fencedSelection;
        if (trimmed.endsWith(QLatin1Char('\n')))
            trimmed.chop(1);
        cur.insertText(QStringLiteral("\n\n```\n%1\n```\n\n").arg(trimmed));
    }

    m_input->setTextCursor(cur);
    m_input->setFocus();
}

void QClaudePanel::updateMentionPicker()
{
    if (!m_filePicker || !m_input)
        return;

    const QTextCursor cur = m_input->textCursor();
    const int pos = cur.position();
    const QString text = m_input->toPlainText();

    // Walk backwards from the caret to find the most recent `@`. Stop at
    // whitespace or another @, which means we're not in a mention.
    int at = -1;
    for (int i = pos - 1; i >= 0; --i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('@')) {
            at = i;
            break;
        }
        if (c.isSpace())
            break;
    }
    if (at < 0) {
        if (m_filePicker->isVisible())
            closeMentionPicker();
        return;
    }

    // Mentions only trigger at the start of a token: the char before `@`
    // must be whitespace or non-existent. Otherwise emails / decorators
    // would pop the picker spuriously.
    if (at > 0) {
        const QChar prev = text.at(at - 1);
        if (!prev.isSpace()) {
            if (m_filePicker->isVisible())
                closeMentionPicker();
            return;
        }
    }

    const QString query = text.mid(at + 1, pos - at - 1);

    // The query token ends at the first whitespace; if anything after the
    // caret is still part of the token (e.g. user moved the caret back into
    // an existing word) we still want to match on what's typed so far —
    // accept appends a single ref and removes the in-flight query.

    m_mentionAnchorPos = at + 1;
    m_filePicker->setQuery(query);
    if (!m_filePicker->isVisible())
        m_filePicker->popupAbove(m_input);
}

void QClaudePanel::closeMentionPicker()
{
    m_mentionAnchorPos = -1;
    if (m_filePicker)
        m_filePicker->hide();
}

void QClaudePanel::acceptMentionPick(const Utils::FilePath &filePath)
{
    if (!m_input || filePath.isEmpty()) {
        closeMentionPicker();
        return;
    }

    const QString cwd = currentWorkingDir();
    const QString abs = filePath.toFSPathString();
    const QString ref = makeFileRef(abs, cwd);

    QTextCursor cur = m_input->textCursor();
    if (m_mentionAnchorPos < 0) {
        // Picker was opened explicitly (no `@` typed) — append at caret.
        const QString existing = m_input->toPlainText();
        if (!existing.isEmpty() && !existing.endsWith(QLatin1Char(' '))
            && !existing.endsWith(QLatin1Char('\n')))
            cur.insertText(QStringLiteral(" "));
        cur.insertText(ref + QLatin1Char(' '));
    } else {
        // Replace `@<typed-query>` with `@<rel-path> ` (the ref already has
        // the leading `@`). The anchor position points to the char *after*
        // `@`, so we step back one to include it in the replacement.
        const int start = m_mentionAnchorPos - 1;
        const int end = cur.position();
        cur.setPosition(start);
        cur.setPosition(end, QTextCursor::KeepAnchor);
        cur.insertText(ref + QLatin1Char(' '));
    }
    m_input->setTextCursor(cur);
    m_input->setFocus();
    closeMentionPicker();
}

void QClaudePanel::openMentionPickerExplicit()
{
    if (!m_filePicker || !m_input)
        return;
    // Explicit open from the + menu: no @-anchor, just show the picker so
    // the user can browse / fuzzy-find. The accepted file is appended to
    // the existing input rather than replacing a typed query.
    m_mentionAnchorPos = -1;
    m_filePicker->setQuery(QString());
    m_filePicker->popupAbove(m_input);
    m_input->setFocus();
}

void QClaudePanel::onAddClicked()
{
    const QString cwd = currentWorkingDir();

    QMenu menu(this);

    auto displayPath = [&cwd](const QString &abs) {
        QString rel = abs;
        if (!cwd.isEmpty() && abs.startsWith(cwd)) {
            rel = abs.mid(cwd.size());
            if (rel.startsWith(QDir::separator()))
                rel.remove(0, 1);
        }
        return rel.isEmpty() ? abs : rel;
    };

    // ---- Current editor's file (with optional selection) ----
    QString currentFile;
    QString currentSelection;
    int selStart = -1;
    int selEnd = -1;
    int caretLine = -1;

    if (auto *editor = TextEditor::BaseTextEditor::currentTextEditor()) {
        if (auto *doc = editor->document())
            currentFile = doc->filePath().toFSPathString();
        const QTextCursor cursor = editor->textCursor();
        caretLine = cursor.blockNumber() + 1;
        if (cursor.hasSelection()) {
            currentSelection = editor->selectedText();
            QTextCursor c = cursor;
            c.setPosition(cursor.selectionStart());
            selStart = c.blockNumber() + 1;
            c.setPosition(cursor.selectionEnd());
            selEnd = c.blockNumber() + 1;
        }
    } else if (auto *doc = Core::EditorManager::currentDocument()) {
        currentFile = doc->filePath().toFSPathString();
    }

    if (!currentFile.isEmpty()) {
        const QString labelFile = displayPath(currentFile);
        QAction *fileAct = menu.addAction(
            Tr::tr("Current file · %1").arg(labelFile));
        connect(fileAct, &QAction::triggered, this, [this, currentFile, cwd]() {
            insertRefsIntoInput({ makeFileRef(currentFile, cwd) });
        });

        if (!currentSelection.trimmed().isEmpty()) {
            QString rangeLabel = (selStart > 0 && selEnd > 0 && selEnd != selStart)
                ? Tr::tr("lines %1–%2").arg(selStart).arg(selEnd)
                : Tr::tr("line %1").arg(qMax(selStart, caretLine));
            QAction *selAct = menu.addAction(
                Tr::tr("Current selection · %1 (%2)").arg(labelFile, rangeLabel));
            const QString fileCopy = currentFile;
            const QString selCopy  = currentSelection;
            const int s = selStart, e = selEnd;
            connect(selAct, &QAction::triggered, this,
                    [this, fileCopy, cwd, s, e, selCopy]() {
                insertRefsIntoInput({ makeFileRef(fileCopy, cwd, s, e) }, selCopy);
            });
        } else if (caretLine > 0) {
            QAction *lineAct = menu.addAction(
                Tr::tr("Current line · %1 (line %2)").arg(labelFile).arg(caretLine));
            const QString fileCopy = currentFile;
            const int line = caretLine;
            connect(lineAct, &QAction::triggered, this,
                    [this, fileCopy, cwd, line]() {
                insertRefsIntoInput({ makeFileRef(fileCopy, cwd, line) });
            });
        }
        menu.addSeparator();
    }

    // ---- Other editors currently open ----
    QList<Core::IDocument *> openDocs = Core::DocumentModel::openedDocuments();
    int openCount = 0;
    QMenu *openMenu = nullptr;
    for (Core::IDocument *doc : openDocs) {
        if (!doc) continue;
        const QString p = doc->filePath().toFSPathString();
        if (p.isEmpty() || p == currentFile)
            continue;
        if (!openMenu)
            openMenu = menu.addMenu(Tr::tr("Open editors"));
        QAction *a = openMenu->addAction(displayPath(p));
        connect(a, &QAction::triggered, this, [this, p, cwd]() {
            insertRefsIntoInput({ makeFileRef(p, cwd) });
        });
        if (++openCount >= 20)
            break;
    }

    // ---- Search files… (fuzzy picker) ----
    if (!menu.isEmpty())
        menu.addSeparator();
    QAction *searchAct = menu.addAction(Tr::tr("Search files in project…   @"));
    connect(searchAct, &QAction::triggered, this,
            &QClaudePanel::openMentionPickerExplicit);

    // ---- Browse… (fall-back full file dialog) ----
    QAction *browseAct = menu.addAction(Tr::tr("Browse files…"));
    connect(browseAct, &QAction::triggered, this, [this, cwd]() {
        const QStringList files = QFileDialog::getOpenFileNames(
            this, Tr::tr("Add files as context"), cwd);
        if (files.isEmpty())
            return;
        QStringList refs;
        refs.reserve(files.size());
        for (const QString &f : files)
            refs << makeFileRef(f, cwd);
        insertRefsIntoInput(refs);
    });

    QAction *cwdAct = menu.addAction(
        Tr::tr("Add working directory · %1").arg(cwd.isEmpty() ? QStringLiteral("?") : cwd));
    connect(cwdAct, &QAction::triggered, this, [this, cwd]() {
        if (cwd.isEmpty())
            return;
        insertRefsIntoInput({ QStringLiteral("@%1").arg(cwd) });
    });

    menu.exec(m_addTb->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
}

void QClaudePanel::onCommandsClicked()
{
    QMenu menu(this);

    auto addSectionHeader = [&menu](const QString &title) {
        QAction *h = menu.addSection(title);
        Q_UNUSED(h);
    };

    auto addInsertSlash = [this, &menu](const QString &cmd, const QString &help) {
        QAction *a = menu.addAction(QStringLiteral("%1  —  %2").arg(cmd, help));
        connect(a, &QAction::triggered, this, [this, cmd]() {
            QTextCursor cur = m_input->textCursor();
            cur.insertText(cmd + QLatin1Char(' '));
            m_input->setTextCursor(cur);
            m_input->setFocus();
        });
        return a;
    };

    // ---- Context ----
    addSectionHeader(Tr::tr("Context"));
    {
        QAction *attach = menu.addAction(Tr::tr("Attach file…"));
        connect(attach, &QAction::triggered, this, &QClaudePanel::onAddClicked);
    }
    addInsertSlash(QStringLiteral("/clear"), Tr::tr("Clear conversation"));
    {
        QAction *newChat = menu.addAction(Tr::tr("New chat"));
        connect(newChat, &QAction::triggered, this, &QClaudePanel::onNewChatClicked);
    }

    // ---- Model ----
    addSectionHeader(Tr::tr("Model"));
    {
        auto &cfg = QClaudeSettings::instance();

        struct ModelOption { QString id; QString label; };
        const QList<ModelOption> models = {
            { QStringLiteral("claude-opus-4-7"),   Tr::tr("Opus 4.7  ·  1M context") },
            { QStringLiteral("claude-sonnet-4-6"), Tr::tr("Sonnet 4.6") },
            { QStringLiteral("claude-haiku-4-5"),  Tr::tr("Haiku 4.5  ·  fastest") },
            { QString(),                            Tr::tr("(CLI default)") },
        };

        // ---- Chat: Switch model… ----
        const QString chatCurrent = cfg.defaultModel().isEmpty()
            ? Tr::tr("(CLI default)")
            : cfg.defaultModel();
        QMenu *chatMenu = menu.addMenu(Tr::tr("Chat: Switch model…   %1").arg(chatCurrent));
        for (const ModelOption &m : models) {
            const QString labelLine = m.id.isEmpty()
                ? m.label
                : QStringLiteral("%1   %2").arg(m.label, m.id);
            QAction *a = chatMenu->addAction(labelLine);
            a->setCheckable(true);
            a->setChecked(m.id == cfg.defaultModel());
            const QString id = m.id;
            const QString label = m.label;
            connect(a, &QAction::triggered, this, [this, id, label]() {
                QClaudeSettings::instance().setDefaultModel(id);
                m_proc->setModel(id);
                appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                    .arg(escape(Tr::tr("Chat model → %1")
                                    .arg(id.isEmpty() ? Tr::tr("(CLI default)") : label)))));
            });
        }

        // ---- AI Complete: model picker ----
        // Independent of the chat model. Empty == inherit from chat model.
        const QString acm = cfg.autocompleteModel();
        QString acCurrent;
        if (acm.isEmpty()) {
            acCurrent = Tr::tr("(use chat model)");
        } else {
            acCurrent = acm;
            for (const ModelOption &m : models) {
                if (m.id == acm) { acCurrent = m.label; break; }
            }
        }
        QMenu *acMenu = menu.addMenu(Tr::tr("AI Complete model…   %1").arg(acCurrent));

        // Sentinel "use chat model" first, then the same Claude lineup.
        {
            QAction *a = acMenu->addAction(Tr::tr("Use chat model"));
            a->setCheckable(true);
            a->setChecked(acm.isEmpty());
            connect(a, &QAction::triggered, this, [this]() {
                QClaudeSettings::instance().setAutocompleteModel(QString());
                appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                    .arg(escape(Tr::tr("AI Complete model → (use chat model)")))));
            });
            acMenu->addSeparator();
        }
        for (const ModelOption &m : models) {
            if (m.id.isEmpty())
                continue; // CLI-default doesn't make sense for an explicit override
            const QString labelLine = QStringLiteral("%1   %2").arg(m.label, m.id);
            QAction *a = acMenu->addAction(labelLine);
            a->setCheckable(true);
            a->setChecked(m.id == acm);
            const QString id = m.id;
            const QString label = m.label;
            connect(a, &QAction::triggered, this, [this, id, label]() {
                QClaudeSettings::instance().setAutocompleteModel(id);
                appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                    .arg(escape(Tr::tr("AI Complete model → %1").arg(label)))));
            });
        }
    }
    addInsertSlash(QStringLiteral("/cost"), Tr::tr("Show usage / cost"));

    // ---- Account ----
    addSectionHeader(Tr::tr("Account"));
    addInsertSlash(QStringLiteral("/login"),  Tr::tr("Sign in"));
    addInsertSlash(QStringLiteral("/logout"), Tr::tr("Sign out"));
    addInsertSlash(QStringLiteral("/status"), Tr::tr("Show status"));

    // ---- Help ----
    addSectionHeader(Tr::tr("Help"));
    addInsertSlash(QStringLiteral("/help"), Tr::tr("Claude Code help"));
    {
        QAction *settings = menu.addAction(Tr::tr("QClaude settings…"));
        connect(settings, &QAction::triggered, this, []() {
            Core::ICore::showSettings(Utils::Id("QClaude.Settings"));
        });
    }

    menu.exec(m_commandsTb->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
}

void QClaudePanel::onAskBeforeEditsToggled(bool checked)
{
    // When the chip is on we route claude through
    // `--permission-prompt-tool mcp__qclaude_perm__permission_prompt`
    // (with `--mcp-config` pointing at qclaude_mcp_permission). The
    // helper relays each pre-flight permission request to the panel via
    // QLocalSocket; the panel renders an Allow/Deny card and Deny means
    // the tool is *never invoked* — a true pre-flight gate that also
    // covers MCP tool calls (`mcp__server__tool`), not just Edit/Write.
    //
    // claude's CLI mode also has to flip to `default` for the prompt
    // tool to fire on file modifications — `acceptEdits` short-circuits
    // before consulting the prompt tool. When the chip is off we drop
    // the prompt tool and revert to `acceptEdits` (or whatever the
    // settings mode was, for plan / bypassPermissions).
    m_askBeforeEdits = checked;

    bool gateActive = false;
    if (m_hookServer && m_proc) {
        if (checked) {
            // Locate the MCP helper next to the loaded plugin .so. We
            // rely on the PluginManager rather than hard-coded paths so
            // this works for both the build tree (lib/qtcreator/plugins)
            // and the installed location (~/.local/share/.../plugins/<id>).
            QString helper;
            for (ExtensionSystem::PluginSpec *spec
                 : ExtensionSystem::PluginManager::plugins()) {
                if (spec && spec->name() == QStringLiteral("QClaude")) {
                    const Utils::FilePath fp = spec->filePath();
                    QString name = QStringLiteral("qclaude_mcp_permission");
#ifdef Q_OS_WIN
                    name += QStringLiteral(".exe");
#endif
                    helper = fp.parentDir().pathAppended(name).toFSPathString();
                    break;
                }
            }
            if (!helper.isEmpty() && m_hookServer->start(helper)) {
                m_proc->setMcpConfigFile(m_hookServer->mcpConfigFilePath());
                m_proc->setPermissionPromptTool(HookServer::permissionPromptToolName());
                gateActive = true;
            } else {
                // Helper missing or server failed — fall back to the
                // post-hoc Yes/No flow so the chip still feels meaningful.
                m_proc->setMcpConfigFile(QString());
                m_proc->setPermissionPromptTool(QString());
            }
        } else {
            m_hookServer->stop();
            m_proc->setMcpConfigFile(QString());
            m_proc->setPermissionPromptTool(QString());
        }

        // Re-resolve the CLI permission mode in light of the new chip
        // state. `default` is required for `--permission-prompt-tool` to
        // fire on Edit/Write; otherwise we keep the user's preferred
        // run-mode (acceptEdits / plan / bypassPermissions).
        m_proc->setPermissionMode(
            resolveRunMode(QClaudeSettings::instance().defaultPermissionMode(),
                           gateActive));
    }

    const QString collapsed = checked ? QStringLiteral("✋")
                                      : QStringLiteral("⚡");
    const QString expanded  = checked ? Tr::tr("✋  Ask before edits")
                                      : Tr::tr("⚡  Auto-accept edits");
    m_askEditsTb->setProperty("collapsedText", collapsed);
    m_askEditsTb->setProperty("expandedText",  expanded);
    m_askEditsTb->setText(m_askEditsTb->underMouse() ? expanded : collapsed);
}

// Compact one-line summary of what claude wants to do, for the Allow/Deny
// card. Matches the post-hoc Edit/Write phrasing so the chat reads as one
// continuous narrative.
static QString summarizePreToolUse(const QString &tool,
                                   const QJsonObject &input,
                                   const QString &cwd)
{
    auto rel = [&cwd](const QString &abs) {
        if (cwd.isEmpty() || !abs.startsWith(cwd))
            return abs;
        QString r = abs.mid(cwd.size());
        if (r.startsWith(QLatin1Char('/')) || r.startsWith(QLatin1Char('\\')))
            r.remove(0, 1);
        return r.isEmpty() ? abs : r;
    };

    if (tool == QStringLiteral("Edit")) {
        const QString fp = rel(input.value(QStringLiteral("file_path")).toString());
        return QObject::tr("Claude wants to <b>edit</b> %1.")
            .arg(fp.toHtmlEscaped());
    }
    if (tool == QStringLiteral("Write")) {
        const QString fp = rel(input.value(QStringLiteral("file_path")).toString());
        return QObject::tr("Claude wants to <b>write</b> %1.")
            .arg(fp.toHtmlEscaped());
    }
    return QObject::tr("Claude wants to call <b>%1</b>.")
        .arg(tool.toHtmlEscaped());
}

void QClaudePanel::onPermissionRequested(const QString &id,
                                         const QString &toolName,
                                         const QJsonObject &toolInput,
                                         const QString &cwd)
{
    m_pendingPermissions.insert(id, toolName);

    const QString summary = summarizePreToolUse(toolName, toolInput, cwd);
    const QString allowHref = QStringLiteral("qclaude://hook-allow/%1").arg(id);
    const QString denyHref  = QStringLiteral("qclaude://hook-deny/%1").arg(id);

    // Inline buttons rendered as anchors — QTextBrowser's anchorClicked
    // signal carries them back to onAnchorClicked. Style mirrors the warm
    // Claude accent for Allow and a muted red for Deny.
    const QString html = QStringLiteral(
        "<div style='margin: 4px 0;'>"
        "  <div style='margin-bottom: 6px;'>%1</div>"
        "  <a href='%2' style='background: #d97757; color: white; "
        "                       padding: 3px 12px; border-radius: 6px; "
        "                       text-decoration: none; font-weight: 600;'>"
        "    %4</a>"
        "  &nbsp;"
        "  <a href='%3' style='background: rgba(239,83,80,0.15); "
        "                       color: #ef5350; padding: 3px 12px; "
        "                       border-radius: 6px; text-decoration: none; "
        "                       font-weight: 600;'>"
        "    %5</a>"
        "</div>")
        .arg(summary, allowHref, denyHref,
             escape(Tr::tr("Allow")),
             escape(Tr::tr("Deny")));

    appendBlock(railRow(QStringLiteral("dot-gray"),
                        QStringLiteral("?"),
                        html));
}

void QClaudePanel::onPermissionAbandoned(const QString &id)
{
    if (!m_pendingPermissions.contains(id))
        return;
    m_pendingPermissions.remove(id);
    appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                    .arg(escape(Tr::tr("Permission request abandoned (claude exited)")))));
}

void QClaudePanel::respondToPermission(const QString &id, bool allow)
{
    if (!m_hookServer)
        return;
    if (!m_pendingPermissions.contains(id))
        return; // stale click on an already-answered card
    const QString tool = m_pendingPermissions.take(id);

    m_hookServer->respond(id,
                          allow ? QStringLiteral("allow")
                                : QStringLiteral("deny"),
                          allow ? QStringLiteral("user approved")
                                : QStringLiteral("user declined"));

    appendBlock(railRowEmpty(
        allow
            ? QStringLiteral("<span class='sys-text'>%1</span>")
                  .arg(escape(Tr::tr("Allowed %1").arg(tool)))
            : QStringLiteral("<span class='reverted'>%1</span>")
                  .arg(escape(Tr::tr("Denied %1").arg(tool)))));
}

void QClaudePanel::onHistoryClicked()
{
    rememberCurrentSessionInHistory();

    QMenu menu(this);
    if (m_history.isEmpty()) {
        QAction *empty = menu.addAction(Tr::tr("(no past sessions)"));
        empty->setEnabled(false);
    } else {
        for (const ChatHistoryEntry &e : m_history) {
            QAction *act = menu.addAction(e.title);
            const QString sid = e.sessionId;
            const QString title = e.title;
            connect(act, &QAction::triggered, this, [this, sid, title]() {
                if (m_busy)
                    m_proc->stop();
                m_proc->setSessionId(sid);
                m_view->clear();
                m_assistantBuffer.clear();
                m_assistantOpen = false;
                applyTitle(title);
                appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                                .arg(escape(Tr::tr("Resuming session %1").arg(sid)))));
            });
        }
    }
    menu.exec(m_historyTb->mapToGlobal(QPoint(0, m_historyTb->height())));
}

void QClaudePanel::onSystemInit(const QString &sessionId, const QString &cwd, const QString &model)
{
    Q_UNUSED(sessionId);
    appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                    .arg(escape(Tr::tr("Session started · model %1 · cwd %2")
                                    .arg(model.isEmpty() ? Tr::tr("(default)") : model,
                                         cwd)))));
    persistCurrentSession();
}

void QClaudePanel::onAssistantText(const QString &text)
{
    m_assistantBuffer += text;
    rerenderTrailingAssistant();
}

void QClaudePanel::onThinking(const QString &text)
{
    QString preview = text.trimmed();
    const int nl = preview.indexOf('\n');
    if (nl >= 0)
        preview = preview.left(nl);
    if (preview.size() > 200)
        preview = preview.left(200) + "…";

    const QString inner = QStringLiteral(
        "<span class='label muted'>%1</span> <span class='caret'>›</span>"
        "<br><span class='think-text'>%2</span>")
        .arg(escape(Tr::tr("Thinking")), escape(preview));
    appendBlock(railRow(QStringLiteral("dot-gray"),
                        QStringLiteral("●"),
                        inner));
    m_assistantOpen = false;
}

void QClaudePanel::onToolUse(const QString &name, const QString &summary)
{
    QString inner;
    if (name == QStringLiteral("Bash")) {
        // Show command in an IN row of a code card. The OUT row is appended
        // by onToolResult by patching the trailing tool-card data-marker.
        inner = QStringLiteral(
            "<span class='label'>%1</span> <span class='muted'>%2</span>"
            "<div class='tool-card' data-role='bash-card'>"
            "  <div><span class='badge'>IN</span><span>%3</span></div>"
            "</div>")
            .arg(escape(name),
                 escape(Tr::tr("Run shell command")),
                 escape(summary));
    } else {
        // Generic tool: name + monospace argument summary. Wrap in a marker
        // span so onToolResult can attach the result line directly under it
        // as a muted sub-row (no OUT badge), matching the dashboard layout.
        inner = QStringLiteral(
            "<div data-role='tool-row'>"
            "<span class='label'>%1</span> <span class='mono muted'>%2</span>"
            "</div>")
            .arg(escape(name), escape(summary));
    }
    appendBlock(railRow(QStringLiteral("dot-green"),
                        QStringLiteral("●"),
                        inner));
    m_assistantOpen = false;
}

void QClaudePanel::onToolResult(const QString &summary, bool isError)
{
    if (isError) {
        appendBlock(railRow(QStringLiteral("dot-red"),
                            QStringLiteral("●"),
                            QStringLiteral("<span class='err-text mono'>%1</span>")
                                .arg(escape(summary))));
        m_assistantOpen = false;
        return;
    }

    // Try to attach OUT to the most recent Bash card if there is one.
    QString html = m_view->toHtml();
    const QString bashMarker = QStringLiteral("data-role=\"bash-card\"");
    const int bashIdx = html.lastIndexOf(bashMarker);

    // For non-Bash tools, attach a muted sub-line directly under the tool row
    // (no OUT badge) — produces "Glob pattern: ..." / "Found 1 file" stacks.
    const QString toolMarker = QStringLiteral("data-role=\"tool-row\"");
    const int toolIdx = html.lastIndexOf(toolMarker);

    auto rerenderAndScroll = [&](const QString &updated) {
        m_view->setHtml(updated);
        if (auto *bar = m_view->verticalScrollBar())
            bar->setValue(bar->maximum());
    };

    if (bashIdx >= 0 && bashIdx > toolIdx) {
        const int divEnd = html.indexOf("</div>", bashIdx);
        if (divEnd >= 0) {
            const QString outRow = QStringLiteral(
                "<div><span class='badge'>OUT</span><span>%1</span></div>")
                .arg(escape(summary));
            html.insert(divEnd, outRow);
            rerenderAndScroll(html);
            QString cleaned = m_view->toHtml();
            cleaned.replace(QStringLiteral("data-role=\"bash-card\""),
                            QStringLiteral("data-role=\"bash-card-done\""),
                            Qt::CaseSensitive);
            m_view->setHtml(cleaned);
            m_assistantOpen = false;
            return;
        }
    }

    if (toolIdx >= 0) {
        const int divEnd = html.indexOf("</div>", toolIdx);
        if (divEnd >= 0) {
            const QString sub = QStringLiteral(
                "<div class='tool-sub'>%1</div>")
                .arg(escape(summary));
            html.insert(divEnd, sub);
            rerenderAndScroll(html);
            QString cleaned = m_view->toHtml();
            cleaned.replace(QStringLiteral("data-role=\"tool-row\""),
                            QStringLiteral("data-role=\"tool-row-done\""),
                            Qt::CaseSensitive);
            m_view->setHtml(cleaned);
            m_assistantOpen = false;
            return;
        }
    }

    // Fallback: render as a small standalone muted row.
    appendBlock(railRowEmpty(QStringLiteral(
        "<span class='tool-sub'>%1</span>")
            .arg(escape(summary))));
    m_assistantOpen = false;
}

void QClaudePanel::onFinished(const QString &finalText, double costUsd, qint64 durationMs, bool success)
{
    Q_UNUSED(finalText);
    setBusy(false);
    if (success) {
        QString stats;
        if (durationMs > 0 || costUsd > 0)
            stats = Tr::tr("done · %1 ms · $%2")
                        .arg(durationMs)
                        .arg(QString::number(costUsd, 'f', 4));
        else
            stats = Tr::tr("done");
        m_statusLabel->setText(stats);
    }
    m_assistantOpen = false;
    rememberCurrentSessionInHistory();
    persistCurrentSession();
}

void QClaudePanel::onAuthError(const QString &detail)
{
    setBusy(false);
    appendBlock(railRow(QStringLiteral("dot-red"),
                        QStringLiteral("●"),
                        QStringLiteral("<span class='err-text'>%1</span>")
                            .arg(escape(Tr::tr("Authentication required. %1").arg(detail)))));
    m_loginButton->setVisible(true);
}

void QClaudePanel::onErrorOccurred(const QString &message)
{
    setBusy(false);
    appendBlock(railRow(QStringLiteral("dot-red"),
                        QStringLiteral("●"),
                        QStringLiteral("<span class='err-text'>%1</span>")
                            .arg(escape(message))));
}

void QClaudePanel::onEditToolApplied(const QString &tool,
                                     const QString &filePath,
                                     const QString &oldContent,
                                     const QString &newContent,
                                     const QString &editOld,
                                     const QString &editNew)
{
    EditCardEntry e;
    e.tool         = tool;
    e.filePath     = filePath;
    e.oldContent   = oldContent;
    e.newContent   = newContent;
    e.editOld      = editOld;
    e.editNew      = editNew;
    e.hadFileBefore = !oldContent.isEmpty();
    const QString id = QString::number(m_nextEditId++);
    m_edits.insert(id, e);

    // Compute the "after" snapshot for stats. For Edit, splice editNew over
    // editOld in oldContent; for Write, the input is the whole new content.
    QString before = oldContent;
    QString after  = oldContent;
    if (tool == QStringLiteral("Edit")) {
        const int idx = oldContent.indexOf(editOld);
        if (idx >= 0)
            after = QString(oldContent).replace(idx, editOld.size(), editNew);
        else
            after = oldContent; // fallback — likely already applied
    } else { // Write
        after = newContent;
    }
    const DiffStats stats = computeDiffStats(before, after);

    QString shortPath = filePath;
    const QString cwd = currentWorkingDir();
    if (!cwd.isEmpty() && filePath.startsWith(cwd)) {
        shortPath = filePath.mid(cwd.size());
        if (shortPath.startsWith(QDir::separator()))
            shortPath.remove(0, 1);
    }

    const QString inner = QStringLiteral(
        "<span class='label'>%2</span> <span class='mono'>%3</span>"
        " <span class='edit-stats-add'>+%4</span>"
        " <span class='edit-stats-del'>−%5</span>"
        "&nbsp;·&nbsp;<a href='qclaude://diff/%1'>%6</a>"
        "&nbsp;·&nbsp;<a href='qclaude://revert/%1'>%7</a>")
        .arg(id,
             escape(tool),
             escape(shortPath),
             QString::number(stats.added),
             QString::number(stats.removed),
             escape(Tr::tr("Show diff")),
             escape(Tr::tr("Revert")));

    appendBlock(railRow(QStringLiteral("dot-red"),
                        QStringLiteral("●"),
                        inner));

    // Inline diff hunk under the card. For Edit, diff just the changed
    // chunk (editOld vs editNew) so the block stays small. For Write, diff
    // the whole pre/post content but cap it at 24 lines — full diff stays
    // available behind the Show diff link.
    QString diffHtml;
    if (tool == QStringLiteral("Edit"))
        diffHtml = renderInlineDiff(editOld, editNew, /*maxLines*/ 32);
    else
        diffHtml = renderInlineDiff(oldContent, newContent, /*maxLines*/ 24);
    if (!diffHtml.isEmpty())
        appendBlock(railRowEmpty(diffHtml));

    m_assistantOpen = false;

    // ---- Non-blocking Yes / No confirmation when "Ask before edits" is on.
    // The dialog does NOT block Claude — it keeps streaming while the prompt
    // is on screen. Yes simply dismisses (no extra log row); No reverts.
    //
    // Suppressed when the hook server is running: the user has already
    // answered Allow/Deny up-front via the pre-flight card, so a redundant
    // "did you really mean it?" QMessageBox just gets in the way.
    const bool hookActive = m_hookServer && m_hookServer->isRunning();
    if (m_askBeforeEdits && !hookActive && m_edits.contains(id)) {
        auto *box = new QMessageBox(this);
        box->setIcon(QMessageBox::Question);
        box->setWindowTitle(Tr::tr("Allow Claude to write?"));
        box->setText(Tr::tr("Claude wants to %1 <b>%2</b>.")
                         .arg(tool == QStringLiteral("Write")
                                  ? Tr::tr("write")
                                  : Tr::tr("edit"),
                              shortPath.toHtmlEscaped()));
        box->setInformativeText(Tr::tr(
            "The change is already on disk because Claude Code applies it "
            "before reporting back. Click <b>No</b> to revert immediately."));
        box->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box->setDefaultButton(QMessageBox::Yes);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setModal(false);

        const QString idCopy = id;
        connect(box, &QMessageBox::finished, this, [this, idCopy](int result) {
            if (!m_edits.contains(idCopy))
                return;
            if (result != QMessageBox::No)
                return; // Yes / closed → silently continue
            EditCardEntry &entry = m_edits[idCopy];
            bool ok = false;
            if (entry.tool == QStringLiteral("Edit"))
                ok = revertEdit(entry.filePath, entry.editOld, entry.editNew);
            else
                ok = revertWrite(entry.filePath, entry.oldContent, entry.hadFileBefore);
            if (ok) {
                entry.reverted = true;
                appendBlock(railRowEmpty(QStringLiteral("<span class='reverted'>%1</span>")
                    .arg(escape(Tr::tr("Denied · reverted %1")
                                    .arg(QFileInfo(entry.filePath).fileName())))));
            } else {
                appendBlock(railRow(QStringLiteral("dot-red"),
                                    QStringLiteral("●"),
                                    QStringLiteral("<span class='err-text'>%1</span>")
                                        .arg(escape(Tr::tr("Could not revert %1").arg(entry.filePath)))));
            }
        });
        box->open();
    }
}

void QClaudePanel::onAnchorClicked(const QUrl &url)
{
    if (url.scheme() != QStringLiteral("qclaude")) {
        QDesktopServices::openUrl(url);
        return;
    }

    const QString host = url.host();
    const QString tail = url.path().mid(1); // strip leading '/'

    // Suggestion-chip click: prefill the composer and focus it.
    if (host == QStringLiteral("prompt")) {
        const QString text = QUrl::fromPercentEncoding(tail.toUtf8());
        m_input->setPlainText(text);
        QTextCursor c = m_input->textCursor();
        c.movePosition(QTextCursor::End);
        m_input->setTextCursor(c);
        m_input->setFocus();
        return;
    }

    // Pre-flight permission card → release the bridge subprocess.
    if (host == QStringLiteral("hook-allow")) {
        respondToPermission(tail, true);
        return;
    }
    if (host == QStringLiteral("hook-deny")) {
        respondToPermission(tail, false);
        return;
    }

    const QString id = tail;
    if (!m_edits.contains(id))
        return;
    EditCardEntry &e = m_edits[id];

    if (host == QStringLiteral("diff")) {
        // Reconstruct before/after for the diff viewer.
        QString before = e.oldContent;
        QString after  = e.oldContent;
        if (e.tool == QStringLiteral("Edit")) {
            const int idx = e.oldContent.indexOf(e.editOld);
            if (idx >= 0)
                after = QString(e.oldContent).replace(idx, e.editOld.size(), e.editNew);
            else // pre-snapshot was already post-edit; flip the perspective
                before = QString(e.oldContent).replace(e.editNew, e.editOld);
        } else {
            after = e.newContent;
        }
        showClaudeDiff(e.filePath, before, after);
        return;
    }

    if (host == QStringLiteral("revert")) {
        if (e.reverted) {
            appendBlock(railRowEmpty(QStringLiteral("<span class='sys-text'>%1</span>")
                            .arg(escape(Tr::tr("Already reverted.")))));
            return;
        }
        bool ok = false;
        if (e.tool == QStringLiteral("Edit"))
            ok = revertEdit(e.filePath, e.editOld, e.editNew);
        else
            ok = revertWrite(e.filePath, e.oldContent, e.hadFileBefore);

        if (ok) {
            e.reverted = true;
            appendBlock(railRowEmpty(QStringLiteral("<span class='reverted'>%1</span>")
                            .arg(escape(Tr::tr("Reverted %1")
                                            .arg(QFileInfo(e.filePath).fileName())))));
        } else {
            appendBlock(railRow(QStringLiteral("dot-red"),
                                QStringLiteral("●"),
                                QStringLiteral("<span class='err-text'>%1</span>")
                                    .arg(escape(Tr::tr("Could not revert %1").arg(e.filePath)))));
        }
        return;
    }
}

bool QClaudePanel::isAutocompleteEnabled() const
{
    return m_autocompleteTb && m_autocompleteTb->isChecked();
}

bool QClaudePanel::triggerAutocompletion()
{
    if (!isAutocompleteEnabled())
        return false;

    auto *editor = TextEditor::BaseTextEditor::currentTextEditor();
    if (!editor)
        return false;

    QString filePath;
    if (auto *doc = editor->document())
        filePath = doc->filePath().toFSPathString();
    if (filePath.isEmpty())
        return false;

    auto *widget = editor->editorWidget();
    if (!widget)
        return false;

    // Tight context window: 100 lines before the caret + 30 lines after.
    // Empirically this is enough for nearly all inline-completion tasks
    // while keeping the prompt small enough to be fast.
    const QString full = widget->toPlainText();
    const int caretPos = editor->textCursor().position();
    QString prefix = full.left(caretPos);
    QString suffix = full.mid(caretPos);

    auto trimToTrailingLines = [](const QString &s, int maxLines, bool keepHead) {
        const QStringList lines = s.split(QLatin1Char('\n'));
        if (lines.size() <= maxLines)
            return s;
        if (keepHead)
            return lines.mid(0, maxLines).join(QLatin1Char('\n'));
        return lines.mid(lines.size() - maxLines).join(QLatin1Char('\n'));
    };
    prefix = trimToTrailingLines(prefix, 100, /*keepHead*/ false);
    suffix = trimToTrailingLines(suffix, 30,  /*keepHead*/ true);

    m_pendingCompletionFile = filePath;
    m_pendingCaretPos = caretPos;
    m_autocompleteTb->setProperty("collapsedText", QStringLiteral("⏳"));
    m_autocompleteTb->setProperty("expandedText",  Tr::tr("⏳  Completing…"));
    m_autocompleteTb->setText(m_autocompleteTb->underMouse()
                                  ? Tr::tr("⏳  Completing…")
                                  : QStringLiteral("⏳"));
    m_autocomplete->requestCompletion(filePath, prefix, suffix);
    return true;
}

// ---- Usage tooltip ----

void QClaudePanel::refreshUsageTooltip()
{
    if (!m_usageBar)
        return;
    const double pct = m_usageMaxContext > 0
        ? (double(m_lastContextTokens) / m_usageMaxContext) * 100.0
        : 0.0;

    QString tip = Tr::tr(
        "Context · %1% used  ·  %L2 / %L3 tokens  ·  $%4")
        .arg(QString::number(pct, 'f', 1))
        .arg(m_lastContextTokens)
        .arg(m_usageMaxContext)
        .arg(QString::number(m_lastChatCost, 'f', 4));

    if (m_autocompleteTb && m_autocompleteTb->isChecked()) {
        tip += QStringLiteral("\n");
        tip += Tr::tr(
            "AI Complete · %1 request%2  ·  ~%L3 tokens  ·  $%4")
            .arg(m_acRequestCount)
            .arg(m_acRequestCount == 1 ? QString() : QStringLiteral("s"))
            .arg(m_acTotalTokens)
            .arg(QString::number(m_acTotalCost, 'f', 4));
    } else if (m_acRequestCount > 0) {
        tip += QStringLiteral("\n");
        tip += Tr::tr(
            "AI Complete (off) · %1 request%2  ·  ~%L3 tokens  ·  $%4")
            .arg(m_acRequestCount)
            .arg(m_acRequestCount == 1 ? QString() : QStringLiteral("s"))
            .arg(m_acTotalTokens)
            .arg(QString::number(m_acTotalCost, 'f', 4));
    }

    m_usageBar->setToolTip(tip);
}

void QClaudePanel::refreshUsageLabel()
{
    if (!m_usageLabel)
        return;
    if (m_lastContextTokens <= 0 && m_rateLimitResetEpoch <= 0) {
        m_usageLabel->setVisible(false);
        m_usageLabel->clear();
        return;
    }

    QStringList parts;
    if (m_lastContextTokens > 0 && m_usageMaxContext > 0) {
        const double pct = (double(m_lastContextTokens) / m_usageMaxContext) * 100.0;
        parts << QStringLiteral("%1%").arg(QString::number(pct, 'f', pct < 10.0 ? 1 : 0));
    }
    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    if (m_rateLimitResetEpoch > nowSec) {
        const QDateTime when = QDateTime::fromSecsSinceEpoch(m_rateLimitResetEpoch).toLocalTime();
        parts << Tr::tr("resets %1").arg(when.toString(QStringLiteral("HH:mm")));
    }

    if (parts.isEmpty()) {
        m_usageLabel->setVisible(false);
        m_usageLabel->clear();
        return;
    }
    m_usageLabel->setText(parts.join(QStringLiteral(" · ")));
    m_usageLabel->setToolTip(m_usageBar ? m_usageBar->toolTip() : QString());
    m_usageLabel->setVisible(true);
}

// ---- Auto-suggest debouncer + ghost text ----

void QClaudePanel::onActiveEditorChanged()
{
    // Detach from any previous editor.
    if (m_attachedEditor) {
        disconnect(m_attachedEditor.data(), &Utils::PlainTextEdit::textChanged,
                   this, &QClaudePanel::onEditorTextChanged);
        m_attachedEditor->removeEventFilter(this);
    }
    removeGhostText();

    auto *editor = TextEditor::BaseTextEditor::currentTextEditor();
    if (!editor)
        return;
    auto *widget = editor->editorWidget();
    if (!widget)
        return;
    m_attachedEditor = widget;
    connect(widget, &Utils::PlainTextEdit::textChanged,
            this, &QClaudePanel::onEditorTextChanged);
    widget->installEventFilter(this);
}

void QClaudePanel::onEditorTextChanged()
{
    if (!isAutocompleteEnabled())
        return;
    if (hasGhostText())
        return; // suggestion already on screen — don't kick a new request

    if (m_attachedEditor) {
        const int caret = m_attachedEditor->textCursor().position();
        const QString full = m_attachedEditor->toPlainText();

        // Skip mid-identifier: the next character to the right of the caret
        // is a letter/digit/underscore. Suggestions inside a word are noisy.
        if (caret < full.size()) {
            const QChar next = full.at(caret);
            if (next.isLetterOrNumber() || next == QLatin1Char('_'))
                return;
        }
        // Skip if the user just typed a plain space — trailing-space pauses
        // are rarely a useful trigger, and waiting for the next non-space
        // keystroke keeps the engine quiet.
        if (caret > 0 && caret <= full.size()
            && full.at(caret - 1) == QLatin1Char(' ')) {
            return;
        }
    }

    m_autocompleteTimer->start();
}

void QClaudePanel::onAutocompleteTimerFired()
{
    if (!isAutocompleteEnabled())
        return;
    if (hasGhostText())
        return;
    if (m_autocomplete && m_autocomplete->isRunning())
        return;
    triggerAutocompletion();
}

void QClaudePanel::insertGhostText(Utils::PlainTextEdit *editor, const QString &text)
{
    if (!editor || text.isEmpty())
        return;
    removeGhostText();

    QTextCursor cur = editor->textCursor();
    const int origPos = cur.position();

    QTextCharFormat fade;
    QColor mid = editor->palette().color(QPalette::Disabled, QPalette::Text);
    if (!mid.isValid())
        mid = QColor(160, 160, 160);
    mid.setAlpha(180);
    fade.setForeground(mid);
    fade.setFontItalic(true);

    cur.beginEditBlock();
    cur.setCharFormat(fade);
    cur.insertText(text);
    const int endPos = cur.position();
    cur.endEditBlock();

    // Move caret back so the user keeps typing where they left off.
    QTextCursor restore = editor->textCursor();
    restore.setPosition(origPos);
    // Reset format so newly-typed chars use the editor's normal style.
    QTextCharFormat reset;
    restore.setCharFormat(reset);
    editor->setTextCursor(restore);

    m_ghostEditor = editor;
    m_ghostStart  = origPos;
    m_ghostEnd    = endPos;
}

void QClaudePanel::growGhostText(const QString &chunk)
{
    if (!hasGhostText() || chunk.isEmpty())
        return;
    Utils::PlainTextEdit *editor = m_ghostEditor;
    if (!editor)
        return;

    QTextCharFormat fade;
    QColor mid = editor->palette().color(QPalette::Disabled, QPalette::Text);
    if (!mid.isValid())
        mid = QColor(160, 160, 160);
    mid.setAlpha(180);
    fade.setForeground(mid);
    fade.setFontItalic(true);

    // Insert at the end of the current ghost span. This doesn't move the
    // user's caret because we're using a separate cursor, and the stored
    // m_ghostStart stays put (caret is still parked there for the user to
    // keep typing).
    QTextCursor c(editor->document());
    c.setPosition(m_ghostEnd);
    c.beginEditBlock();
    c.setCharFormat(fade);
    c.insertText(chunk);
    m_ghostEnd = c.position();
    c.endEditBlock();

    // Re-park the visible caret at the trigger position. Inserting via a
    // detached QTextCursor doesn't normally move the editor's cursor, but
    // if the user's caret happened to coincide with m_ghostEnd it would
    // shift — keep it pinned to m_ghostStart so the position-gating in
    // partial/completion handlers stays consistent.
    QTextCursor caret = editor->textCursor();
    if (caret.position() != m_ghostStart) {
        caret.setPosition(m_ghostStart);
        QTextCharFormat reset;
        caret.setCharFormat(reset);
        editor->setTextCursor(caret);
    }
}

void QClaudePanel::dismissAutocomplete()
{
    if (m_autocomplete && m_autocomplete->isRunning())
        m_autocomplete->cancel();
    if (hasGhostText())
        removeGhostText();
    m_pendingCompletionFile.clear();
    m_pendingCaretPos = -1;
}

void QClaudePanel::acceptGhostText()
{
    if (!hasGhostText())
        return;
    Utils::PlainTextEdit *editor = m_ghostEditor;

    QTextCursor c(editor->document());
    c.setPosition(m_ghostStart);
    c.setPosition(m_ghostEnd, QTextCursor::KeepAnchor);

    // QTextDocument selectedText replaces newlines with U+2029 — convert
    // back so insertText reproduces real line breaks.
    QString text = c.selectedText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));

    c.beginEditBlock();
    c.removeSelectedText();
    // Reset the cursor's character format to the editor's default so the
    // syntax highlighter can recolor the inserted text on the next rehighlight
    // pass. Without this, the faded ghost format would persist.
    QTextCharFormat normal;
    c.setCharFormat(normal);
    c.insertText(text);
    c.endEditBlock();

    QTextCursor caret(editor->document());
    caret.setPosition(m_ghostStart + text.length());
    editor->setTextCursor(caret);

    m_ghostEditor = nullptr;
    m_ghostStart = m_ghostEnd = -1;
}

void QClaudePanel::removeGhostText()
{
    if (!hasGhostText())
        return;
    Utils::PlainTextEdit *editor = m_ghostEditor;
    if (editor) {
        QTextCursor c(editor->document());
        if (m_ghostStart >= 0 && m_ghostEnd >= m_ghostStart
            && m_ghostEnd <= editor->document()->characterCount()) {
            c.setPosition(m_ghostStart);
            c.setPosition(m_ghostEnd, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            editor->setTextCursor(c);
        }
    }
    m_ghostEditor = nullptr;
    m_ghostStart = m_ghostEnd = -1;
}

} // namespace QClaude::Internal
