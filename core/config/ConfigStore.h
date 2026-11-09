#pragma once

#include "core/Result.h"
#include "core/config/Config.h"

#include <QString>

namespace nexa {

// Loads and saves Config as JSON. The file holds API keys, so the directory is kept 0700 and the
// file 0600, and saves are atomic (QSaveFile): a crash never leaves a half-written config. A save that removes
// providers first keeps the file as it was in previousPath() and logs which ones went, so a list lost by a slip in
// Settings can be put back.
class ConfigStore {
public:
    explicit ConfigStore(QString path = defaultPath());

    static QString defaultPath();               // $XDG_CONFIG_HOME/nexa/config.json
    const QString &path() const { return path_; }
    QString previousPath() const { return path_ + QStringLiteral(".prev"); }    // before the last removal

    // A missing file yields the defaults; an unreadable or malformed file is an error and is left untouched.
    Result<Config> load() const;
    Result<void> save(const Config &config) const;
    // Renames an unusable file to config.json.broken-<timestamp>; returns the new path.
    Result<QString> moveAside() const;

private:
    QString path_;
};

} // namespace nexa
