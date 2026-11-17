#include "app/SingleInstance.h"

#include "core/Logging.h"

#include <QDir>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kConnectTimeoutMs = 1'000;
constexpr qsizetype kMaxMessage = 64 * 1024;

} // namespace

SingleInstance::SingleInstance(const QString &name, QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
    path_ = name;
#else
    QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (directory.isEmpty()) directory = QDir::tempPath();
    path_ = directory + u'/' + name + u".socket"_s;
#endif
}

SingleInstance::~SingleInstance() {
    if (server_) server_->close();
}

bool SingleInstance::handOver(const QJsonObject &message) {
    QLocalSocket socket;
    socket.connectToServer(path_);
    if (!socket.waitForConnected(kConnectTimeoutMs)) return false;     // nobody is running
    socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    const bool sent = socket.waitForBytesWritten(kConnectTimeoutMs);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) socket.waitForDisconnected(kConnectTimeoutMs);
    return sent;
}

bool SingleInstance::listen() {
    server_ = new QLocalServer(this);
    // The socket only answers this user, whatever directory it ended up in.
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, &SingleInstance::accept);
    if (server_->listen(path_)) return true;
    // A socket left behind by a copy that crashed. handOver() has just failed to reach anyone
    // through it, so it is safe to take over.
    if (server_->serverError() == QAbstractSocket::AddressInUseError) {
        QLocalServer::removeServer(path_);
        if (server_->listen(path_)) return true;
    }
    qCWarning(lcApp).noquote() << "cannot listen for other copies of Nexa:" << server_->errorString();
    return false;
}

void SingleInstance::accept() {
    while (QLocalSocket *socket = server_->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            if (!socket->canReadLine()) {
                if (socket->bytesAvailable() > kMaxMessage) socket->abort();
                return;
            }
            const QJsonObject message = QJsonDocument::fromJson(socket->readLine(kMaxMessage)).object();
            socket->disconnectFromServer();
            if (!message.isEmpty()) emit messageReceived(message);
        });
    }
}

} // namespace nexa
