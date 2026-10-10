#pragma once

#include "system/android/AdbParsers.h"
#include "system/android/MdnsPacket.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QUdpSocket>

namespace nexa {

// Nexa's own DNS-SD browser for a phone's wireless debugging (Observer over a multicast UDP socket):
// asks the network for `_adb-tls-connect` and `_adb-tls-pairing`, listens for the answers and for
// the phone's own announcements, and keeps what it heard until it expires. It exists because
// Debian's adb is built without mDNS — `adb mdns services` never finds anything there — and a phone
// that cannot be found cannot be reconnected to. Listening happens only while something asks
// (setActive), on IPv4; the socket is shared, so Avahi can keep its own.
class MdnsBrowser : public QObject {
    Q_OBJECT
public:
    static constexpr quint16 kPort = 5353;

    explicit MdnsBrowser(QObject *parent = nullptr);

    void setActive(bool active);
    bool isActive() const { return socket_.state() == QAbstractSocket::BoundState; }
    // Why the socket could not be opened, else empty.
    QString problem() const { return problem_; }

    // Sends the questions out of every interface that can multicast. Nothing happens when inactive.
    void query();
    QList<MdnsService> services() const;

signals:
    void changed();

private:
    void readDatagrams();
    void joinGroups();

    QUdpSocket socket_;
    mdns::ServiceTable table_;
    QString problem_;
    QList<MdnsService> lastReported_;
};

} // namespace nexa
