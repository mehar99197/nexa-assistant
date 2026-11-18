#include "system/setup/AssetDownloader.h"

#include "core/Logging.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include "core/config/Portable.h"
#include <QTemporaryDir>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr QFileDevice::Permissions kPrivate = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

Result<QString> sha256Of(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Error{ErrorCode::Io, u"Cannot read %1: %2"_s.arg(path, file.errorString())};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return Error{ErrorCode::Io, u"Cannot read %1."_s.arg(path)};
    return QString::fromLatin1(hash.result().toHex());
}

// Moves a file into place, copying when the two folders are on different file systems.
Result<void> moveInto(const QString &from, const QString &to) {
    QDir().mkpath(QFileInfo(to).path());
    QFile::remove(to);
    if (QFile::rename(from, to) || (QFile::copy(from, to) && QFile::remove(from))) {
        QFile::setPermissions(to, kPrivate | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        return {};
    }
    return Error{ErrorCode::Io, u"Cannot place %1."_s.arg(to)};
}

} // namespace

AssetDownloader::AssetDownloader(QString dataDir, QObject *parent)
    : QObject(parent), dataDir_(std::move(dataDir)), cacheDir_(defaultCacheDir()) {
    pool_.setMaxThreadCount(1);
    idle_.setSingleShot(true);
    connect(&idle_, &QTimer::timeout, this, [this] {
        if (reply_) reply_->abort();
    });
}

AssetDownloader::~AssetDownloader() {
    cancel();
    pool_.waitForDone();
}

QString AssetDownloader::defaultCacheDir() {
    return portable::cacheDir() + u"/downloads"_s;
}

void AssetDownloader::start(const QList<engineassets::Asset> &assets) {
    if (running_) return;
    queue_ = assets;
    running_ = !queue_.isEmpty();
    ++generation_;
    if (!running_) {
        emit finished();
        return;
    }
    QDir().mkpath(cacheDir_);
    next();
}

void AssetDownloader::cancel() {
    if (!running_) return;
    ++generation_;
    running_ = false;
    queue_.clear();
    idle_.stop();
    if (reply_) {
        QNetworkReply *reply = std::exchange(reply_, nullptr);
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (part_.isOpen()) part_.close();      // the .part stays: the next start resumes it
    if (tarProcess_) {
        tarProcess_->disconnect(this);
        tarProcess_->kill();
        tarProcess_->deleteLater();
        tarProcess_ = nullptr;
    }
}

void AssetDownloader::next() {
    if (queue_.isEmpty()) {
        running_ = false;
        emit finished();
        return;
    }
    current_ = queue_.takeFirst();
    fetch();
}

QString AssetDownloader::fileName() const {
    return QUrl(current_.url).fileName();
}

QString AssetDownloader::partPath() const {
    return cacheDir_ + u'/' + fileName() + u".part"_s;
}

void AssetDownloader::fetch() {
    // What is there already is kept: the request asks for the rest.
    part_.setFileName(partPath());
    resumedFrom_ = part_.exists() ? part_.size() : 0;
    if (resumedFrom_ >= current_.bytes && current_.bytes > 0) {
        received_ = resumedFrom_;
        verify();
        return;
    }
    if (!part_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        fail(Error{ErrorCode::Io, u"Cannot write %1: %2"_s.arg(part_.fileName(), part_.errorString())});
        return;
    }
    QFile::setPermissions(part_.fileName(), kPrivate);
    received_ = resumedFrom_;
    QNetworkRequest request{QUrl(current_.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, u"nexa-assistant"_s);
    if (resumedFrom_ > 0) request.setRawHeader("Range", "bytes=" + QByteArray::number(resumedFrom_) + "-");
    reply_ = manager_.get(request);
    const quint64 generation = generation_;
    connect(reply_, &QNetworkReply::readyRead, this, [this, generation] {
        if (generation == generation_) onReadyRead();
    });
    connect(reply_, &QNetworkReply::finished, this, [this, generation] {
        if (generation == generation_) onReplyFinished();
    });
    idle_.start(idleTimeoutMs_);
    emit progress(current_.id, received_, current_.bytes);
}

void AssetDownloader::onReadyRead() {
    idle_.start(idleTimeoutMs_);
    const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (resumedFrom_ > 0 && status == 200 && received_ == resumedFrom_) {
        // The server ignored the range: start the file over.
        part_.close();
        part_.remove();
        if (!part_.open(QIODevice::WriteOnly)) {
            fail(Error{ErrorCode::Io, u"Cannot write %1."_s.arg(part_.fileName())});
            return;
        }
        received_ = 0;
        resumedFrom_ = 0;
    }
    const QByteArray bytes = reply_->readAll();
    if (part_.write(bytes) != bytes.size()) {
        fail(Error{ErrorCode::Io, u"The disk refused %1 — is it full?"_s.arg(part_.fileName())});
        return;
    }
    received_ += bytes.size();
    emit progress(current_.id, received_, current_.bytes);
}

void AssetDownloader::onReplyFinished() {
    idle_.stop();
    QNetworkReply *reply = reply_;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || (status != 200 && status != 206)) {
        reply_ = nullptr;
        const bool timedOut = reply->error() == QNetworkReply::OperationCanceledError;
        const QString said = reply->errorString();
        reply->deleteLater();
        part_.close();
        fail(Error{timedOut ? ErrorCode::Timeout : ErrorCode::Network,
                   timedOut ? u"%1 stopped arriving; what came is kept, try again."_s.arg(fileName())
                            : u"%1 could not be downloaded: %2 (what came is kept, try again)."_s.arg(fileName(), said)});
        return;
    }
    onReadyRead();      // whatever is still buffered (reply_ still set; a disk error in there cancels everything)
    if (!running_ || reply_ != reply) return;
    reply_ = nullptr;
    reply->deleteLater();
    part_.close();
    verify();
}

void AssetDownloader::verify() {
    const QPointer<AssetDownloader> self(this);
    const QString path = partPath();
    const QString expected = current_.sha256;
    const quint64 generation = generation_;
    pool_.start([self, path, expected, generation] {
        const Result<QString> hash = sha256Of(path);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, hash, expected, generation] {
            if (generation != self->generation_) return;
            if (!hash) {
                self->fail(hash.error());
                return;
            }
            if (hash.value() != expected) {
                QFile::remove(self->partPath());
                self->fail(Error{ErrorCode::Parse,
                                 u"%1 did not match its checksum — the download was removed; try again later."_s.arg(self->fileName())});
                return;
            }
            self->install();
        }, Qt::QueuedConnection);
    });
}

