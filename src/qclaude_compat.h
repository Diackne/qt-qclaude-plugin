#pragma once

#include <coreplugin/coreconstants.h>
#include <utils/filepath.h>
#include <utils/id.h>

#include <QString>

namespace QClaude::Compat {

inline Utils::Id settingsCategory()
{
#ifdef QCLAUDE_HAS_SETTINGS_CATEGORY_AI
    return Core::Constants::SETTINGS_CATEGORY_AI;
#else
    return Utils::Id("ZY.AI");
#endif
}

// FilePath::relativePathFromDir() returned Utils::FilePath in Qt Creator <= 18
// and was changed to return QString in Qt Creator 19. Overload resolution picks
// the right path at compile time without preprocessor gates.
inline QString toQString(QString s) { return s; }
inline QString toQString(const Utils::FilePath &fp) { return fp.toFSPathString(); }

inline QString relativePathFromDir(const Utils::FilePath &fp,
                                   const Utils::FilePath &anchorDir)
{
    return toQString(fp.relativePathFromDir(anchorDir));
}

} // namespace QClaude::Compat
