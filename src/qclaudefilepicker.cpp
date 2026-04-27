#include "qclaudefilepicker.h"

#include "qclaude_compat.h"
#include "qclaudetr.h"

#include <projectexplorer/project.h>

#include <utils/filepath.h>

#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace QClaude::Internal {

namespace {

// Subsequence-style fuzzy score. Returns a positive score on match, or -1
// on a non-match (any pattern char that can't be found in order).
//
// Heuristics:
//   * consecutive run of matching chars  → +5 per char in run
//   * word-start match (pos 0 or after `/`, '_', '-', '.', ' ')  → +10
//   * exact-prefix bonus on basename  → +25
//   * basename match outranks path match  → caller boosts basename hits
int fuzzyScore(QStringView pattern, QStringView candidate)
{
    if (pattern.isEmpty())
        return 1;
    if (candidate.size() < pattern.size())
        return -1;

    int score = 0;
    int run = 0;
    qsizetype ci = 0;
    for (qsizetype pi = 0; pi < pattern.size(); ++pi) {
        const QChar pc = pattern[pi].toLower();
        bool found = false;
        while (ci < candidate.size()) {
            const QChar cc = candidate[ci].toLower();
            if (cc == pc) {
                int s = 1;
                if (run > 0)
                    s += 5;
                const QChar prev = ci > 0 ? candidate[ci - 1] : QChar('/');
                if (prev == QChar('/') || prev == QChar('_')
                    || prev == QChar('-') || prev == QChar('.')
                    || prev == QChar(' '))
                    s += 10;
                score += s;
                ++run;
                ++ci;
                found = true;
                break;
            }
            run = 0;
            ++ci;
        }
        if (!found)
            return -1;
    }
    return score;
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QChar('_') || c == QChar('-')
        || c == QChar('.') || c == QChar('/');
}

} // namespace

