#include "core/config/ConfigStore.h"

#include "core/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr QFileDevice::Permissions kPrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr QFileDevice::Permissions kPrivateDir =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr QFileDevice::Permissions kGroupOrOther = QFileDevice::ReadGroup | QFileDevice::WriteGroup
    | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;

// The ids of the providers in `before` that `after` no longer has.
QStringList removedProviders(const QJsonObject &before, const Config &after) {
    QStringList removed;
    for (const QJsonValue &value : before.value(u"providers"_s).toArray()) {
        const QString id = value.toObject().value(u"id"_s).toString();
        if (!id.isEmpty() && !after.provider(id)) removed.append(id);
    }
    return removed;
}

} // namespace

ConfigStore::ConfigStore(QString path) : path_(std::move(path)) {}

QString ConfigStore::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/nexa/config.json"_s;
}

Result<Config> ConfigStore::load() const {
    QFile file(path_);
    if (!file.exists()) return Config{};
    if (!file.open(QIODevice::ReadOnly)) {
        return Error{ErrorCode::Io, u"Cannot read %1: %2"_s.arg(path_, file.errorString())};
    }
    if (file.permissions().testAnyFlags(kGroupOrOther)) {
        qCWarning(lcConfig) << "Restricting" << path_ << "to 0600 (it contains API keys)";
        file.setPermissions(kPrivateFile);
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return Error{ErrorCode::Parse,
                     u"%1 is not valid JSON: %2 (offset %3)"_s.arg(path_, parseError.errorString())
                         .arg(parseError.offset)};
    }
    if (!doc.isObject()) return Error{ErrorCode::Parse, u"%1 must contain a JSON object"_s.arg(path_)};
    return Config::fromJson(doc.object());
}

Result<void> ConfigStore::save(const Config &config) const {
    const QString dirPath = QFileInfo(path_).absolutePath();
    if (!QDir().mkpath(dirPath)) return Error{ErrorCode::Io, u"Cannot create %1"_s.arg(dirPath)};
    QFile::setPermissions(dirPath, kPrivateDir);

    if (QFile current(path_); current.open(QIODevice::ReadOnly)) {
        const QByteArray before = current.readAll();
        const QStringList removed = removedProviders(QJsonDocument::fromJson(before).object(), config);
        if (!removed.isEmpty()) {
            QSaveFile previous(previousPath());
            if (previous.open(QIODevice::WriteOnly)) {
                previous.write(before);
                if (previous.commit()) QFile::setPermissions(previousPath(), kPrivateFile);
            }
            qCWarning(lcConfig).noquote() << "providers removed:" << removed.join(u", "_s) << "— the settings from before"
                                          << "are kept in" << previousPath();
        }
    }

    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        return Error{ErrorCode::Io, u"Cannot write %1: %2"_s.arg(path_, file.errorString())};
    }
    file.write(QJsonDocument(config.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        return Error{ErrorCode::Io, u"Cannot save %1: %2"_s.arg(path_, file.errorString())};
    }
    // New files are created with the umask default; the 0700 directory already hides them meanwhile.
    QFile::setPermissions(path_, kPrivateFile);
    return {};
}

Result<QString> ConfigStore::moveAside() const {
    const QString target =
        path_ + u".broken-"_s + QDateTime::currentDateTime().toString(u"yyyyMMdd-HHmmss"_s);
    if (!QFile::rename(path_, target)) return Error{ErrorCode::Io, u"Cannot rename %1"_s.arg(path_)};
    return target;
}

} // namespace nexa
