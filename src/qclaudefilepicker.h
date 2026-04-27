#pragma once

#include <QFrame>
#include <QPointer>
#include <QString>
#include <QVector>

#include <utils/filepath.h>

QT_BEGIN_NAMESPACE
class QListWidget;
class QListWidgetItem;
class QLabel;
QT_END_NAMESPACE

namespace ProjectExplorer { class Project; }

namespace QClaude::Internal {

// Inline "@-mention" fuzzy file picker for the composer.
//
// Shown as a frameless popup anchored to the input. Indexes the active
// project's source files lazily and ranks them with a subsequence-style
// fuzzy matcher (consecutive-run + word-start + basename bonuses), so the
// user can type `@<query>` and pick any file in the project without
// touching the mouse.
class FilePicker : public QFrame
{
    Q_OBJECT

public:
    explicit FilePicker(QWidget *parent = nullptr);

    // (Re)load the file list from `project`. Pass nullptr to clear. Cheap
    // because we hold onto the index until the project changes.
    void setProject(ProjectExplorer::Project *project);

    // Rebuild the visible match list for `query`. Empty query → recently
    // opened files first, then everything else alphabetically.
    void setQuery(const QString &query);

    // Position above `anchor` (rooted in the anchor's top-level window) and
    // show. Sized to ~30% of the anchor's window height, capped at 320px.
    void popupAbove(QWidget *anchor);

    // Move highlight up/down within the visible list. Wraps around.
    void moveSelection(int delta);

    // Emit `accepted(...)` for the highlighted entry, if any.
    void acceptCurrent();

    bool hasMatches() const;

signals:
    // The user picked a file. The panel inserts an `@<rel-path>` token at
    // the active mention anchor and dismisses the popup.
    void accepted(const Utils::FilePath &filePath);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Indexed {
        Utils::FilePath path;
        QString rel;       // path relative to project root, with '/'
        QString basename;  // filename only
    };

    void rebuildIndex();
    void rebuildVisible();
    void onItemActivated(QListWidgetItem *item);

    QPointer<ProjectExplorer::Project> m_project;
    Utils::FilePath m_projectRoot;

    QVector<Indexed> m_index;   // full list, built once per project
    QString m_query;

    QListWidget *m_list = nullptr;
    QLabel *m_empty = nullptr;
};

} // namespace QClaude::Internal