FilePicker::FilePicker(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("QClaude.FilePicker"));
    // Frameless tooltip-style popup so it floats above the composer without
    // stealing focus from the input.
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral(
        "QClaude--Internal--FilePicker {"
        "  background: palette(base);"
        "  border: 1px solid rgba(217,119,87,0.45);"
        "  border-radius: 8px;"
        "}"
        "QClaude--Internal--FilePicker QListWidget {"
        "  background: transparent;"
        "  border: none;"
        "  outline: 0;"
        "  padding: 4px 0;"
        "}"
        "QClaude--Internal--FilePicker QListWidget::item {"
        "  padding: 4px 10px;"
        "}"
        "QClaude--Internal--FilePicker QListWidget::item:selected {"
        "  background: rgba(217,119,87,0.18);"
        "  color: palette(text);"
        "}"
        "QClaude--Internal--FilePicker QLabel#emptyLabel {"
        "  color: palette(mid);"
        "  padding: 14px;"
        "}"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setAttribute(Qt::WA_MacShowFocusRect, false);
    layout->addWidget(m_list);

    m_empty = new QLabel(Tr::tr("No matches"), this);
    m_empty->setObjectName(QStringLiteral("emptyLabel"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->hide();
    layout->addWidget(m_empty);

    connect(m_list, &QListWidget::itemActivated,
            this, &FilePicker::onItemActivated);
    connect(m_list, &QListWidget::itemClicked,
            this, &FilePicker::onItemActivated);
}

void FilePicker::setProject(ProjectExplorer::Project *project)
{
    if (m_project == project)
        return;
    m_project = project;
    rebuildIndex();
    rebuildVisible();
}

void FilePicker::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    rebuildVisible();
}

void FilePicker::popupAbove(QWidget *anchor)
{
    if (!anchor)
        return;
    if (!m_list->count() && m_query.isEmpty())
        rebuildVisible(); // first show — make sure something's listed

    QWidget *win = anchor->window();
    const int desired = qMin(320, (win ? win->height() : 600) * 30 / 100);
    setFixedSize(qMax(anchor->width(), 360), qMax(160, desired));

    const QPoint topLeft = anchor->mapToGlobal(QPoint(0, 0));
    QPoint where = topLeft - QPoint(0, height() + 4);
    if (where.y() < 0) {
        // Fallback: not enough headroom — drop below.
        where = anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
    }
    move(where);
    show();
    raise();
}

void FilePicker::moveSelection(int delta)
{
    const int n = m_list->count();
    if (n == 0)
        return;
    int row = m_list->currentRow();
    if (row < 0)
        row = 0;
    row = (row + delta) % n;
    if (row < 0)
        row += n;
    m_list->setCurrentRow(row);
}

void FilePicker::acceptCurrent()
{
    if (auto *item = m_list->currentItem())
        onItemActivated(item);
}

bool FilePicker::hasMatches() const
{
    return m_list->count() > 0;
}

bool FilePicker::eventFilter(QObject *, QEvent *)
{
    return false;
}

void FilePicker::hideEvent(QHideEvent *event)
{
    QFrame::hideEvent(event);
}

void FilePicker::rebuildIndex()
{
    m_index.clear();
    m_projectRoot = {};
    if (!m_project)
        return;

    m_projectRoot = m_project->projectDirectory();
    const Utils::FilePaths files
        = m_project->files(ProjectExplorer::Project::SourceFiles);
    m_index.reserve(files.size());
    for (const Utils::FilePath &fp : files) {
        Indexed e;
        e.path = fp;
        e.rel = m_projectRoot.isEmpty()
                    ? fp.toFSPathString()
                    : QClaude::Compat::relativePathFromDir(fp, m_projectRoot);
        if (e.rel.isEmpty())
            e.rel = fp.toFSPathString();
        e.basename = fp.fileName();
        m_index.push_back(std::move(e));
    }

    // Stable alphabetical default order — gives a sensible empty-query view.
    std::sort(m_index.begin(), m_index.end(),
              [](const Indexed &a, const Indexed &b) {
        return a.rel.compare(b.rel, Qt::CaseInsensitive) < 0;
    });
}

void FilePicker::rebuildVisible()
{
    m_list->clear();

    struct Scored {
        const Indexed *e;
        int score;
    };
    QVector<Scored> scored;
    scored.reserve(m_index.size());

    if (m_query.isEmpty()) {
        // No query: show everything in alphabetical order, capped.
        const int cap = qMin<int>(m_index.size(), 200);
        for (int i = 0; i < cap; ++i)
            scored.push_back({&m_index[i], 0});
    } else {
        for (const Indexed &e : m_index) {
            const int sBase = fuzzyScore(m_query, e.basename);
            const int sRel  = fuzzyScore(m_query, e.rel);
            int s = -1;
            if (sBase >= 0)
                s = sBase + 50; // basename hits beat path hits
            if (sRel > s)
                s = sRel;
            if (s >= 0)
                scored.push_back({&e, s});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const Scored &a, const Scored &b) {
            if (a.score != b.score)
                return a.score > b.score;
            return a.e->rel.compare(b.e->rel, Qt::CaseInsensitive) < 0;
        });
        if (scored.size() > 200)
            scored.resize(200);
    }

    for (const Scored &s : scored) {
        auto *it = new QListWidgetItem(s.e->rel, m_list);
        it->setToolTip(s.e->path.toFSPathString());
        // Stash the absolute path on the item so onItemActivated can recover
        // it without holding extra state.
        it->setData(Qt::UserRole, s.e->path.toFSPathString());
    }

    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
        m_list->show();
        m_empty->hide();
    } else {
        m_list->hide();
        m_empty->show();
    }
}

void FilePicker::onItemActivated(QListWidgetItem *item)
{
    if (!item)
        return;
    const QString abs = item->data(Qt::UserRole).toString();
    if (abs.isEmpty())
        return;
    emit accepted(Utils::FilePath::fromString(abs));
}

} // namespace QClaude::Internal
