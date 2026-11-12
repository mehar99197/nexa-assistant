#pragma once

#include "core/Result.h"
#include "system/setup/EngineAssets.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

class QNetworkReply;
class QProcess;

namespace nexa {

// Fetches the models EngineAssets lists into the data folder, one at a time (Facade over the network, the disk
// and `tar`): each download goes to <cache>/<file>.part and resumes from there with a Range request, is checked
// against its SHA-256 (on a worker thread) and only then moved into place; an archive is unpacked with `tar` into
// a scratch folder and only the listed members are installed. Nothing blocks the GUI thread but the signals.
class AssetDownloader : public QObject {
    Q_OBJECT
public:
    explicit AssetDownloader(QString dataDir, QObject *parent = nullptr);
    ~AssetDownloader() override;

    static QString defaultCacheDir();                   // ~/.cache/nexa/downloads, where the scripts keep theirs too
    void setCacheDir(const QString &dir) { cacheDir_ = dir; }
    void setTarProgram(const QString &path) { tar_ = path; }       // the tests: one that does not exist
    void setIdleTimeoutMs(int ms) { idleTimeoutMs_ = ms; }

    void start(const QList<engineassets::Asset> &assets);
    void cancel();
    bool isRunning() const { return running_; }
    QString currentAssetId() const { return running_ ? current_.id : QString(); }
    QString dataDir() const { return dataDir_; }

signals:
    void progress(const QString &assetId, qint64 received, qint64 total);
    void assetInstalled(const QString &assetId);
    void finished();                                    // every asset asked for is installed
    void failed(const QString &assetId, const nexa::Error &error);      // and the run stopped

private:
    void next();
    void fetch();
    void onReadyRead();
    void onReplyFinished();
    void verify();
    void install();
    void installFile();
    void installArchive();
    void fail(const Error &error);
    QString partPath() const;
    QString fileName() const;

    QString dataDir_;
    QString cacheDir_;
    QString tar_{QStringLiteral("tar")};
    int idleTimeoutMs_{60'000};
    QNetworkAccessManager manager_;
    QThreadPool pool_;
    QList<engineassets::Asset> queue_;
    engineassets::Asset current_;
    QNetworkReply *reply_{nullptr};
    QProcess *tarProcess_{nullptr};
    QFile part_;
    QTimer idle_;
    qint64 received_{0};
    qint64 resumedFrom_{0};
    quint64 generation_{0};
    bool running_{false};
};

} // namespace nexa
