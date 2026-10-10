// remote/PairingManager: who may use the web remote. These are the properties that keep a
// stranger on the same Wi-Fi out.

#include "remote/PairingManager.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

struct Fixture {
    QTemporaryDir directory;
    QDateTime time = QDateTime(QDate(2026, 10, 10), QTime(12, 0), QTimeZone::utc());
    QString path = directory.filePath(u"remote-devices.json"_s);

    PairingManager make() {
        return PairingManager(path, [this] { return time; });
    }
};

} // namespace

TEST_CASE("A pairing code is six digits and lasts five minutes") {
    Fixture f;
    PairingManager pairing = f.make();
    CHECK(pairing.code().isEmpty());

    const QString code = pairing.startPairing();
    CHECK(code.size() == 6);
    CHECK(code.toInt() >= 0);
    CHECK(pairing.code() == code);

    f.time = f.time.addSecs(PairingManager::kCodeLifetimeSeconds - 1);
    CHECK(pairing.code() == code);
    f.time = f.time.addSecs(1);
    CHECK(pairing.code().isEmpty());
    CHECK_FALSE(pairing.pair(code, u"Phone"_s));        // expired
}

TEST_CASE("The right code buys a token once, and the token lets the phone in") {
    Fixture f;
    PairingManager pairing = f.make();
    const QString code = pairing.startPairing();

    const auto paired = pairing.pair(code, u"Chrome on Android"_s);
    REQUIRE(paired);
    CHECK(paired.value().token.size() >= 43);           // 32 random bytes
    CHECK(paired.value().device.name == u"Chrome on Android"_s);
    CHECK(pairing.authenticate(paired.value().token).has_value());
    CHECK_FALSE(pairing.authenticate(paired.value().token + u'x').has_value());
    CHECK_FALSE(pairing.authenticate(QString()).has_value());

    // One code, one phone.
    CHECK(pairing.code().isEmpty());
    CHECK_FALSE(pairing.pair(code, u"Second phone"_s));
}

TEST_CASE("Guessing withdraws the code before it can be found") {
    Fixture f;
    PairingManager pairing = f.make();
    const QString code = pairing.startPairing();
    const QString wrong = code == u"000000" ? u"111111"_s : u"000000"_s;

    for (int i = 0; i < PairingManager::kMaxCodeAttempts - 1; ++i) CHECK_FALSE(pairing.pair(wrong, u"x"_s));
    CHECK(pairing.code() == code);                      // still there after nine
    CHECK_FALSE(pairing.pair(wrong, u"x"_s));
    CHECK(pairing.code().isEmpty());                    // gone after ten
    CHECK_FALSE(pairing.pair(code, u"x"_s));            // even the right one is too late now

    // Near misses and junk are just wrong.
    pairing.startPairing();
    CHECK_FALSE(pairing.pair(QString(), u"x"_s));
    CHECK_FALSE(pairing.pair(pairing.code().left(5), u"x"_s));
    CHECK_FALSE(pairing.pair(pairing.code() + u'0', u"x"_s));
}

TEST_CASE("The token is never written down, only its hash") {
    Fixture f;
    QString token;
    {
        PairingManager pairing = f.make();
        token = pairing.pair(pairing.startPairing(), u"Phone"_s).value().token;
    }
    QFile file(f.path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray stored = file.readAll();
    CHECK_FALSE(stored.contains(token.toUtf8()));
    CHECK(stored.contains(PairingManager::hashToken(token).toUtf8()));
    CHECK_FALSE(file.permissions().testAnyFlags(QFileDevice::ReadGroup | QFileDevice::ReadOther));

    // A restart remembers the phone.
    PairingManager restarted = f.make();
    CHECK(restarted.authenticate(token).has_value());
}

TEST_CASE("Revoking shuts the phone out and says so") {
    Fixture f;
    PairingManager pairing = f.make();
    const auto first = pairing.pair(pairing.startPairing(), u"Phone"_s).value();
    const auto second = pairing.pair(pairing.startPairing(), u"Tablet"_s).value();
    CHECK(first.token != second.token);
    CHECK(pairing.devices().size() == 2);

    QSignalSpy revoked(&pairing, &PairingManager::revoked);
    CHECK(pairing.revoke(first.device.id));
    CHECK(revoked.size() == 1);
    CHECK(revoked.first().first().toString() == first.device.id);
    CHECK_FALSE(pairing.authenticate(first.token).has_value());
    CHECK(pairing.authenticate(second.token).has_value());
    CHECK_FALSE(pairing.revoke(first.device.id));      // already gone

    CHECK_FALSE(f.make().authenticate(first.token).has_value());
}

TEST_CASE("A device name is something printable and short") {
    Fixture f;
    PairingManager pairing = f.make();
    const auto odd = pairing.pair(pairing.startPairing(), u"  Evil\nName\twith\x01 control  "_s).value();
    CHECK(odd.device.name == u"EvilNamewith control"_s);
    const auto blank = pairing.pair(pairing.startPairing(), u"   "_s).value();
    CHECK(blank.device.name == u"Phone"_s);
    const auto longName = pairing.pair(pairing.startPairing(), QString(500, u'a')).value();
    CHECK(longName.device.name.size() == 60);
}
