#pragma once

#include "system/android/AdbParsers.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa::mdns {

// The DNS wire format as mDNS / DNS-SD use it (RFC 1035, 6762, 6763), the little of it a phone's
// wireless-debugging advertisement needs: PTR (service type → instance), SRV (instance → host and
// port), A and AAAA (host → address). Pure, and tolerant: a record it cannot read is skipped, a
// packet it cannot read is refused.

enum class RecordType : quint16 { A = 1, Ptr = 12, Txt = 16, Aaaa = 28, Srv = 33 };

// Names keep the case they came in: the phone's hardware serial is part of its instance name, and
// matching is case-insensitive as DNS is.
struct Record {
    QString name;           // the owner, without the trailing dot
    quint16 type{0};
    quint32 ttl{0};         // 0: a goodbye — the service is going away
    QString target;         // PTR: the instance; SRV: the host; A/AAAA: the address, as text
    quint16 port{0};        // SRV
};

struct Message {
    bool response{false};
    QStringList questions;  // the names asked about
    QList<Record> records;  // answers, authority and additional records, in that order
};

std::optional<Message> decode(QByteArrayView packet);

// One query asking for PTR records of each name (the service types to browse for).
QByteArray query(const QStringList &names);

// The service types adb's wireless debugging advertises.
inline const QString kConnectService = QStringLiteral("_adb-tls-connect._tcp.local");
inline const QString kPairingService = QStringLiteral("_adb-tls-pairing._tcp.local");

// What has been heard, folded together: instances from PTR and SRV records, their hosts' addresses
// from A and AAAA records, the sender's address as the fallback, and everything forgotten when its
// TTL runs out or a goodbye arrives. Pure: time comes in as an argument.
class ServiceTable {
public:
    static constexpr int kMaxTtlSeconds = 5 * 60;

    void absorb(const Message &message, const QHostAddress &sender, const QDateTime &now);
    // The services alive at `now`, as `adb mdns services` would list them: IPv4 first, else the
    // sender's address, else IPv6.
    QList<MdnsService> services(const QDateTime &now) const;
    bool isEmpty() const { return instances_.isEmpty(); }

private:
    struct Instance {
        QString name;           // the full instance name as advertised
        MdnsService::Kind kind{MdnsService::Kind::Other};
        QString host;           // the SRV target, lower-case
        int port{0};
        QHostAddress sender;
        QDateTime expires;
    };
    Instance &instance(const QString &name, MdnsService::Kind kind);

    QHash<QString, Instance> instances_;       // by lower-case instance name
    QHash<QString, QHostAddress> ipv4_;        // by lower-case host name
    QHash<QString, QHostAddress> ipv6_;
};

} // namespace nexa::mdns
