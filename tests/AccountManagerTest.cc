#include "core/account/AccountManager.h"
#include "core/account/CredentialStore.h"

#include "LicenseTestUtil.h"
#include "QtStringMakers.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class MockApiServer {
public:
    struct Reply {
        int status{200};
        QByteArray body;
    };

    using Handler = std::function<Reply(const QByteArray &method, const QString &path, const QJsonObject &json,
                                        const QByteArray &auth)>;

    explicit MockApiServer(Handler handler) : handler_(std::move(handler)) {
        REQUIRE(server_.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (QTcpSocket *sock = server_.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
                    buf->append(sock->readAll());
                    const qsizetype hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray head = buf->left(hdrEnd);
                    qsizetype len = 0;
                    QByteArray auth;
                    for (const QByteArray &line : head.split('\n')) {
                        const qsizetype c = line.indexOf(':');
                        if (c <= 0) continue;
                        const QByteArray k = line.left(c).trimmed().toLower();
                        if (k == "content-length") len = line.mid(c + 1).trimmed().toLongLong();
                        else if (k == "authorization") auth = line.mid(c + 1).trimmed();
                    }
                    if (buf->size() < hdrEnd + 4 + len) return;
                    const QList<QByteArray> parts = head.split('\n').first().trimmed().split(' ');
                    const QByteArray method = parts.value(0);
                    const QString path = QString::fromUtf8(parts.value(1));
                    const QJsonObject bodyJson = QJsonDocument::fromJson(buf->mid(hdrEnd + 4, len)).object();
                    requests.append({method, path, bodyJson, auth});
                    const Reply res = handler_(method, path, bodyJson, auth);
                    const QByteArray out = "HTTP/1.1 " + QByteArray::number(res.status) + " OK\r\n"
                        + "Content-Type: application/json\r\n"
                        + "Content-Length: " + QByteArray::number(res.body.size()) + "\r\n"
                        + "Connection: close\r\n\r\n" + res.body;
                    sock->write(out);
                    sock->flush();
                    sock->disconnectFromHost();
                });
            }
        });
    }

    QString licenseValidateUrl() const {
        return u"http://127.0.0.1:%1/api/license/validate"_s.arg(server_.serverPort());
    }

    struct RecordedRequest {
        QByteArray method;
        QString path;
        QJsonObject json;
        QByteArray auth;
    };
    QList<RecordedRequest> requests;

private:
    QTcpServer server_;
    Handler handler_;
};

QByteArray okJson(const QJsonObject &data) {
    return QJsonDocument(QJsonObject{{u"ok"_s, true}, {u"data"_s, data}}).toJson(QJsonDocument::Compact);
}

} // namespace

TEST_CASE("AccountManager: hardware fingerprint v3 filters virtual/randomized MACs", "[account]") {
    CHECK(AccountManager::isVirtualMac(u"00:15:5d:12:34:56"_s));
    CHECK(AccountManager::isVirtualMac(u"52:54:00:ab:cd:ef"_s));
    CHECK(AccountManager::isVirtualMac(u"02:11:22:33:44:55"_s)); // locally-administered bit
    CHECK_FALSE(AccountManager::isVirtualMac(u"d8:bb:c1:12:34:56"_s));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString settingsPath = dir.filePath(u"account.ini"_s);
    const QString fp1 = AccountManager::deviceFingerprint(settingsPath);
    const QString fp2 = AccountManager::deviceFingerprint(settingsPath);
    CHECK(fp1.size() == 64);
    CHECK(fp1 == fp2);
}

