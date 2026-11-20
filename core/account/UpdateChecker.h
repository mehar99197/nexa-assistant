#pragma once

#include <QObject>
#include <QString>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace nexa {

// Checks /api/releases/feed?os=<platform>&product=assistant, verifies Ed25519 signature over
// `nexa-update-v1\n<version>\n<url>\n<sha256>`, downloads & verifies SHA-256, and opens/runs.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    struct ReleaseInfo {
        QString version;
        QString url;
        QString notes;
        QString sha256;
    };

    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    [[nodiscard]] bool isConfigured() const;
    [[nodiscard]] QString feedUrl() const;
    void check(const QString &currentVersion);

    // Downloads `url`, verifies streaming SHA-256 == `expectedSha256`, saves under cacheDir/updates.
    void downloadUpdate(const QString &url, const QString &expectedSha256, const QString &version);
    void cancelDownload();

    // Launches the downloaded .exe installer on Windows or opens the .deb/folder on Linux.
    [[nodiscard]] static bool launchInstaller(const QString &filePath);

    [[nodiscard]] static QString platformKey();
    [[nodiscard]] static bool isNewer(const QString &remote, const QString &current);

    // Pure parser/verifier (unit-tested): validates feed JSON, HTTPS url, 64-hex sha256 & Ed25519 signature.
    [[nodiscard]] static std::optional<ReleaseInfo> parseSignedFeed(const QByteArray &json, QString *error = nullptr);

signals:
    void updateAvailable(const QString &version, const QString &url, const QString &notes, const QString &sha256);
    void upToDate();
    void checkFailed(const QString &reason);
    void downloadProgress(qint64 received, qint64 total);
    void downloadReady(const QString &filePath);
    void downloadFailed(const QString &reason);

private:
    QNetworkAccessManager *nam_{nullptr};
    QNetworkReply *downloadReply_{nullptr};
};

} // namespace nexa
