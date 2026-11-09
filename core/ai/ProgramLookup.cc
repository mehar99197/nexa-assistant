#include "core/ai/ProgramLookup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QVersionNumber>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// npm's `prefix=` in ~/.npmrc (where `npm install -g` puts programs for this user), with ~ and ${HOME} expanded.
QString npmPrefix(const QString &home) {
    QFile file(home + u"/.npmrc"_s);
    if (!file.open(QIODevice::ReadOnly)) return {};
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (!line.startsWith(u"prefix"_s)) continue;
        const qsizetype equals = line.indexOf(u'=');
        if (equals < 0 || line.left(equals).trimmed() != u"prefix"_s) continue;
        QString prefix = line.mid(equals + 1).trimmed();
        if (prefix.size() >= 2 && (prefix.startsWith(u'"') || prefix.startsWith(u'\''))) prefix = prefix.mid(1, prefix.size() - 2);
        prefix.replace(u"${HOME}"_s, home);
        prefix.replace(u"$HOME"_s, home);
        if (prefix.startsWith(u"~/"_s)) prefix = home + prefix.mid(1);
        return QDir::isAbsolutePath(prefix) ? QDir::cleanPath(prefix) : QString();
    }
    return {};
}

} // namespace

QStringList programSearchDirs(const QString &home) {
    QStringList dirs{home + u"/.local/bin"_s, home + u"/.npm-global/bin"_s};
    if (const QString prefix = npmPrefix(home); !prefix.isEmpty()) dirs.append(prefix + u"/bin"_s);

    QStringList nodes = QDir(home + u"/.nvm/versions/node"_s).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(nodes.begin(), nodes.end(), [](const QString &a, const QString &b) {
        return QVersionNumber::fromString(a.mid(a.startsWith(u'v') ? 1 : 0))
               > QVersionNumber::fromString(b.mid(b.startsWith(u'v') ? 1 : 0));
    });
    for (const QString &node : nodes) dirs.append(home + u"/.nvm/versions/node/"_s + node + u"/bin"_s);

    dirs.append({home + u"/.volta/bin"_s, home + u"/.bun/bin"_s, u"/usr/local/bin"_s});
    dirs.removeDuplicates();
    return dirs;
}

QString findProgramIn(const QString &name, const QStringList &dirs) {
    return dirs.isEmpty() ? QString() : QStandardPaths::findExecutable(name, dirs);
}

QString findProgram(const QString &name, const QString &configured) {
    if (!configured.isEmpty()) {
        const QFileInfo file(configured);
        return file.isFile() && file.isExecutable() ? file.absoluteFilePath() : QString();
    }
    if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty()) return onPath;
    return findProgramIn(name, programSearchDirs(QDir::homePath()));
}

QString programSearchHint() {
    return u"looked on PATH, in ~/.local/bin, ~/.npm-global/bin, npm's prefix, nvm's, Volta's and Bun's folders "
           "and /usr/local/bin"_s;
}

QProcessEnvironment environmentFor(const QString &program) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString folder = QFileInfo(program).absolutePath();
    const QString path = environment.value(u"PATH"_s);
    environment.insert(u"PATH"_s, path.isEmpty() ? folder : folder + u':' + path);
    return environment;
}

} // namespace nexa