TEST_CASE("AccountManager: manual key activation, usage tracking, and deactivation", "[account]") {
    credentialstore::clearForTesting();
    const auto cleanup = qScopeGuard([] {
        credentialstore::clearForTesting();
        qunsetenv("NEXA_LICENSE_API_URL");
        qunsetenv("NEXA_ALLOW_INSECURE_LICENSE_API");
    });

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString settingsPath = dir.filePath(u"account.ini"_s);
    const QString fp = AccountManager::deviceFingerprint(settingsPath);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString signedJwt = test::signAssistantJwt(
        u"pro"_s, fp, now, now + 86400,
        QJsonObject{{u"nexaAi"_s, true}, {u"nexaAiMonthlyRequests"_s, 1000}, {u"ownVoice"_s, true},
                    {u"awayFromHome"_s, true}, {u"seats"_s, 1}});

    MockApiServer api([&](const QByteArray &, const QString &path, const QJsonObject &body, const QByteArray &) {
        if (path == u"/api/license/validate"_s) {
            CHECK(body.value(u"product"_s).toString() == u"assistant"_s);
            return MockApiServer::Reply{200, QJsonDocument(QJsonObject{
                {u"valid"_s, true},
                {u"plan"_s, u"pro"_s},
                {u"trial"_s, false},
                {u"seats"_s, 1},
                {u"activeSeats"_s, 1},
                {u"expires"_s, QDateTime::fromSecsSinceEpoch(now + 86400).toUTC().toString(Qt::ISODate)},
                {u"token"_s, signedJwt},
                {u"features"_s, QJsonObject{{u"nexaAi"_s, true}, {u"nexaAiMonthlyRequests"_s, 1000},
                                           {u"ownVoice"_s, true}, {u"awayFromHome"_s, true}, {u"seats"_s, 1}}},
            }).toJson(QJsonDocument::Compact)};
        }
        if (path == u"/api/assistant/v1/usage"_s) {
            return MockApiServer::Reply{200, QJsonDocument(QJsonObject{
                {u"enabled"_s, true},
                {u"plan"_s, u"pro"_s},
                {u"month"_s, u"2026-11"_s},
                {u"used"_s, 42},
                {u"limit"_s, 1000},
                {u"remaining"_s, 958},
                {u"resetsAt"_s, u"2026-12-01T00:00:00Z"_s},
            }).toJson(QJsonDocument::Compact)};
        }
        if (path == u"/api/license/deactivate"_s) {
            return MockApiServer::Reply{200, okJson({{u"released"_s, true}})};
        }
        return MockApiServer::Reply{404, {}};
    });

    qputenv("NEXA_LICENSE_API_URL", api.licenseValidateUrl().toUtf8());
    qputenv("NEXA_ALLOW_INSECURE_LICENSE_API", "1");

    AccountManager account(settingsPath);
    QSignalSpy activated(&account, &AccountManager::activationFinished);
    QSignalSpy usageSpy(&account, &AccountManager::usageChanged);

    account.activate(u"NDM-AAAA-BBBB-CCCC"_s);
    REQUIRE(activated.wait(3000));
    CHECK(activated.last().at(0).toBool());
    CHECK(account.isPaid());
    CHECK(account.plan() == u"pro"_s);
    CHECK(account.licenseToken() == signedJwt);
    CHECK(account.verifiedFeatures().nexaAi);
    CHECK(account.verifiedFeatures().nexaAiMonthlyRequests == 1000);
    CHECK(account.verifiedFeatures().ownVoice);
    CHECK(account.verifiedFeatures().awayFromHome);

    REQUIRE(QTest::qWaitFor([&] { return account.aiUsage().known; }, 3000));
    CHECK(account.aiUsage().used == 42);
    CHECK(account.aiUsage().limit == 1000);
    CHECK(account.aiUsage().remaining == 958);

    account.noteUsageRemaining(950);
    CHECK(account.aiUsage().remaining == 950);
    CHECK(account.aiUsage().used == 50);

    account.deactivate();
    CHECK_FALSE(account.isPaid());
    CHECK(account.plan() == u"free"_s);
    CHECK_FALSE(account.verifiedFeatures().nexaAi);
    CHECK_FALSE(account.verifiedFeatures().ownVoice);
    CHECK_FALSE(account.verifiedFeatures().awayFromHome);
}

