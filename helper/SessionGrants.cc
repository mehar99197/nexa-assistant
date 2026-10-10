#include "helper/SessionGrants.h"

#include "core/Logging.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kVersion = 1;

} // namespace

bool SessionGrants::Grant::covers(const LoginSession &session) const {
    return session.isValid() && session.uid == uid && session.id == sessionId
           && session.startedMonotonicUsec == startedMonotonicUsec;
}

SessionGrants::SessionGrants() : SessionGrants(defaultPath()) {}

SessionGrants::SessionGrants(QString path) : path_(std::move(path)) { load(); }

QString SessionGrants::defaultPath() { return u"/run/nexa-helper/grants.json"_s; }

bool SessionGrants::has(const LoginSession &session) const {
    return std::any_of(grants_.cbegin(), grants_.cend(),
                       [&session](const Grant &grant) { return grant.covers(session); });
}

void SessionGrants::add(const LoginSession &session) {
    if (!session.isValid() || has(session)) return;
    // A record for the same session id but a different start time is a previous login, and is of
    // no use to anyone: this file only ever holds the login it was written for.
    grants_.removeIf([&session](const Grant &grant) {
        return grant.uid == session.uid && grant.sessionId == session.id;
    });
    grants_.append(Grant{session.uid, session.id, session.startedMonotonicUsec,
                         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)});
    save();
}

bool SessionGrants::forget(const LoginSession &session) {
    const qsizetype dropped = grants_.removeIf([&session](const Grant &grant) {
        return grant.uid == session.uid && grant.sessionId == session.id;
    });
    if (dropped > 0) save();
    return dropped > 0;
}

QByteArray SessionGrants::encode(const QList<Grant> &grants) {
    QJsonArray array;
    for (const Grant &grant : grants) {
        array.append(QJsonObject{
            {u"uid"_s, qint64(grant.uid)},
            {u"session"_s, grant.sessionId},
            // A string, so the number survives JSON exactly whatever its size.
            {u"started"_s, QString::number(grant.startedMonotonicUsec)},
            {u"at"_s, grant.grantedAt},
        });
    }
    return QJsonDocument(QJsonObject{{u"version"_s, kVersion}, {u"grants"_s, array}})
        .toJson(QJsonDocument::Indented);
}

QList<SessionGrants::Grant> SessionGrants::decode(const QByteArray &json) {
    QList<Grant> grants;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return grants;
    const QJsonObject root = document.object();
    if (root.value(u"version"_s).toInt() != kVersion) return grants;

    const QJsonArray array = root.value(u"grants"_s).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const QJsonValue uid = object.value(u"uid"_s);
        const QString session = object.value(u"session"_s).toString();
        bool numeric = false;
        const qulonglong started = object.value(u"started"_s).toString().toULongLong(&numeric);
        // A record that cannot be read in full is dropped, not guessed at: half an authorisation
        // must never become a whole one.
        if (!uid.isDouble() || session.isEmpty() || !numeric) continue;
        grants.append(Grant{uint(uid.toInteger()), session, started, object.value(u"at"_s).toString()});
    }
    return grants;
}

void SessionGrants::load() {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return;        // nothing authorised yet
    grants_ = decode(file.readAll());
}

void SessionGrants::save() const {
    const QByteArray directory = QFile::encodeName(QFileInfo(path_).absolutePath());
    // 0700 from the moment it exists, rather than created loose and tightened afterwards.
    if (::mkdir(directory.constData(), 0700) != 0 && errno != EEXIST) {
        qCWarning(lcApp).noquote() << "cannot create" << directory << ":" << ::strerror(errno);
        return;
    }
    if (::chmod(directory.constData(), 0700) != 0) {
        qCWarning(lcApp).noquote() << "cannot restrict" << directory << ":" << ::strerror(errno);
        return;
    }

    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcApp).noquote() << "cannot write" << path_ << ":" << file.errorString();
        return;
    }
    file.write(encode(grants_));
    if (!file.commit()) {
        qCWarning(lcApp).noquote() << "cannot write" << path_ << ":" << file.errorString();
        return;
    }
    QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

} // namespace nexa
