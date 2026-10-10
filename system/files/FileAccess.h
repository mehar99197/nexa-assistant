#pragma once

#include "core/Result.h"

#include <QString>
#include <QStringList>

namespace nexa {

// File reads and writes for the model, confined to allowed roots (home and /tmp by default).
// Paths are canonicalised first, so `..` and symlinks cannot escape a root.
class FileAccess {
public:
    explicit FileAccess(QStringList roots = defaultRoots());

    static QStringList defaultRoots();

    // "~/x", relative (to home) or absolute → canonical absolute path inside a root.
    Result<QString> resolve(const QString &path) const;
    // Text only; refuses binary files. Longer files are cut at `maxBytes` with a note.
    Result<QString> read(const QString &path, qint64 maxBytes = 64 * 1024) const;
    // Creates missing parent folders; overwrites atomically unless `append`. Returns the bytes written.
    Result<qint64> write(const QString &path, const QString &content, bool append) const;

private:
    QStringList roots_;
};

} // namespace nexa