TEST_CASE("AccountManager: OAuth device flow sign-in polls and acquires assistant license", "[account]") {
    credentialstore::clearForTesting();
    const auto cleanup = qScopeGuard([] {
        credentialstore::clearForTesting();
        qunsetenv("NEXA_LICENSE_API_URL");
        qunsetenv("NEXA_ALLOW_INSECURE_LICENSE_API");
    });

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString settingsPath = dir.filePath(u"account.ini"_s);
    const QString fp = AccountManager::deviceFingerprint(settingsPath);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString signedJwt = test::signAssistantJwt(
        u"team"_s, fp, now, now + 86400,
        QJsonObject{{u"nexaAi"_s, true}, {u"nexaAiMonthlyRequests"_s, 1000},
                    {u"ownVoice"_s, true}, {u"awayFromHome"_s, true}, {u"seats"_s, 5}},
        u"assistant"_s, test::devSeed(), 77);

    int pollCount = 0;
    MockApiServer api([&](const QByteArray &, const QString &path, const QJsonObject &body, const QByteArray &) {
        if (path == u"/api/device/code"_s) {
            CHECK(body.value(u"product"_s).toString() == u"assistant"_s);
            return MockApiServer::Reply{200, okJson({
                {u"deviceCode"_s, u"dev-123"_s},
                {u"userCode"_s, u"ABCD-EFGH"_s},
                {u"verificationUrl"_s, u"https://nexadownloadmanager.com/activate"_s},
                {u"expiresIn"_s, 300},
                {u"interval"_s, 1},
            })};
        }
        if (path == u"/api/device/token"_s) {
            if (++pollCount < 1) return MockApiServer::Reply{200, okJson({{u"status"_s, u"pending"_s}})};
            return MockApiServer::Reply{200, okJson({
                {u"status"_s, u"approved"_s},
                {u"deviceToken"_s, u"acct-tok-999"_s},
                {u"account"_s, QJsonObject{{u"email"_s, u"alice@example.com"_s}, {u"id"_s, 77}}},
            })};
        }
        if (path == u"/api/license/validate"_s) {
            CHECK(body.value(u"device_token"_s).toString() == u"acct-tok-999"_s);
            CHECK(body.value(u"product"_s).toString() == u"assistant"_s);
            return MockApiServer::Reply{200, QJsonDocument(QJsonObject{
                {u"valid"_s, true},
                {u"plan"_s, u"team"_s},
                {u"trial"_s, false},
                {u"seats"_s, 5},
                {u"activeSeats"_s, 1},
                {u"account"_s, QJsonObject{{u"email"_s, u"alice@example.com"_s}, {u"id"_s, 77}}},
                {u"token"_s, signedJwt},
                {u"features"_s, QJsonObject{{u"nexaAi"_s, true}, {u"nexaAiMonthlyRequests"_s, 1000},
                                           {u"ownVoice"_s, true}, {u"awayFromHome"_s, true}, {u"seats"_s, 5}}},
            }).toJson(QJsonDocument::Compact)};
        }
        if (path == u"/api/assistant/v1/usage"_s) {
            return MockApiServer::Reply{200, QJsonDocument(QJsonObject{
                {u"enabled"_s, true}, {u"plan"_s, u"team"_s}, {u"used"_s, 0}, {u"limit"_s, 1000}, {u"remaining"_s, 1000},
            }).toJson(QJsonDocument::Compact)};
        }
        if (path == u"/api/device/signout"_s) {
            return MockApiServer::Reply{200, okJson({{u"signed_out"_s, true}})};
        }
        return MockApiServer::Reply{404, {}};
    });

    qputenv("NEXA_LICENSE_API_URL", api.licenseValidateUrl().toUtf8());
    qputenv("NEXA_ALLOW_INSECURE_LICENSE_API", "1");

    AccountManager account(settingsPath);
    QSignalSpy codeSpy(&account, &AccountManager::signInCodeReady);
    QSignalSpy signInSpy(&account, &AccountManager::signInFinished);

    account.beginSignIn();
    REQUIRE(codeSpy.wait(3000));
    CHECK(codeSpy.last().at(0).toString() == u"ABCD-EFGH"_s);

    // Wait for automatic 5 s poll tick or call within 7 s window
    REQUIRE(signInSpy.wait(7000));
    CHECK(signInSpy.last().at(0).toBool());
    CHECK(account.isSignedIn());
    CHECK(account.accountEmail() == u"alice@example.com"_s);

    REQUIRE(QTest::qWaitFor([&] { return account.isPaid(); }, 3000));
    CHECK(account.plan() == u"team"_s);
    CHECK(account.seats() == 5);
    CHECK(account.verifiedFeatures().nexaAi);

    account.signOut();
    CHECK_FALSE(account.isSignedIn());
    CHECK_FALSE(account.isPaid());
}
