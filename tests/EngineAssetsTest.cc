// system/setup/EngineAssets: the pinned manifest of the speech models — the same pins as the download scripts.

#include "system/setup/EngineAssets.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::engineassets;
using namespace Qt::StringLiterals;

namespace {

QString scriptText(const char *name) {
    QFile file(QStringLiteral(NEXA_TESTS_DIR "/../scripts/") + QLatin1StringView(name));
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
}

void touch(const QString &path) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("x");
}

} // namespace

TEST_CASE("The manifest pins what the download scripts pin") {
    QSet<QString> manifest;
    for (const Asset &asset : all()) manifest.insert(asset.sha256);
    for (const Asset &asset : engines()) manifest.insert(asset.sha256);
    static const QRegularExpression pin(u"^[A-Z_]+SHA256=\"([0-9a-f]{64})\""_s, QRegularExpression::MultilineOption);
    int pins = 0;
    for (const char *script : {"download-sherpa.sh", "download-piper.sh"}) {
        for (const QRegularExpressionMatch &match : pin.globalMatch(scriptText(script))) {
            INFO(script << " pins " << match.captured(1).toStdString());
            CHECK(manifest.contains(match.captured(1)));
            ++pins;
        }
    }
    CHECK(pins == 8);        // sherpa: lib, kws, silero, gtcrn, speaker, zipvoice, vocoder; piper: its archive — nothing new unpinned
}

TEST_CASE("Every asset has a url, a hash, a size, an install folder and at least one file") {
    QSet<QString> ids;
    for (const Asset &asset : all()) {
        INFO(asset.id.toStdString());
        CHECK(asset.url.startsWith(u"https://"_s));
        CHECK(asset.sha256.size() == 64);
        CHECK(asset.bytes > 0);
        CHECK_FALSE(asset.installDir.isEmpty());
        CHECK_FALSE(asset.files.isEmpty());
        CHECK(groups().contains(asset.group));
        CHECK_FALSE(ids.contains(asset.id));
        ids.insert(asset.id);
        if (asset.isArchive()) CHECK(asset.url.endsWith(u".tar.bz2"_s));
    }
    CHECK(all().size() == 9);
    CHECK(totalBytes(all()) > 400'000'000);
    CHECK(sizeText(totalBytes(all())).endsWith(u" MB"_s));
    CHECK(sizeText(1'234'567'890) == u"1.2 GB"_s);
    CHECK(sizeText(4'885) == u"5 KB"_s);
}

TEST_CASE("missing() sees what is installed, file by file") {
    QTemporaryDir data;
    CHECK(missing(data.path()).size() == all().size());
    touch(data.path() + u"/models/ggml-base.en.bin"_s);
    for (const QString &file : {u"encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s, u"decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s,
                                u"joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s, u"tokens.txt"_s}) {
        touch(data.path() + u"/sherpa/kws/"_s + file);       // bpe.model still missing: the archive counts as missing
    }
    const QList<Asset> left = missing(data.path());
    QStringList ids;
    for (const Asset &asset : left) ids.append(asset.id);
    CHECK_FALSE(ids.contains(u"whisper-base.en"_s));
    CHECK(ids.contains(u"kws"_s));
    touch(data.path() + u"/sherpa/kws/bpe.model"_s);
    ids.clear();
    for (const Asset &asset : missing(data.path())) ids.append(asset.id);
    CHECK_FALSE(ids.contains(u"kws"_s));
    CHECK(ids.size() == all().size() - 2);
    CHECK(groupTitle(u"own-voice"_s).contains(u"ZipVoice"_s));
}
