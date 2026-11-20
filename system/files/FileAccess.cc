#include "system/files/FileAccess.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

using namespace Qt::StringLiterals;

namespace nexa {

FileAccess::FileAccess(QStringList roots) {
    for (const QString &root : roots) {
        const QString canonical = QFileInfo(root).canonicalFilePath();
        if (!canonical.isEmpty()) roots_.append(canonical);
    }
}

QStringList FileAccess::defaultRoots() {
    return {QDir::homePath(), QDir::tempPath()};
}

Result<QString> FileAccess::resolve(const QString &path) const {
    QString expanded = path.trimmed();
    if (expanded.isEmpty()) return Error{ErrorCode::InvalidArgument, u"No path given"_s};
    if (expanded == u"~" || expanded.startsWith(u"~/")) expanded = QDir::homePath() + expanded.mid(1);
    const QString absolute = QDir::cleanPath(QDir(QDir::homePath()).absoluteFilePath(expanded));

    // Canonicalise the deepest existing ancestor: symlinks on the way may point outside the roots.
    QString existing = absolute;
    QStringList missing;
    while (!QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).absolutePath();
        if (parent.isEmpty() || parent == existing) break;
        missing.prepend(QFileInfo(existing).fileName());
        existing = parent;
    }
    QString canonical = QFileInfo(existing).canonicalFilePath();
    if (!missing.isEmpty()) canonical = QDir::cleanPath(canonical + u'/' + missing.join(u'/'));

#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif
    for (const QString &root : roots_) {
        const QString prefix = root.endsWith(u'/') ? root : root + u'/';
        if (canonical.compare(root, kPathCase) == 0 || canonical.startsWith(prefix, kPathCase)) return canonical;
    }
    return Error{ErrorCode::PermissionDenied,
                 u"%1 is outside the folders Nexa may use (%2)"_s.arg(path, roots_.join(u", "_s))};
}

Result<QString> FileAccess::read(const QString &path, qint64 maxBytes) const {
    const Result<QString> resolved = resolve(path);
    if (!resolved) return resolved.error();
    QFile file(resolved.value());
    if (QFileInfo(file).isDir()) return Error{ErrorCode::InvalidArgument, u"%1 is a folder"_s.arg(resolved.value())};
    if (!file.open(QIODevice::ReadOnly)) {
        return Error{ErrorCode::Io, u"Cannot read %1: %2"_s.arg(resolved.value(), file.errorString())};
    }
    const qint64 size = file.size();
    const QByteArray data = file.read(qMax<qint64>(maxBytes, 1));
    if (data.first(qMin<qsizetype>(data.size(), 8 * 1024)).contains('\0')) {
        return Error{ErrorCode::InvalidArgument, u"%1 is a binary file"_s.arg(resolved.value())};
    }
    QString text = QString::fromUtf8(data);
    if (size > data.size()) text += u"\n[truncated: showing the first %1 of %2 bytes]"_s.arg(data.size()).arg(size);
    return text;
}

Result<qint64> FileAccess::write(const QString &path, const QString &content, bool append) const {
    const Result<QString> resolved = resolve(path);
    if (!resolved) return resolved.error();
    const QString target = resolved.value();
    if (QFileInfo(target).isDir()) return Error{ErrorCode::InvalidArgument, u"%1 is a folder"_s.arg(target)};
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        return Error{ErrorCode::Io, u"Cannot create the folder for %1"_s.arg(target)};
    }
    const QByteArray data = content.toUtf8();

    if (append) {
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            return Error{ErrorCode::Io, u"Cannot write %1: %2"_s.arg(target, file.errorString())};
        }
        return file.write(data);
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        return Error{ErrorCode::Io, u"Cannot write %1: %2"_s.arg(target, file.errorString())};
    }
    const qint64 written = file.write(data);
    if (!file.commit()) return Error{ErrorCode::Io, u"Cannot save %1: %2"_s.arg(target, file.errorString())};
    return written;
}

} // namespace nexa
