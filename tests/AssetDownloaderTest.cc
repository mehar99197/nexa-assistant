// system/setup/AssetDownloader: the models fetched into the data folder — resumed, verified, unpacked.

#include "system/setup/AssetDownloader.h"

#include "FakeHttpServer.h"
#include "QtStringMakers.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 10'000;

QString sha256(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray body(int size) {
    QByteArray out;
    out.reserve(size);
    for (int i = 0; i < size; ++i) out.append(char('a' + i % 23));
    return out;
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

struct Rig {
    Rig() : downloader(data.path()) {
        downloader.setCacheDir(cache.path());
        QObject::connect(&downloader, &AssetDownloader::finished, [this] { done = true; });
        QObject::connect(&downloader, &AssetDownloader::failed, [this](const QString &id, const Error &error) {
            failedId = id;
            failure = error;
        });
        QObject::connect(&downloader, &AssetDownloader::assetInstalled, [this](const QString &id) { installed.append(id); });
        QObject::connect(&downloader, &AssetDownloader::progress, [this](const QString &, qint64 received, qint64) { lastProgress = received; });
    }

    engineassets::Asset plain(const QByteArray &bytes, const QString &name = u"file.bin"_s) {
        engineassets::Asset asset;
        asset.id = u"test-"_s + name;
        asset.group = u"listening"_s;
        asset.url = server.baseUrl().toString() + u'/' + name;
        asset.bytes = bytes.size();
        asset.sha256 = sha256(bytes);
        asset.installDir = u"models"_s;
        asset.files = {name};
        return asset;
    }

    bool wait() { return QTest::qWaitFor([this] { return done || failure.has_value(); }, kWaitMs); }

    test::FakeHttpServer server;
    QTemporaryDir data;
    QTemporaryDir cache;
    AssetDownloader downloader;
    bool done{false};
    QString failedId;
    std::optional<Error> failure;
    QStringList installed;
    qint64 lastProgress{0};
};

} // namespace

TEST_CASE("A plain file downloads, verifies and lands in its folder") {
    Rig rig;
    const QByteArray bytes = body(30'000);
    rig.server.respond({.status = 200, .contentType = "application/octet-stream", .body = bytes, .honourRange = true});
    rig.downloader.start({rig.plain(bytes)});
    REQUIRE(rig.wait());
    CHECK_FALSE(rig.failure.has_value());
    CHECK(rig.installed == QStringList{u"test-file.bin"_s});
    CHECK(readAll(rig.data.path() + u"/models/file.bin"_s) == bytes);
    CHECK(rig.lastProgress == bytes.size());
    CHECK_FALSE(QFile::exists(rig.cache.path() + u"/file.bin.part"_s));
    CHECK_FALSE(rig.downloader.isRunning());
}

TEST_CASE("A part file resumes from where it stopped") {
    Rig rig;
    const QByteArray bytes = body(20'000);
    QFile part(rig.cache.path() + u"/file.bin.part"_s);
    REQUIRE(part.open(QIODevice::WriteOnly));
    part.write(bytes.left(5'000));
    part.close();
    rig.server.respond({.status = 200, .contentType = "application/octet-stream", .body = bytes, .honourRange = true});
    rig.downloader.start({rig.plain(bytes)});
    REQUIRE(rig.wait());
    CHECK_FALSE(rig.failure.has_value());
    REQUIRE(!rig.server.requestHeads().isEmpty());
    CHECK(rig.server.requestHeads().last().contains("Range: bytes=5000-"));
    CHECK(readAll(rig.data.path() + u"/models/file.bin"_s) == bytes);
}

TEST_CASE("A server that ignores the range starts the file over, and the result is still right") {
    Rig rig;
    const QByteArray bytes = body(12'000);
    QFile part(rig.cache.path() + u"/file.bin.part"_s);
    REQUIRE(part.open(QIODevice::WriteOnly));
    part.write("garbage that would spoil the file");
    part.close();
    rig.server.respond({.status = 200, .contentType = "application/octet-stream", .body = bytes, .honourRange = false});
    rig.downloader.start({rig.plain(bytes)});
    REQUIRE(rig.wait());
    CHECK_FALSE(rig.failure.has_value());
    CHECK(readAll(rig.data.path() + u"/models/file.bin"_s) == bytes);
}

TEST_CASE("A wrong hash removes the part and fails the asset in words") {
    Rig rig;
    const QByteArray bytes = body(8'000);
    rig.server.respond({.status = 200, .contentType = "application/octet-stream", .body = bytes, .honourRange = true});
    engineassets::Asset asset = rig.plain(bytes);
    asset.sha256 = QString(64, u'0');
    rig.downloader.start({asset});
    REQUIRE(rig.wait());
    REQUIRE(rig.failure.has_value());
    CHECK(rig.failure->code == ErrorCode::Parse);
    CHECK(rig.failure->message.contains(u"did not match its checksum"_s));
    CHECK(rig.failedId == u"test-file.bin"_s);
    CHECK_FALSE(QFile::exists(rig.cache.path() + u"/file.bin.part"_s));
    CHECK_FALSE(QFile::exists(rig.data.path() + u"/models/file.bin"_s));
    CHECK(rig.installed.isEmpty());
}

TEST_CASE("An archive unpacks and only the listed members are installed") {
    Rig rig;
    QTemporaryDir source;
    QDir().mkpath(source.path() + u"/pkg-1"_s);
    for (const char *name : {"a.txt", "b.txt", "skip.txt"}) {
        QFile file(source.path() + u"/pkg-1/"_s + QLatin1StringView(name));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(name);
    }
    QString tarProgram = u"tar"_s;
#ifdef Q_OS_WIN
    const QString winTar =
        QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", u"C:/Windows"_s)) + u"/System32/tar.exe"_s;
    if (QFile::exists(winTar)) tarProgram = winTar;
#endif
    QProcess tar;
    tar.start(tarProgram, {u"-cjf"_s, source.path() + u"/pkg.tar.bz2"_s, u"-C"_s, source.path(), u"pkg-1"_s});
    REQUIRE(tar.waitForFinished(kWaitMs));
    REQUIRE(tar.exitCode() == 0);
    const QByteArray bytes = readAll(source.path() + u"/pkg.tar.bz2"_s);
    REQUIRE(!bytes.isEmpty());
    rig.server.respond({.status = 200, .contentType = "application/x-bzip2", .body = bytes, .honourRange = true});
    engineassets::Asset asset = rig.plain(bytes, u"pkg.tar.bz2"_s);
    asset.installDir = u"sherpa/kws"_s;
    asset.files = {u"a.txt"_s, u"b.txt"_s};
    asset.archiveTop = u"pkg-1"_s;
    rig.downloader.start({asset});
    REQUIRE(rig.wait());
    CHECK_FALSE(rig.failure.has_value());
    CHECK(readAll(rig.data.path() + u"/sherpa/kws/a.txt"_s) == "a.txt");
    CHECK(readAll(rig.data.path() + u"/sherpa/kws/b.txt"_s) == "b.txt");
    CHECK_FALSE(QFile::exists(rig.data.path() + u"/sherpa/kws/skip.txt"_s));
    CHECK_FALSE(QFile::exists(rig.cache.path() + u"/pkg.tar.bz2.part"_s));
    CHECK(QDir(rig.cache.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());   // the scratch folder is gone
}

TEST_CASE("Without tar an archive asset fails in words") {
    Rig rig;
    const QByteArray bytes = body(2'000);
    rig.server.respond({.status = 200, .contentType = "application/x-bzip2", .body = bytes, .honourRange = true});
    engineassets::Asset asset = rig.plain(bytes, u"pkg.tar.bz2"_s);
    asset.archiveTop = u"pkg-1"_s;
    rig.downloader.setTarProgram(u"/nonexistent/tar"_s);
    rig.downloader.start({asset});
    REQUIRE(rig.wait());
    REQUIRE(rig.failure.has_value());
    CHECK(rig.failure->code == ErrorCode::NotSupported);
    CHECK(rig.failure->message.contains(u"tar is needed"_s));
}

TEST_CASE("cancel() stops and leaves the part for next time; a stalled download times out in words") {
    Rig rig;
    const QByteArray bytes = body(50'000);
    rig.server.respond({.status = 200, .contentType = "application/octet-stream", .holdOpen = true, .body = bytes, .honourRange = true,
                        .sendOnly = 9'000});
    rig.downloader.setIdleTimeoutMs(60'000);
    rig.downloader.start({rig.plain(bytes)});
    REQUIRE(QTest::qWaitFor([&rig] { return rig.lastProgress > 0; }, kWaitMs));
    rig.downloader.cancel();
    CHECK_FALSE(rig.downloader.isRunning());
    CHECK(QFile(rig.cache.path() + u"/file.bin.part"_s).size() > 0);
    CHECK_FALSE(rig.done);
    CHECK_FALSE(rig.failure.has_value());

    Rig stalled;
    stalled.server.respond({.status = 200, .contentType = "application/octet-stream", .holdOpen = true, .body = bytes, .honourRange = true,
                            .sendOnly = 9'000});
    stalled.downloader.setIdleTimeoutMs(400);
    stalled.downloader.start({stalled.plain(bytes)});
    REQUIRE(stalled.wait());
    REQUIRE(stalled.failure.has_value());
    CHECK(stalled.failure->code == ErrorCode::Timeout);
    CHECK(stalled.failure->message.contains(u"stopped arriving"_s));
}

TEST_CASE("Several assets install in turn, and an empty list finishes at once") {
    Rig rig;
    const QByteArray bytes = body(1'000);
    rig.server.respond({.status = 200, .contentType = "application/octet-stream", .body = bytes, .honourRange = true});
    rig.downloader.start({rig.plain(bytes, u"one.bin"_s), rig.plain(bytes, u"two.bin"_s)});
    REQUIRE(rig.wait());
    CHECK(rig.installed == QStringList{u"test-one.bin"_s, u"test-two.bin"_s});
    Rig empty;
    empty.downloader.start({});
    CHECK(empty.done);
}
