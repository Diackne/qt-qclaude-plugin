#include "qclaudediff.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/icore.h>

#include <diffeditor/diffeditorcontroller.h>
#include <diffeditor/diffenums.h>
#include <diffeditor/diffutils.h>

#include <utils/differ.h>

#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QMenu>

namespace QClaude::Internal {

namespace {

DiffEditor::FileData makeFileData(const QString &filePath,
                                  const QString &before,
                                  const QString &after)
{
    Utils::Differ differ;
    differ.setDiffMode(Utils::Differ::LineMode);
    const QList<Utils::Diff> raw = differ.diff(before, after);

    QList<Utils::Diff> leftDiffs;
    QList<Utils::Diff> rightDiffs;
    Utils::Differ::splitDiffList(raw, &leftDiffs, &rightDiffs);

    const DiffEditor::ChunkData chunk =
        DiffEditor::DiffUtils::calculateOriginalData(leftDiffs, rightDiffs);
    DiffEditor::FileData fd =
        DiffEditor::DiffUtils::calculateContextData(chunk, /*contextLineCount*/ 3);
    fd.fileInfo[DiffEditor::LeftSide]  = DiffEditor::DiffFileInfo(filePath, QStringLiteral("before"));
    fd.fileInfo[DiffEditor::RightSide] = DiffEditor::DiffFileInfo(filePath, QStringLiteral("after"));
    return fd;
}

class QClaudeDiffController : public DiffEditor::DiffEditorController
{
public:
    explicit QClaudeDiffController(Core::IDocument *doc)
        : DiffEditor::DiffEditorController(doc) {}

    void setSource(const QString &filePath, const QString &before)
    {
        m_filePath = filePath;
        m_before   = before;
    }

    void publish(const QList<DiffEditor::FileData> &files)
    {
        setDiffFiles(files);
    }

    void publishFromDisk()
    {
        const QString after = readFile(m_filePath);
        publish({ makeFileData(m_filePath, m_before, after) });
    }

protected:
    void addExtraActions(QMenu *menu, int fileIndex, int chunkIndex,
                         const DiffEditor::ChunkSelection &selection) override
    {
        Q_UNUSED(fileIndex);
        Q_UNUSED(selection);
        if (chunkIndex < 0)
            return;

        QAction *revertChunk = menu->addAction(tr("Revert this chunk (Claude)"));
        connect(revertChunk, &QAction::triggered, this, [this, chunkIndex]() {
            revertChunkAt(chunkIndex);
        });

        QAction *keepChunk = menu->addAction(tr("Keep only this chunk (revert all others)"));
        connect(keepChunk, &QAction::triggered, this, [this, chunkIndex]() {
            keepOnlyChunkAt(chunkIndex);
        });
    }

private:
    static QString readFile(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return QString::fromUtf8(f.readAll());
    }

    static bool writeFile(const QString &path, const QString &text)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return false;
        const qint64 n = f.write(text.toUtf8());
        return n >= 0;
    }

    // Find the actual change region inside a (potentially context-padded)
    // chunk: returns the right-side (current-file) line range to replace and
    // the left-side ("before") lines to splice in.
    struct ChangeRange {
        bool valid = false;
        int rightStart = 0;       // 0-indexed line in current
        int rightCount = 0;
        QStringList leftLines;    // lines from `before` to splice in
    };

    static ChangeRange computeChangeRange(const DiffEditor::ChunkData &chunk)
    {
        ChangeRange r;
        int firstChange = -1, lastChange = -1;
        for (int i = 0; i < chunk.rows.size(); ++i) {
            if (!chunk.rows[i].equal) {
                if (firstChange < 0) firstChange = i;
                lastChange = i;
            }
        }
        if (firstChange < 0)
            return r; // pure-context chunk, nothing to revert

        int leadingLeft = 0, leadingRight = 0;
        for (int i = 0; i < firstChange; ++i) {
            if (chunk.rows[i].line[DiffEditor::LeftSide].textLineType
                == DiffEditor::TextLineData::TextLine)
                ++leadingLeft;
            if (chunk.rows[i].line[DiffEditor::RightSide].textLineType
                == DiffEditor::TextLineData::TextLine)
                ++leadingRight;
        }

        QStringList leftLines;
        int rightCount = 0;
        for (int i = firstChange; i <= lastChange; ++i) {
            const auto &row = chunk.rows[i];
            if (row.line[DiffEditor::LeftSide].textLineType
                == DiffEditor::TextLineData::TextLine)
                leftLines.append(row.line[DiffEditor::LeftSide].text);
            if (row.line[DiffEditor::RightSide].textLineType
                == DiffEditor::TextLineData::TextLine)
                ++rightCount;
        }

        r.valid      = true;
        r.rightStart = chunk.startingLineNumber[DiffEditor::RightSide] + leadingRight;
        r.rightCount = rightCount;
        r.leftLines  = leftLines;
        Q_UNUSED(leadingLeft);
        return r;
    }

