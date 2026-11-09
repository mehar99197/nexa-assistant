#include "core/audit/AuditLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kMaxString = 2'000;

QString clip(const QString &text) {
    if (text.size() <= kMaxString) return text;
    return text.left(kMaxString) + u"… [%1 more chars]"_s.arg(text.size() - kMaxString);
}

QJsonValue clipValue(const QJsonValue &value) {
    if (value.isString()) return clip(value.toString());
    if (value.isArray()) {
        QJsonArray clipped;
        for (const QJsonValue &item : value.toArray()) clipped.append(clipValue(item));
        return clipped;
    }
    if (value.isObject()) {
        QJsonObject clipped;
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) clipped.insert(it.key(), clipValue(it.value()));
        return clipped;
    }
    return value;
}

} // namespace

AuditLog::AuditLog(QString path) : path_(std::move(path)) {}

QString AuditLog::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa/audit.jsonl"_s;
}

QJsonObject AuditLog::toJson(const Entry &entry) {
    QJsonObject json{
        {u"time"_s, entry.time.toString(Qt::ISODateWithMs)},
        {u"source"_s, entry.source},
        {u"tool"_s, entry.tool},
        {u"args"_s, clipValue(entry.arguments)},
        {u"ok"_s, entry.ok},
        {u"result"_s, clip(entry.result)},
        {u"ms"_s, entry.elapsedMs},
    };
    if (entry.destructive) json.insert(u"destructive"_s, true);
    if (entry.confirmed) json.insert(u"confirmed"_s, true);
    if (entry.task != 0) json.insert(u"task"_s, qint64(entry.task));
    return json;
}

Result<void> AuditLog::append(const Entry &entry) const {
    const QString dir = QFileInfo(path_).absolutePath();
    if (!QDir().mkpath(dir)) return Error{ErrorCode::Io, u"Cannot create %1"_s.arg(dir)};

    QFile file(path_);
    const bool created = !file.exists();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return Error{ErrorCode::Io, u"Cannot write %1: %2"_s.arg(path_, file.errorString())};
    }
    if (created) file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(toJson(entry)).toJson(QJsonDocument::Compact) + '\n');
    return {};
}

} // namespace nexa
