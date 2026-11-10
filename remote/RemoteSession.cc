#include "remote/RemoteSession.h"

#include "core/Logging.h"
#include "remote/PairingManager.h"
#include "remote/RemoteProtocol.h"

#include <QWebSocket>

using namespace Qt::StringLiterals;

namespace nexa {

RemoteSession::RemoteSession(std::unique_ptr<QWebSocket> socket, PairingManager *pairing, QObject *parent)
    : QObject(parent), socket_(std::move(socket)), pairing_(pairing) {
    clock_.start();
    // Nothing bigger than one utterance gets through: the limit is enforced while reading.
    socket_->setMaxAllowedIncomingFrameSize(remote::kMaxAudioFrame);
    socket_->setMaxAllowedIncomingMessageSize(remote::kMaxAudioFrame);
    connect(socket_.get(), &QWebSocket::textMessageReceived, this, &RemoteSession::onText);
    connect(socket_.get(), &QWebSocket::binaryMessageReceived, this, &RemoteSession::onBinary);
    connect(socket_.get(), &QWebSocket::disconnected, this, &RemoteSession::closed);

    helloTimeout_.setSingleShot(true);
    connect(&helloTimeout_, &QTimer::timeout, this, [this] { close(u"Say hello first."_s); });
    helloTimeout_.start(kHelloTimeoutMs);
}

RemoteSession::~RemoteSession() {
    if (socket_) socket_->disconnect(this);
}

void RemoteSession::send(const QByteArray &message) {
    if (!socket_ || closing_) return;
    socket_->sendTextMessage(QString::fromUtf8(message));
}

void RemoteSession::close(const QString &reason) {
    if (!socket_ || closing_) return;
    closing_ = true;
    helloTimeout_.stop();
    socket_->close(QWebSocketProtocol::CloseCodePolicyViolated, reason);
}

bool RemoteSession::allowRequest() {
    const qint64 now = clock_.elapsed();
    recent_.removeIf([now](qint64 at) { return now - at > 60'000; });
    if (recent_.size() >= kMaxRequestsPerMinute) return false;
    recent_.append(now);
    return true;
}

void RemoteSession::onText(const QString &frame) {
    const std::optional<remote::ClientMessage> message = remote::decode(frame.toUtf8());

    if (!isAuthenticated()) {
        // Before hello, nothing else is listened to at all.
        if (!message || message->type != remote::ClientMessage::Type::Hello) {
            close(u"Say hello first."_s);
            return;
        }
        const std::optional<PairedDevice> device = pairing_->authenticate(message->token);
        if (!device) {
            send(remote::error(u"unauthorized"_s, u"This phone is not paired, or its pairing was revoked."_s));
            close(u"Not paired."_s);
            qCInfo(lcApp).noquote() << "remote: refused an unknown token from" << socket_->peerAddress().toString();
            return;
        }
        helloTimeout_.stop();
        deviceId_ = device->id;
        deviceName_ = device->name;
        qCInfo(lcApp).noquote() << "remote:" << deviceName_ << "connected from" << socket_->peerAddress().toString();
        emit authenticated();
        return;
    }

    if (!message) {
        send(remote::error(u"invalid"_s, u"That message could not be read."_s));
        return;
    }
    switch (message->type) {
    case remote::ClientMessage::Type::Hello:
        return;     // already said
    case remote::ClientMessage::Type::Ping:
        send(remote::pong());
        return;
    case remote::ClientMessage::Type::Cancel:
        emit cancelRequested();
        return;
    case remote::ClientMessage::Type::Unlock:
        qCInfo(lcApp).noquote() << "remote:" << deviceName_ << "asked to unlock the keyboard and mouse";
        emit unlockRequested();
        return;
    case remote::ClientMessage::Type::Answer:
        emit answered(message->id, message->allow);
        return;
    case remote::ClientMessage::Type::Text:
        if (!allowRequest()) {
            send(remote::error(u"too-fast"_s, u"That is a lot of requests at once; wait a moment."_s));
            return;
        }
        emit textRequest(message->text);
        return;
    }
}

void RemoteSession::onBinary(const QByteArray &frame) {
    if (!isAuthenticated()) {
        close(u"Say hello first."_s);
        return;
    }
    // 16-bit samples come in pairs of bytes; a tenth of a second is too short to say anything.
    if (frame.size() % 2 != 0 || frame.size() < 3'200 || frame.size() > remote::kMaxAudioFrame) {
        send(remote::error(u"invalid"_s, u"That recording could not be used."_s));
        return;
    }
    if (!allowRequest()) {
        send(remote::error(u"too-fast"_s, u"That is a lot of requests at once; wait a moment."_s));
        return;
    }
    emit audioRequest(frame);
}

} // namespace nexa
