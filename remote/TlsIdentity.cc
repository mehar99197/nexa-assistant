#include "remote/TlsIdentity.h"

#include "core/Logging.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include <QStandardPaths>
#include <QTimer>

#include <sys/stat.h>

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

// "AB:CD:…", the way browsers show a fingerprint.
QString colonHex(const QByteArray &bytes) {
    QStringList parts;
    parts.reserve(bytes.size());
    for (const char byte : bytes) parts.append(u"%1"_s.arg(uchar(byte), 2, 16, u'0').toUpper());
    return parts.join(u':');
}

} // namespace

TlsIdentity::TlsIdentity(QString directory, QObject *parent) : QObject(parent), directory_(std::move(directory)) {}

QString TlsIdentity::defaultDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/nexa/tls"_s;
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
    fingerprint_ = colonHex(certificate.digest(QCryptographicHash::Sha256));
    return configuration;
}

void TlsIdentity::ensure(const QStringList &addresses, Done done) {
    if (Result<QSslConfiguration> loaded = load(); loaded) {
        done(std::move(loaded));
        return;
    }
    if (QStandardPaths::findExecutable(u"openssl"_s).isEmpty()) {
        done(Error{ErrorCode::NotSupported, u"openssl is not installed, so the web remote cannot use HTTPS"_s});
        return;
    }

    const QByteArray directory = QFile::encodeName(directory_);
    QDir().mkpath(directory_);
    ::chmod(directory.constData(), 0700);

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
    process->setChildProcessModifier([] { ::umask(077); });
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
        qCInfo(lcApp) << "remote: made a new certificate";
        finish(load());
    });
    timeout->start(kOpensslTimeoutMs);
    process->start(u"openssl"_s, arguments);
}

} // namespace nexa
