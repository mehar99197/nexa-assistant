#include "remote/TlsIdentity.h"

#include "core/Logging.h"
#include "remote/RemoteClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include "core/config/Portable.h"
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include <memory>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// The longest a browser accepts for a server certificate.
constexpr int kValidDays = 825;
constexpr int kOpensslTimeoutMs = 30'000;

QString certificatePath(const QString &directory) {
    return directory + u"/cert.pem"_s;
}

QString keyPath(const QString &directory) {
    return directory + u"/key.pem"_s;
}

QString opensslProgram() {
#ifdef Q_OS_WIN
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        if (const QString bundled = QStandardPaths::findExecutable(u"openssl"_s, {appDir}); !bundled.isEmpty()) {
            return bundled;
        }
    }
    if (const QString onPath = QStandardPaths::findExecutable(u"openssl"_s); !onPath.isEmpty()) return onPath;
    QStringList fallbacks;
    for (const QString &root : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)")}) {
        if (!root.isEmpty()) {
            fallbacks.append({root + u"/OpenSSL-Win64/bin"_s, root + u"/Git/usr/bin"_s, root + u"/Git/mingw64/bin"_s});
        }
    }
    return QStandardPaths::findExecutable(u"openssl"_s, fallbacks);
#else
    return QStandardPaths::findExecutable(u"openssl"_s);
#endif
}

} // namespace

TlsIdentity::TlsIdentity(QString directory, QObject *parent) : QObject(parent), directory_(std::move(directory)) {}

QString TlsIdentity::defaultDirectory() {
    return portable::configDir() + u"/tls"_s;
}

Result<QSslConfiguration> TlsIdentity::load() {
    QFile certificateFile(certificatePath(directory_));
    QFile keyFile(keyPath(directory_));
    if (!certificateFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
        return Error{ErrorCode::NotFound, u"there is no certificate yet"_s};
    }
    const QSslCertificate certificate(certificateFile.readAll(), QSsl::Pem);
    const QSslKey key(keyFile.readAll(), QSsl::Ec, QSsl::Pem);
    if (certificate.isNull() || key.isNull()) {
        return Error{ErrorCode::Io, u"the certificate or its key cannot be read"_s};
    }
    // A day to spare, so it does not run out while a phone is connected.
    if (certificate.expiryDate() < QDateTime::currentDateTimeUtc().addDays(1)) {
        return Error{ErrorCode::NotFound, u"the certificate has expired"_s};
    }
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificate(certificate);
    configuration.setPrivateKey(key);
    configuration.setProtocol(QSsl::TlsV1_2OrLater);
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);     // browsers present no certificate of their own
    fingerprint_ = fingerprintOf(certificate);      // the phone app compares against exactly this
    return configuration;
}

void TlsIdentity::ensure(const QStringList &addresses, Done done) {
    if (Result<QSslConfiguration> loaded = load(); loaded) {
        done(std::move(loaded));
        return;
    }
    const QString openssl = opensslProgram();
    if (openssl.isEmpty()) {
        done(Error{ErrorCode::NotSupported, u"openssl is not installed, so the web remote cannot use HTTPS"_s});
        return;
    }

    const QByteArray directory = QFile::encodeName(directory_);
    QDir().mkpath(directory_);
#ifdef Q_OS_UNIX
    ::chmod(directory.constData(), 0700);
#endif

    const QString host = QHostInfo::localHostName();
    QStringList names{u"DNS:localhost"_s, u"IP:127.0.0.1"_s};
    if (!host.isEmpty()) names.prepend(u"DNS:"_s + host);
    for (const QString &address : addresses) names.append(u"IP:"_s + address);
    const QStringList arguments{
        u"req"_s, u"-x509"_s, u"-newkey"_s, u"ec"_s, u"-pkeyopt"_s, u"ec_paramgen_curve:prime256v1"_s, u"-nodes"_s,
        u"-days"_s, QString::number(kValidDays),
        u"-subj"_s, u"/CN=Nexa on %1"_s.arg(host.isEmpty() ? u"this PC"_s : host),
        u"-addext"_s, u"subjectAltName="_s + names.join(u','),
        u"-addext"_s, u"extendedKeyUsage=serverAuth"_s,
        u"-keyout"_s, keyPath(directory_), u"-out"_s, certificatePath(directory_),
    };

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    // The key is the user's alone from the moment it exists, not tightened afterwards.
#ifdef Q_OS_UNIX
    process->setChildProcessModifier([] { ::umask(077); });
#endif
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    auto answered = std::make_shared<bool>(false);
    const auto finish = [process, answered, done](Result<QSslConfiguration> result) {
        if (*answered) return;
        *answered = true;
        process->deleteLater();
        done(std::move(result));
    };
    connect(timeout, &QTimer::timeout, this, [process, finish] {
        process->kill();
        finish(Error{ErrorCode::Timeout, u"openssl took too long to make a certificate"_s});
    });
    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(Error{ErrorCode::NotSupported, process->errorString()});
    });
    connect(process, &QProcess::finished, this, [this, process, finish](int exitCode, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
        if (status != QProcess::NormalExit || exitCode != 0) {
            finish(Error{ErrorCode::Io, u"openssl could not make a certificate: %1"_s.arg(output.left(300))});
            return;
        }
        QFile::setPermissions(keyPath(directory_), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        qCInfo(lcApp) << "remote: made a new certificate";
        finish(load());
    });
    timeout->start(kOpensslTimeoutMs);
    process->start(openssl, arguments);
}

} // namespace nexa
