#include "system/android/MdnsPacket.h"

#include <QtEndian>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa::mdns {

namespace {

constexpr qsizetype kHeaderSize = 12;
constexpr int kMaxLabels = 128;
constexpr quint16 kClassMask = 0x7FFF;     // the top bit is mDNS's cache-flush flag

quint16 u16(QByteArrayView packet, qsizetype at) {
    return qFromBigEndian<quint16>(packet.data() + at);
}

quint32 u32(QByteArrayView packet, qsizetype at) {
    return qFromBigEndian<quint32>(packet.data() + at);
}

// Reads a name at `at`, following compression pointers, and leaves `at` after the name as it
// appears in the packet (after the pointer, when there is one). False when the packet is cut short
// or the pointers chase each other.
bool readName(QByteArrayView packet, qsizetype &at, QString &name) {
    QStringList labels;
    qsizetype position = at;
    std::optional<qsizetype> after;
    for (int hops = 0; hops < kMaxLabels; ++hops) {
        if (position >= packet.size()) return false;
        const quint8 length = quint8(packet[position]);
        if ((length & 0xC0) == 0xC0) {
            if (position + 1 >= packet.size()) return false;
            const qsizetype pointer = u16(packet, position) & 0x3FFF;
            if (pointer >= position) return false;      // only backwards, so it cannot loop
            if (!after) after = position + 2;
            position = pointer;
            continue;
        }
        if (length & 0xC0) return false;               // the two other forms are not used
        ++position;
        if (length == 0) {
            at = after.value_or(position);
            name = labels.join(u'.');
            return true;
        }
        if (position + length > packet.size()) return false;
        labels.append(QString::fromUtf8(packet.data() + position, length));
        position += length;
    }
    return false;
}

QByteArray labels(const QString &name) {
    QByteArray out;
    for (const QString &label : name.split(u'.', Qt::SkipEmptyParts)) {
        const QByteArray utf8 = label.toUtf8().left(63);
        out.append(char(utf8.size()));
        out.append(utf8);
    }
    out.append('\0');
    return out;
}

MdnsService::Kind kindOf(const QString &serviceType) {
    if (serviceType.compare(kConnectService, Qt::CaseInsensitive) == 0) return MdnsService::Kind::Connect;
    if (serviceType.compare(kPairingService, Qt::CaseInsensitive) == 0) return MdnsService::Kind::Pairing;
    return MdnsService::Kind::Other;
}

// "adb-x._adb-tls-connect._tcp.local" → the kind of "_adb-tls-connect._tcp.local".
MdnsService::Kind kindOfInstance(const QString &instance) {
    const qsizetype dot = instance.indexOf(u'.');
    return dot < 0 ? MdnsService::Kind::Other : kindOf(instance.mid(dot + 1));
}

QString firstLabel(const QString &name) {
    const qsizetype dot = name.indexOf(u'.');
    return dot < 0 ? name : name.left(dot);
}

} // namespace

std::optional<Message> decode(QByteArrayView packet) {
    if (packet.size() < kHeaderSize) return std::nullopt;
    Message message;
    message.response = (u16(packet, 2) & 0x8000) != 0;
    const int questions = u16(packet, 4);
    const int records = u16(packet, 6) + u16(packet, 8) + u16(packet, 10);
    qsizetype at = kHeaderSize;

    for (int i = 0; i < questions; ++i) {
        QString name;
        if (!readName(packet, at, name)) return std::nullopt;
        at += 4;                                        // type and class
        if (at > packet.size()) return std::nullopt;
        message.questions.append(name);
    }
    for (int i = 0; i < records; ++i) {
        Record record;
        if (!readName(packet, at, record.name)) return std::nullopt;
        if (at + 10 > packet.size()) return std::nullopt;
        record.type = u16(packet, at);
        const quint16 recordClass = u16(packet, at + 2) & kClassMask;
        record.ttl = u32(packet, at + 4);
        const qsizetype length = u16(packet, at + 8);
        at += 10;
        if (at + length > packet.size()) return std::nullopt;
        const qsizetype dataStart = at;
        at += length;
        if (recordClass != 1) continue;                 // not IN: nothing we know

        qsizetype cursor = dataStart;
        switch (RecordType(record.type)) {
        case RecordType::Ptr:
            if (!readName(packet, cursor, record.target)) continue;
            break;
        case RecordType::Srv:
            if (length < 7) continue;
            record.port = u16(packet, dataStart + 4);
            cursor = dataStart + 6;
            if (!readName(packet, cursor, record.target)) continue;
            break;
        case RecordType::A:
            if (length != 4) continue;
            record.target = QHostAddress(u32(packet, dataStart)).toString();
            break;
        case RecordType::Aaaa:
            if (length != 16) continue;
            record.target = QHostAddress(reinterpret_cast<const quint8 *>(packet.data() + dataStart)).toString();
            break;
        default:
            continue;                                   // TXT and the rest carry nothing we use
        }
        message.records.append(record);
    }
    return message;
}

