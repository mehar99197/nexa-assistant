#include "core/ai/ProgramLookup.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

QString findProgram(const QString &name, const QString &configured) {
    if (!configured.isEmpty()) {
        const QFileInfo file(configured);
        return file.isFile() && file.isExecutable() ? file.absoluteFilePath() : QString();
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty()) return onPath;
    return QStandardPaths::findExecutable(name, {QDir::homePath() + u"/.local/bin"_s});
}

} // namespace nexa