void AssetDownloader::install() {
    if (current_.isArchive()) {
        installArchive();
    } else {
        installFile();
    }
}

void AssetDownloader::installFile() {
    const QString target = dataDir_ + u'/' + current_.installDir + u'/' + current_.files.first();
    if (const Result<void> moved = moveInto(partPath(), target); !moved) {
        fail(moved.error());
        return;
    }
    qCInfo(lcApp).noquote() << "installed" << target;
    emit assetInstalled(current_.id);
    next();
}

void AssetDownloader::installArchive() {
    auto scratch = std::make_shared<QTemporaryDir>(cacheDir_ + u"/unpack-XXXXXX"_s);
    if (!scratch->isValid()) {
        fail(Error{ErrorCode::Io, u"Cannot make a folder to unpack %1 in."_s.arg(fileName())});
        return;
    }
    tarProcess_ = new QProcess(this);
    const quint64 generation = generation_;
    connect(tarProcess_, &QProcess::finished, this, [this, scratch, generation](int exitCode, QProcess::ExitStatus status) {
        if (generation != generation_) return;
        QProcess *tar = std::exchange(tarProcess_, nullptr);
        const QString said = QString::fromUtf8(tar->readAllStandardError()).trimmed();
        tar->deleteLater();
        if (status != QProcess::NormalExit || exitCode != 0) {
            fail(Error{ErrorCode::Io, u"%1 could not be unpacked: %2"_s.arg(fileName(), said.isEmpty() ? u"tar failed"_s : said)});
            return;
        }
        for (const QString &member : current_.files) {
            const QString from = scratch->path() + u'/' + current_.archiveTop + u'/' + member;
            const QString to = dataDir_ + u'/' + current_.installDir + u'/' + QFileInfo(member).fileName();
            if (!QFileInfo::exists(from)) {
                fail(Error{ErrorCode::Parse, u"%1 does not contain %2."_s.arg(fileName(), member)});
                return;
            }
            if (const Result<void> moved = moveInto(from, to); !moved) {
                fail(moved.error());
                return;
            }
        }
        QFile::remove(partPath());
        qCInfo(lcApp).noquote() << "installed" << current_.id << "into" << dataDir_ + u'/' + current_.installDir;
        emit assetInstalled(current_.id);
        next();
    });
    connect(tarProcess_, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) {
        if (generation != generation_ || error != QProcess::FailedToStart) return;
        QProcess *tar = std::exchange(tarProcess_, nullptr);
        tar->deleteLater();
        fail(Error{ErrorCode::NotSupported, u"tar is needed to unpack %1 and was not found (%2)."_s.arg(fileName(), tar_)});
    });
    QString tarProgram = tar_;
#ifdef Q_OS_WIN
    if (tarProgram == u"tar"_s) {
        const QString winTar =
            QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", u"C:/Windows"_s)) + u"/System32/tar.exe"_s;
        if (QFileInfo::exists(winTar)) tarProgram = winTar;
    }
#endif
    tarProcess_->start(tarProgram, {u"-xf"_s, partPath(), u"-C"_s, scratch->path()});
}

void AssetDownloader::fail(const Error &error) {
    const QString id = current_.id;
    qCWarning(lcApp).noquote() << "download of" << id << "failed:" << error.message;
    if (part_.isOpen()) part_.close();
    cancel();
    emit failed(id, error);
}

} // namespace nexa
