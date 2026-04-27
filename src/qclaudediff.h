#pragma once

#include <QString>

namespace QClaude::Internal {

// Open Qt Creator's diff viewer with the given before/after text for filePath.
// Pre-existing diff documents for the same file are reused.
void showClaudeDiff(const QString &filePath,
                    const QString &before,
                    const QString &after);

// Compute +N/-N stats for a unified line-mode diff.
struct DiffStats { int added = 0; int removed = 0; };
DiffStats computeDiffStats(const QString &before, const QString &after);

// Revert helpers.
// For Edit: replaces the first occurrence of `editNew` with `editOld`.
// For Write: rewrites filePath with the snapshotted oldContent (or removes
// the file if oldContent is empty and the file was newly created).
bool revertEdit(const QString &filePath,
                const QString &editOld,
                const QString &editNew);
bool revertWrite(const QString &filePath,
                 const QString &oldContent,
                 bool hadFileBefore);

} // namespace QClaude::Internal