QByteArray query(const QStringList &names) {
    QByteArray packet(kHeaderSize, '\0');
    qToBigEndian<quint16>(quint16(names.size()), packet.data() + 4);
    for (const QString &name : names) {
        packet.append(labels(name));
        char typeAndClass[4] = {0, char(RecordType::Ptr), 0, 1};
        packet.append(typeAndClass, 4);
    }
    return packet;
}

ServiceTable::Instance &ServiceTable::instance(const QString &name, MdnsService::Kind kind) {
    Instance &instance = instances_[name.toLower()];
    if (instance.name.isEmpty()) instance.name = name;
    instance.kind = kind;
    return instance;
}

void ServiceTable::absorb(const Message &message, const QHostAddress &sender, const QDateTime &now) {
    for (const Record &record : message.records) {
        const QDateTime expires = now.addSecs(std::min<qint64>(record.ttl, kMaxTtlSeconds));
        switch (RecordType(record.type)) {
        case RecordType::Ptr: {
            const MdnsService::Kind kind = kindOf(record.name);
            if (kind == MdnsService::Kind::Other) break;
            if (record.ttl == 0) {
                instances_.remove(record.target.toLower());
                break;
            }
            Instance &found = instance(record.target, kind);
            found.sender = sender;
            found.expires = expires;
            break;
        }
        case RecordType::Srv: {
            const MdnsService::Kind kind = kindOfInstance(record.name);
            if (kind == MdnsService::Kind::Other) break;
            if (record.ttl == 0) {
                instances_.remove(record.name.toLower());
                break;
            }
            Instance &found = instance(record.name, kind);
            found.host = record.target.toLower();
            found.port = record.port;
            found.sender = sender;
            found.expires = expires;
            break;
        }
        case RecordType::A:
            if (record.ttl == 0) ipv4_.remove(record.name.toLower());
            else ipv4_.insert(record.name.toLower(), QHostAddress(record.target));
            break;
        case RecordType::Aaaa:
            if (record.ttl == 0) ipv6_.remove(record.name.toLower());
            else ipv6_.insert(record.name.toLower(), QHostAddress(record.target));
            break;
        default:
            break;
        }
    }
}

QList<MdnsService> ServiceTable::services(const QDateTime &now) const {
    QList<MdnsService> services;
    for (const Instance &instance : instances_) {
        if (instance.port == 0 || instance.expires <= now) continue;
        QHostAddress address = ipv4_.value(instance.host);
        if (address.isNull()) address = instance.sender;
        if (address.isNull()) address = ipv6_.value(instance.host);
        if (address.isNull()) continue;
        MdnsService service;
        service.name = firstLabel(instance.name);
        service.kind = instance.kind;
        service.host = address.toString();
        service.port = instance.port;
        services.append(service);
    }
    std::sort(services.begin(), services.end(), [](const MdnsService &a, const MdnsService &b) {
        return std::tie(a.name, a.kind, a.port) < std::tie(b.name, b.kind, b.port);
    });
    return services;
}

} // namespace nexa::mdns
