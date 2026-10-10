#include "system/android/MdnsBrowser.h"

#include "core/Logging.h"

#include <QDateTime>
#include <QNetworkDatagram>
#include <QNetworkInterface>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QHostAddress kGroup(QStringLiteral("224.0.0.251"));

bool canMulticast(const QNetworkInterface &interface) {
    const QNetworkInterface::InterfaceFlags flags = interface.flags();
    return (flags & QNetworkInterface::IsUp) && (flags & QNetworkInterface::IsRunning)
           && (flags & QNetworkInterface::CanMulticast) && !(flags & QNetworkInterface::IsLoopBack);
}

} // namespace

MdnsBrowser::MdnsBrowser(QObject *parent) : QObject(parent) {
    connect(&socket_, &QUdpSocket::readyRead, this, &MdnsBrowser::readDatagrams);
}

void MdnsBrowser::setActive(bool active) {
    if (active == isActive()) return;
    if (!active) {
        socket_.close();
        return;
    }
    problem_.clear();
    if (!socket_.bind(QHostAddress::AnyIPv4, kPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        problem_ = socket_.errorString();
        qCWarning(lcApp).noquote() << "mdns: cannot listen on port" << kPort << "—" << problem_;
        return;
    }
    socket_.setSocketOption(QAbstractSocket::MulticastTtlOption, 255);      // what RFC 6762 asks for
    joinGroups();
}

void MdnsBrowser::joinGroups() {
    int joined = 0;
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        if (canMulticast(interface) && socket_.joinMulticastGroup(kGroup, interface)) ++joined;
    }
    if (joined == 0 && !socket_.joinMulticastGroup(kGroup)) {
        problem_ = u"no network interface can multicast"_s;
        qCWarning(lcApp).noquote() << "mdns:" << problem_;
    }
}

void MdnsBrowser::query() {
    if (!isActive()) return;
    const QByteArray packet = mdns::query({mdns::kConnectService, mdns::kPairingService});
    bool sent = false;
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        if (!canMulticast(interface)) continue;
        socket_.setMulticastInterface(interface);
        sent = socket_.writeDatagram(packet, kGroup, kPort) == packet.size() || sent;
    }
    if (!sent) socket_.writeDatagram(packet, kGroup, kPort);
}

QList<MdnsService> MdnsBrowser::services() const {
    return table_.services(QDateTime::currentDateTimeUtc());
}

void MdnsBrowser::readDatagrams() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    bool heard = false;
    while (socket_.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket_.receiveDatagram();
        if (!datagram.isValid()) continue;
        const std::optional<mdns::Message> message = mdns::decode(datagram.data());
        if (!message || !message->response) continue;
        table_.absorb(*message, datagram.senderAddress(), now);
        heard = true;
    }
    if (!heard) return;
    const QList<MdnsService> services = table_.services(now);
    if (services.size() == lastReported_.size()
        && std::equal(services.cbegin(), services.cend(), lastReported_.cbegin(), [](const MdnsService &a, const MdnsService &b) {
               return a.name == b.name && a.kind == b.kind && a.address() == b.address();
           })) {
        return;
    }
    lastReported_ = services;
    emit changed();
}

} // namespace nexa
