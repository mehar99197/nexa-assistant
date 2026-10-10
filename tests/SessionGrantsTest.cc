// helper/SessionGrants: which logins have already given the helper a password, so it is asked for
// once per login and not once per Nexa process. The bug this file guards against is the one users
// actually hit: being asked again, and again, and again.

#include "helper/SessionGrants.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

LoginSession login(uint uid = 1000, const QString &id = u"2"_s, qulonglong started = 22'661'539) {
    LoginSession session;
    session.uid = uid;
    session.id = id;
    session.startedMonotonicUsec = started;
    session.active = true;
    session.remote = false;
    return session;
}

} // namespace

TEST_CASE("A login nobody has authorised is not remembered") {
    QTemporaryDir directory;
    const SessionGrants grants(directory.filePath(u"grants.json"_s));
    CHECK_FALSE(grants.has(login()));
}

TEST_CASE("The answer outlives the helper, so an idle exit does not bring the prompt back") {
    QTemporaryDir directory;
    const QString path = directory.filePath(u"grants.json"_s);
    {
        SessionGrants grants(path);
        grants.add(login());
        CHECK(grants.has(login()));
    }
    // A fresh helper, started by D-Bus on the next action, reads the same record.
    const SessionGrants restarted(path);
    CHECK(restarted.has(login()));
}

TEST_CASE("The next login is asked again") {
    QTemporaryDir directory;
    SessionGrants grants(directory.filePath(u"grants.json"_s));
    grants.add(login());

    // logind reuses session ids, so the id alone must never be enough: the login's start time is
    // what tells one login from the next.
    CHECK_FALSE(grants.has(login(1000, u"2"_s, 99'000'000)));
    // Nor does one user's answer cover another's.
    CHECK_FALSE(grants.has(login(1001)));
    // Nor a different session of the same user.
    CHECK_FALSE(grants.has(login(1000, u"7"_s)));
}

TEST_CASE("Forgetting means the next action asks again") {
    QTemporaryDir directory;
    const QString path = directory.filePath(u"grants.json"_s);
    SessionGrants grants(path);
    grants.add(login());

    CHECK(grants.forget(login()));
    CHECK_FALSE(grants.has(login()));
    CHECK_FALSE(grants.forget(login()));        // nothing left to give up
    CHECK_FALSE(SessionGrants(path).has(login()));
}

TEST_CASE("Only one record per login, however many actions are taken") {
    QTemporaryDir directory;
    SessionGrants grants(directory.filePath(u"grants.json"_s));
    for (int i = 0; i < 5; ++i) grants.add(login());

    QFile file(directory.filePath(u"grants.json"_s));
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(SessionGrants::decode(file.readAll()).size() == 1);
}

TEST_CASE("The record says only that the user said yes") {
    QTemporaryDir directory;
    SessionGrants grants(directory.filePath(u"grants.json"_s));
    grants.add(login());

    QFile file(directory.filePath(u"grants.json"_s));
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray written = file.readAll();
    const QJsonObject record =
        QJsonDocument::fromJson(written).object().value(u"grants"_s).toArray().at(0).toObject();
    // A password is typed into the desktop's own prompt and is never seen by Nexa, so there is
    // nothing of the sort to write down. Keys are listed in full here on purpose: a new one has to
    // be a deliberate decision.
    CHECK(record.keys() == QStringList{u"at"_s, u"session"_s, u"started"_s, u"uid"_s});
    CHECK(record.value(u"uid"_s).toInt() == 1000);
    CHECK(record.value(u"session"_s).toString() == u"2"_s);

    // And nobody but root can read even that.
    CHECK(file.permissions().testFlags(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    CHECK_FALSE(file.permissions().testAnyFlags(QFileDevice::ReadGroup | QFileDevice::ReadOther
                                                | QFileDevice::WriteGroup | QFileDevice::WriteOther));
}

TEST_CASE("A record that cannot be read in full is dropped, not guessed at") {
    CHECK(SessionGrants::decode({}).isEmpty());
    CHECK(SessionGrants::decode("not json at all").isEmpty());
    CHECK(SessionGrants::decode(R"({"version":1,"grants":"nonsense"})").isEmpty());
    // The wrong version is a format we do not know, so none of it is trusted.
    CHECK(SessionGrants::decode(R"({"version":99,"grants":[{"uid":1000,"session":"2","started":"1"}]})").isEmpty());
    // Missing pieces: no user, no session, no start time.
    CHECK(SessionGrants::decode(R"({"version":1,"grants":[{"session":"2","started":"1"}]})").isEmpty());
    CHECK(SessionGrants::decode(R"({"version":1,"grants":[{"uid":1000,"started":"1"}]})").isEmpty());
    CHECK(SessionGrants::decode(R"({"version":1,"grants":[{"uid":1000,"session":"2"}]})").isEmpty());
    CHECK(SessionGrants::decode(R"({"version":1,"grants":[{"uid":1000,"session":"2","started":"soon"}]})")
              .isEmpty());
    // And the good one in the same file still comes through.
    CHECK(SessionGrants::decode(R"({"version":1,"grants":[{"uid":1000,"session":"2"},
                                                          {"uid":1000,"session":"3","started":"7"}]})")
              .size()
          == 1);
}

TEST_CASE("A record only covers the login it was written for") {
    const SessionGrants::Grant grant{1000, u"2"_s, 22'661'539, u"2026-10-10T18:40:12Z"_s};
    CHECK(grant.covers(login()));
    CHECK_FALSE(grant.covers({}));                              // no login at all
    CHECK_FALSE(grant.covers(login(1000, u"2"_s, 22'661'540))); // one microsecond later is another login
}

TEST_CASE("A start time too large for a double survives the file") {
    QTemporaryDir directory;
    const QString path = directory.filePath(u"grants.json"_s);
    const LoginSession ancient = login(1000, u"2"_s, 9'007'199'254'740'993ULL);
    {
        SessionGrants grants(path);
        grants.add(ancient);
    }
    CHECK(SessionGrants(path).has(ancient));
}
