#include "core/account/UpdateChecker.h"

#include "LicenseTestUtil.h"
#include "QtStringMakers.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("UpdateChecker: semver ordering and platform key", "[account][update]") {
    CHECK(UpdateChecker::isNewer(u"1.1.0"_s, u"1.0.0"_s));
    CHECK(UpdateChecker::isNewer(u"1.0.1"_s, u"1.0.0"_s));
    CHECK(UpdateChecker::isNewer(u"2.0"_s, u"1.9.9"_s));
    CHECK_FALSE(UpdateChecker::isNewer(u"1.0.0"_s, u"1.0.0"_s));
    CHECK_FALSE(UpdateChecker::isNewer(u"0.9.5"_s, u"1.0.0"_s));
    CHECK_FALSE(UpdateChecker::platformKey().isEmpty());
}

TEST_CASE("UpdateChecker: verifies nexa-update-v1 Ed25519 manifest signatures", "[account][update]") {
    const QString sha = u"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"_s;
    const QString url = u"https://nexadownloadmanager.com/downloads/NexaAssistantSetup-1.2.0.exe"_s;
    const QString manifest = test::signUpdateManifest(u"1.2.0"_s, url, sha, u"Windows release"_s);

    QString err;
    const auto parsed = UpdateChecker::parseSignedFeed(manifest.toUtf8(), &err);
    REQUIRE(parsed.has_value());
    CHECK(err.isEmpty());
    CHECK(parsed->version == u"1.2.0"_s);
    CHECK(parsed->url == url);
    CHECK(parsed->sha256 == sha);

    // Rejects tampered payload
    const QByteArray tampered = manifest.toUtf8().replace("1.2.0", "9.9.9");
    REQUIRE_FALSE(UpdateChecker::parseSignedFeed(tampered, &err).has_value());
    CHECK(err == u"update feed signature is invalid"_s);

    // Rejects plain HTTP download URL
    const QString httpManifest = test::signUpdateManifest(u"1.2.0"_s, u"http://evil.example/payload.exe"_s, sha);
    REQUIRE_FALSE(UpdateChecker::parseSignedFeed(httpManifest.toUtf8(), &err).has_value());
    CHECK(err == u"update feed URL must be HTTPS"_s);

    // Rejects wrong signing key
    const QString badKeyManifest = test::signUpdateManifest(u"1.2.0"_s, url, sha, u"notes"_s, test::otherSeed());
    REQUIRE_FALSE(UpdateChecker::parseSignedFeed(badKeyManifest.toUtf8(), &err).has_value());
}

TEST_CASE("UpdateChecker: check and download verify SHA-256 end-to-end", "[account][update]") {
    const QByteArray payload = QByteArrayLiteral("fake-installer-binary-payload-v1.2.0");
    const QString goodSha = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());

    QTcpServer server;
    REQUIRE(server.listen(QHostAddress::LocalHost, 0));
    const QString dlUrl = u"http://127.0.0.1:%1/installer.deb"_s.arg(server.serverPort());
    const QString feedJson = test::signUpdateManifest(u"1.2.0"_s, dlUrl, goodSha);

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (QTcpSocket *sock = server.nextPendingConnection()) {
            auto buf = std::make_shared<QByteArray>();
            QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, buf, feedJson, payload] {
                buf->append(sock->readAll());
                const qsizetype hdrEnd = buf->indexOf("\r\n\r\n");
                if (hdrEnd < 0) return;
                const QByteArray first = buf->left(hdrEnd).split('\n').first();
                const bool isFeed = first.contains("/feed");
                const QByteArray body = isFeed ? feedJson.toUtf8() : payload;
                const QByteArray ct = isFeed ? "application/json" : "application/octet-stream";
                sock->write("HTTP/1.1 200 OK\r\nContent-Type: " + ct + "\r\nContent-Length: "
                            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                sock->flush();
                sock->disconnectFromHost();
            });
        }
    });

    qputenv("NEXA_UPDATE_URL", u"http://127.0.0.1:%1/feed"_s.arg(server.serverPort()).toUtf8());
    const auto cleanup = qScopeGuard([] { qunsetenv("NEXA_UPDATE_URL"); });

    UpdateChecker checker;
    QSignalSpy availSpy(&checker, &UpdateChecker::updateAvailable);
    checker.check(u"1.0.0"_s);
    REQUIRE(availSpy.wait(3000));
    CHECK(availSpy.last().at(0).toString() == u"1.2.0"_s);
    CHECK(availSpy.last().at(3).toString() == goodSha);

    QSignalSpy readySpy(&checker, &UpdateChecker::downloadReady);
    QSignalSpy failedSpy(&checker, &UpdateChecker::downloadFailed);
    checker.downloadUpdate(dlUrl, goodSha, u"1.2.0"_s);
    REQUIRE(readySpy.wait(3000));
    CHECK(failedSpy.isEmpty());
    const QString downloadedPath = readySpy.last().at(0).toString();
    REQUIRE(QFileInfo::exists(downloadedPath));
    QFile::remove(downloadedPath);

    // Corrupted hash fails cleanly
    checker.downloadUpdate(dlUrl, QString(64, u'0'), u"1.2.0"_s);
    REQUIRE(failedSpy.wait(3000));
    CHECK(failedSpy.last().at(0).toString().contains(u"SHA-256 verification"_s));
}
