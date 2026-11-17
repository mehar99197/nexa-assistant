#include "core/account/UpdateChecker.h"

#include "core/account/LicenseToken.h"
#include "core/config/Portable.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr auto kDefaultFeed = "https://nexadownloadmanager.com/api/releases/feed?product=assistant&os=";
constexpr qsizetype kMaxFeedBytes = 256 * 1024;
constexpr qint64 kMaxInstallerBytes = 1024LL * 1024 * 1024; // 1 GiB

QByteArray feedSigningPayload(const QString &version, const QString &url, const QString &sha256) {
    return "nexa-update-v1\n" + version.toUtf8() + '\n' + url.toUtf8() + '\n' + sha256.toUtf8();
}

bool allowedFeedOrDownloadUrl(const QUrl &url) {
    if (!url.isValid()) return false;
    if (url.scheme() == u"https"_s) return true;
#ifdef NEXA_DEV_BUILD
    return url.scheme() == u"http"_s && (url.host() == u"localhost"_s || url.host() == u"127.0.0.1"_s);
#else
    return false;
#endif
}

QString safeFileName(const QString &version) {
    QString clean = version;
    clean.replace(QRegularExpression(u"[^A-Za-z0-9._-]"_s), u"_"_s);
    if (clean.isEmpty()) clean = u"latest"_s;
#ifdef Q_OS_WIN
    return u"NexaAssistantSetup-%1.exe"_s.arg(clean);
#else
    return u"nexa-assistant_%1_amd64.deb"_s.arg(clean);
#endif
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

UpdateChecker::~UpdateChecker() {
    cancelDownload();
}

QString UpdateChecker::platformKey() {
#if defined(Q_OS_WIN)
    return u"windows"_s;
#elif defined(Q_OS_MACOS)
    return u"macos"_s;
#else
    return u"linux"_s;
#endif
}

QString UpdateChecker::feedUrl() const {
#ifdef NEXA_DEV_BUILD
    const QString env = qEnvironmentVariable("NEXA_UPDATE_URL").trimmed();
    if (!env.isEmpty() && env.compare(u"off"_s, Qt::CaseInsensitive) != 0) return env;
#endif
    return QString::fromLatin1(kDefaultFeed) + platformKey();
}

bool UpdateChecker::isConfigured() const {
#ifdef NEXA_DEV_BUILD
    return qEnvironmentVariable("NEXA_UPDATE_URL").trimmed().compare(u"off"_s, Qt::CaseInsensitive) != 0;
#else
    return true;
#endif
}

bool UpdateChecker::isNewer(const QString &remote, const QString &current) {
    const QStringList a = remote.split(u'.', Qt::SkipEmptyParts);
    const QStringList b = current.split(u'.', Qt::SkipEmptyParts);
    const qsizetype n = std::max(a.size(), b.size());
    static const QRegularExpression nonDigit(u"\\D"_s);
    for (qsizetype i = 0; i < n; ++i) {
        const int av = (i < a.size()) ? a.at(i).section(nonDigit, 0, 0).toInt() : 0;
        const int bv = (i < b.size()) ? b.at(i).section(nonDigit, 0, 0).toInt() : 0;
        if (av != bv) return av > bv;
    }
    return false;
}

std::optional<UpdateChecker::ReleaseInfo> UpdateChecker::parseSignedFeed(const QByteArray &json, QString *error) {
    const auto fail = [error](const QString &why) -> std::optional<ReleaseInfo> {
        if (error) *error = why;
        return std::nullopt;
    };

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(u"update feed is not valid JSON"_s);
    }
    const QJsonObject obj = doc.object();
    const QString version = obj.value(u"version"_s).toString().trimmed();
    if (version.isEmpty()) return fail(u"update feed had no \"version\" field"_s);

    const QUrl dl(obj.value(u"url"_s).toString().trimmed());
    const QString dlUrl = allowedFeedOrDownloadUrl(dl) ? dl.toString() : QString();
    if (dlUrl.isEmpty()) return fail(u"update feed URL must be HTTPS"_s);

    static const QRegularExpression hexRe(u"\\A[0-9a-fA-F]{64}\\z"_s);
    const QString sha = obj.value(u"sha256"_s).toString().trimmed().toLower();
    if (!hexRe.match(sha).hasMatch()) return fail(u"update feed checksum is missing or invalid"_s);

    const QString encodedSig = obj.value(u"signature"_s).toString().trimmed();
    if (encodedSig.isEmpty()) return fail(u"update feed signature is missing"_s);
    const auto sigBytes = QByteArray::fromBase64Encoding(
        encodedSig.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!sigBytes) return fail(u"update feed signature is malformed"_s);

    if (!licensetoken::verifyDetachedSignature(feedSigningPayload(version, dlUrl, sha), *sigBytes)) {
        return fail(u"update feed signature is invalid"_s);
    }

    return ReleaseInfo{
        .version = version,
        .url = dlUrl,
        .notes = obj.value(u"notes"_s).toString(),
        .sha256 = sha,
    };
}