    void revertChunkAt(int chunkIndex)
    {
        if (m_filePath.isEmpty())
            return;
        const QString currentText = readFile(m_filePath);
        const DiffEditor::FileData fd = makeFileData(m_filePath, m_before, currentText);
        if (chunkIndex < 0 || chunkIndex >= fd.chunks.size())
            return;

        const ChangeRange r = computeChangeRange(fd.chunks[chunkIndex]);
        if (!r.valid)
            return;

        QStringList lines = currentText.split(QLatin1Char('\n'));
        if (r.rightStart < 0 || r.rightStart > lines.size())
            return;

        QStringList rebuilt;
        rebuilt += lines.mid(0, r.rightStart);
        rebuilt += r.leftLines;
        rebuilt += lines.mid(r.rightStart + r.rightCount);

        if (writeFile(m_filePath, rebuilt.join(QLatin1Char('\n'))))
            publishFromDisk();
    }

    // Revert every chunk except the one the user clicked. Equivalent to
    // "keep only this chunk's change relative to the before snapshot".
    void keepOnlyChunkAt(int chunkIndex)
    {
        if (m_filePath.isEmpty())
            return;
        const QString currentText = readFile(m_filePath);
        const DiffEditor::FileData fd = makeFileData(m_filePath, m_before, currentText);
        if (chunkIndex < 0 || chunkIndex >= fd.chunks.size())
            return;

        // Process from the bottom up so earlier line indices stay stable as
        // we splice replacements into the buffer.
        QStringList lines = currentText.split(QLatin1Char('\n'));
        for (int i = fd.chunks.size() - 1; i >= 0; --i) {
            if (i == chunkIndex)
                continue;
            const ChangeRange r = computeChangeRange(fd.chunks[i]);
            if (!r.valid)
                continue;
            if (r.rightStart < 0 || r.rightStart > lines.size())
                continue;
            QStringList rebuilt;
            rebuilt += lines.mid(0, r.rightStart);
            rebuilt += r.leftLines;
            rebuilt += lines.mid(r.rightStart + r.rightCount);
            lines = rebuilt;
        }

        if (writeFile(m_filePath, lines.join(QLatin1Char('\n'))))
            publishFromDisk();
    }

    QString m_filePath;
    QString m_before;
};

} // namespace

void showClaudeDiff(const QString &filePath,
                    const QString &before,
                    const QString &after)
{
    if (filePath.isEmpty())
        return;

    const QString vcsId = QStringLiteral("QClaude.Diff:") + filePath;
    const QString displayName = QStringLiteral("Claude · %1")
                                    .arg(QFileInfo(filePath).fileName());

    Core::IDocument *doc =
        DiffEditor::DiffEditorController::findOrCreateDocument(vcsId, displayName);
    if (!doc)
        return;

    auto *ctrl = dynamic_cast<QClaudeDiffController *>(
        DiffEditor::DiffEditorController::controller(doc));
    if (!ctrl)
        ctrl = new QClaudeDiffController(doc);

    ctrl->setSource(filePath, before);
    ctrl->publish({ makeFileData(filePath, before, after) });
    Core::EditorManager::activateEditorForDocument(doc);
}

DiffStats computeDiffStats(const QString &before, const QString &after)
{
    Utils::Differ differ;
    differ.setDiffMode(Utils::Differ::LineMode);
    const QList<Utils::Diff> diffs = differ.diff(before, after);

    DiffStats s;
    for (const Utils::Diff &d : diffs) {
        const int lines = d.text.count(QLatin1Char('\n'))
                          + (d.text.isEmpty() || d.text.endsWith(QLatin1Char('\n')) ? 0 : 1);
        if (d.command == Utils::Diff::Insert)
            s.added += lines;
        else if (d.command == Utils::Diff::Delete)
            s.removed += lines;
    }
    return s;
}

bool revertEdit(const QString &filePath,
                const QString &editOld,
                const QString &editNew)
{
    if (filePath.isEmpty() || editNew.isEmpty())
        return false;

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QString text = QString::fromUtf8(f.readAll());
    f.close();

    const int idx = text.indexOf(editNew);
    if (idx < 0)
        return false;
    text.replace(idx, editNew.size(), editOld);

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const qint64 written = f.write(text.toUtf8());
    f.close();
    return written >= 0;
}

bool revertWrite(const QString &filePath,
                 const QString &oldContent,
                 bool hadFileBefore)
{
    if (filePath.isEmpty())
        return false;

    if (!hadFileBefore && oldContent.isEmpty())
        return QFile::remove(filePath);

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const qint64 written = f.write(oldContent.toUtf8());
    f.close();
    return written >= 0;
}

} // namespace QClaude::Internal