void UpdateChecker::check(const QString &currentVersion) {
    if (!isConfigured()) {
        emit checkFailed(tr("Update checks are disabled."));
        return;
    }
    const QUrl url(feedUrl());
    if (!allowedFeedOrDownloadUrl(url)) {
        emit checkFailed(tr("Update feed must use HTTPS."));
        return;
    }

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  u"NexaAssistant updater/%1 (%2)"_s.arg(currentVersion, platformKey()));
    req.setTransferTimeout(15'000);

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, currentVersion] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }
        const QByteArray body = reply->read(kMaxFeedBytes + 1);
        if (body.size() > kMaxFeedBytes) {
            emit checkFailed(tr("Update feed response is too large."));
            return;
        }

        // Compare version first: if already up to date, skip enforcing update payload checks.
        const QJsonObject peek = QJsonDocument::fromJson(body).object();
        const QString peekVer = peek.value(u"version"_s).toString().trimmed();
        if (!peekVer.isEmpty() && !isNewer(peekVer, currentVersion)) {
            emit upToDate();
            return;
        }

        QString reason;
        const std::optional<ReleaseInfo> info = parseSignedFeed(body, &reason);
        if (!info) {
            emit checkFailed(reason);
            return;
        }
        if (!isNewer(info->version, currentVersion)) {
            emit upToDate();
            return;
        }
        emit updateAvailable(info->version, info->url, info->notes, info->sha256);
    });
}

void UpdateChecker::downloadUpdate(const QString &url, const QString &expectedSha256, const QString &version) {
    cancelDownload();
    const QUrl parsed(url);
    static const QRegularExpression hexRe(u"\\A[0-9a-fA-F]{64}\\z"_s);
    const QString expected = expectedSha256.trimmed().toLower();
    if (!allowedFeedOrDownloadUrl(parsed) || !hexRe.match(expected).hasMatch()) {
        emit downloadFailed(tr("Update URL or checksum is invalid."));
        return;
    }

    const QString updatesDir = portable::cacheDir() + u"/updates"_s;
    if (!QDir().mkpath(updatesDir)) {
        emit downloadFailed(tr("Cannot create updates folder %1.").arg(updatesDir));
        return;
    }
    const QString targetPath = updatesDir + u'/' + safeFileName(version);
    const QString partPath = targetPath + u".part"_s;
    QFile::remove(partPath);
    auto outFile = std::make_shared<QFile>(partPath);
    if (!outFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFailed(tr("Cannot write %1.").arg(partPath));
        return;
    }

    auto hasher = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto overflow = std::make_shared<bool>(false);
    auto receivedBytes = std::make_shared<qint64>(0);

    QNetworkRequest req(parsed);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60'000);

    downloadReply_ = nam_->get(req);
    QNetworkReply *reply = downloadReply_;

    connect(reply, &QNetworkReply::downloadProgress, this, &UpdateChecker::downloadProgress);
    connect(reply, &QNetworkReply::readyRead, this, [reply, outFile, hasher, overflow, receivedBytes] {
        const QByteArray chunk = reply->readAll();
        *receivedBytes += chunk.size();
        if (*receivedBytes > kMaxInstallerBytes) {
            *overflow = true;
            reply->abort();
            return;
        }
        if (outFile->write(chunk) != chunk.size()) {
            reply->abort();
            return;
        }
        hasher->addData(chunk);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, outFile, hasher, overflow, partPath, targetPath, expected] {
                if (downloadReply_ == reply) downloadReply_ = nullptr;
                const QByteArray remaining = reply->readAll();
                if (!remaining.isEmpty() && !*overflow) {
                    outFile->write(remaining);
                    hasher->addData(remaining);
                }
                outFile->close();
                const bool ok = reply->error() == QNetworkReply::NoError && !*overflow;
                const QString errStr = reply->errorString();
                reply->deleteLater();

                if (!ok) {
                    QFile::remove(partPath);
                    emit downloadFailed(*overflow ? tr("Update download exceeded maximum size.") : errStr);
                    return;
                }
                const QString actual = QString::fromLatin1(hasher->result().toHex());
                if (actual != expected) {
                    QFile::remove(partPath);
                    emit downloadFailed(tr("Update download failed SHA-256 verification."));
                    return;
                }
                QFile::remove(targetPath);
                if (!QFile::rename(partPath, targetPath)) {
                    QFile::remove(partPath);
                    emit downloadFailed(tr("Cannot place verified update at %1.").arg(targetPath));
                    return;
                }
                emit downloadReady(targetPath);
            });
}

void UpdateChecker::cancelDownload() {
    if (!downloadReply_) return;
    downloadReply_->disconnect(this);
    downloadReply_->abort();
    downloadReply_->deleteLater();
    downloadReply_ = nullptr;
}

bool UpdateChecker::launchInstaller(const QString &filePath) {
    if (!QFileInfo::exists(filePath)) return false;
#ifdef Q_OS_WIN
    const std::wstring wide = QDir::toNativeSeparators(filePath).toStdWString();
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<qintptr>(result) > 32;
#else
    return QProcess::startDetached(u"xdg-open"_s, {filePath});
#endif
}

} // namespace nexa
