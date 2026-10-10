// system/android/MdnsPacket — the wire format of a phone's wireless-debugging advertisement, read
// the way the phone writes it (compression pointers and all), and the table that turns what was
// heard into addresses to connect to.

#include "system/android/MdnsPacket.h"

#include "QtStringMakers.h"

#include <QTimeZone>
#include <QtEndian>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QByteArray be16(quint16 value) {
    QByteArray out(2, '\0');
    qToBigEndian(value, out.data());
    return out;
}

QByteArray be32(quint32 value) {
    QByteArray out(4, '\0');
    qToBigEndian(value, out.data());
    return out;
}

QByteArray dnsName(const QString &name) {
    QByteArray out;
    for (const QString &label : name.split(u'.', Qt::SkipEmptyParts)) {
        out.append(char(label.size()));
        out.append(label.toUtf8());
    }
    out.append('\0');
    return out;
}

QByteArray pointer(quint16 offset) {
    return be16(0xC000 | offset);
}

QByteArray record(const QByteArray &name, quint16 type, quint32 ttl, const QByteArray &rdata, quint16 cls = 0x8001) {
    return name + be16(type) + be16(cls) + be32(ttl) + be16(quint16(rdata.size())) + rdata;
}

QByteArray response(const QList<QByteArray> &answers, const QList<QByteArray> &additional = {}) {
    QByteArray packet = be16(0) + be16(0x8400) + be16(0) + be16(quint16(answers.size())) + be16(0)
                        + be16(quint16(additional.size()));
    for (const QByteArray &answer : answers) packet += answer;
    for (const QByteArray &extra : additional) packet += extra;
    return packet;
}

const QDateTime kNow = QDateTime(QDate(2026, 10, 11), QTime(1, 0, 0), QTimeZone::UTC);

// What an Android phone sends when wireless debugging goes on: the PTR in the answers, the SRV,
// TXT and addresses in the additional section, every repeated name a pointer back.
QByteArray announcement() {
    // Offsets: the PTR owner "_adb-tls-connect._tcp.local" is at 12; the instance name is in the
    // PTR's rdata at 12 + 29 + 10 = 51 ("adb-R58M12345ABC-AbCdEf" + pointer to 12).
    const QByteArray type = dnsName(u"_adb-tls-connect._tcp.local"_s);
    REQUIRE(type.size() == 29);
    const QByteArray instance = QByteArray("\x17" "adb-R58M12345ABC-AbCdEf") + pointer(12);
    const QByteArray ptr = record(type, 12, 4500, instance);
    const QByteArray srv = record(pointer(51), 33, 120, be16(0) + be16(0) + be16(37135) + dnsName(u"Android.local"_s));
    const QByteArray txt = record(pointer(51), 16, 4500, QByteArray("\x07" "v=1.0.0"));
    const QByteArray a = record(dnsName(u"Android.local"_s), 1, 120, QByteArray("\xC0\xA8\x00\x2A", 4));
    QByteArray six(16, '\0');
    six[0] = char(0xFE);
    six[1] = char(0x80);
    six[15] = 1;
    const QByteArray aaaa = record(dnsName(u"Android.local"_s), 28, 120, six);
    return response({ptr}, {srv, txt, a, aaaa});
}

} // namespace

TEST_CASE("A phone's announcement is read whole, pointers and all") {
    const std::optional<mdns::Message> message = mdns::decode(announcement());
    REQUIRE(message);
    CHECK(message->response);
    CHECK(message->questions.isEmpty());
    REQUIRE(message->records.size() == 4);               // TXT carries nothing we use
    CHECK(message->records.at(0).type == quint16(mdns::RecordType::Ptr));
    CHECK(message->records.at(0).name == u"_adb-tls-connect._tcp.local"_s);
    CHECK(message->records.at(0).target == u"adb-R58M12345ABC-AbCdEf._adb-tls-connect._tcp.local"_s);
    CHECK(message->records.at(0).ttl == 4500);
    CHECK(message->records.at(1).type == quint16(mdns::RecordType::Srv));
    CHECK(message->records.at(1).name == u"adb-R58M12345ABC-AbCdEf._adb-tls-connect._tcp.local"_s);
    CHECK(message->records.at(1).target == u"Android.local"_s);
    CHECK(message->records.at(1).port == 37135);
    CHECK(message->records.at(2).type == quint16(mdns::RecordType::A));
    CHECK(message->records.at(2).target == u"192.168.0.42"_s);
    CHECK(message->records.at(3).type == quint16(mdns::RecordType::Aaaa));
    CHECK(message->records.at(3).target == u"fe80::1"_s);

    mdns::ServiceTable table;
    table.absorb(*message, QHostAddress(u"192.168.0.42"_s), kNow);
    const QList<MdnsService> services = table.services(kNow);
    REQUIRE(services.size() == 1);
    CHECK(services.first().name == u"adb-R58M12345ABC-AbCdEf"_s);     // the serial keeps its case
    CHECK(services.first().kind == MdnsService::Kind::Connect);
    CHECK(services.first().address() == u"192.168.0.42:37135"_s);
    // Gone after its TTL, and at once after a goodbye.
    CHECK(table.services(kNow.addSecs(121)).isEmpty());
    table.absorb(*message, QHostAddress(u"192.168.0.42"_s), kNow.addSecs(60));
    CHECK(table.services(kNow.addSecs(121)).size() == 1);
    const QByteArray goodbye = response({record(dnsName(u"_adb-tls-connect._tcp.local"_s), 12, 0,
                                                dnsName(u"ADB-R58M12345ABC-AbCdEf._adb-tls-connect._tcp.local"_s))});
    table.absorb(*mdns::decode(goodbye), QHostAddress(u"192.168.0.42"_s), kNow.addSecs(61));
    CHECK(table.services(kNow.addSecs(62)).isEmpty());
}

TEST_CASE("Without an A record the sender's address serves, and IPv6 comes last") {
    const QByteArray instance = dnsName(u"adb-6affecc0-Qwerty._adb-tls-pairing._tcp.local"_s);
    const QByteArray srvOnly = response({record(instance, 33, 120, be16(0) + be16(0) + be16(41247) + dnsName(u"phone.local"_s))});
    mdns::ServiceTable table;
    table.absorb(*mdns::decode(srvOnly), QHostAddress(u"192.168.0.77"_s), kNow);
    QList<MdnsService> services = table.services(kNow);
    REQUIRE(services.size() == 1);
    CHECK(services.first().kind == MdnsService::Kind::Pairing);
    CHECK(services.first().name == u"adb-6affecc0-Qwerty"_s);
    CHECK(services.first().address() == u"192.168.0.77:41247"_s);

    // Heard from nowhere in particular, with only an IPv6 address for the host.
    QByteArray six(16, '\0');
    six[0] = char(0xFE);
    six[1] = char(0x80);
    six[15] = 7;
    mdns::ServiceTable quiet;
    quiet.absorb(*mdns::decode(srvOnly), QHostAddress(), kNow);
    CHECK(quiet.services(kNow).isEmpty());                   // no address at all yet
    quiet.absorb(*mdns::decode(response({record(dnsName(u"PHONE.local"_s), 28, 120, six)})), QHostAddress(), kNow);
    services = quiet.services(kNow);
    REQUIRE(services.size() == 1);
    CHECK(services.first().address() == u"[fe80::7]:41247"_s);
    // An A record for the same host, in any case, wins over both.
    quiet.absorb(*mdns::decode(response({record(dnsName(u"phone.LOCAL"_s), 1, 120, QByteArray("\x0A\x00\x00\x05", 4))})),
                 QHostAddress(), kNow);
    CHECK(quiet.services(kNow).first().address() == u"10.0.0.5:41247"_s);
}

TEST_CASE("Other services, other classes and broken packets are left alone") {
    const QByteArray printer = response({record(dnsName(u"_ipp._tcp.local"_s), 12, 4500, dnsName(u"Printer._ipp._tcp.local"_s)),
                                         record(dnsName(u"Printer._ipp._tcp.local"_s), 33, 120,
                                                be16(0) + be16(0) + be16(631) + dnsName(u"printer.local"_s))});
    mdns::ServiceTable table;
    table.absorb(*mdns::decode(printer), QHostAddress(u"192.168.0.9"_s), kNow);
    CHECK(table.services(kNow).isEmpty());
    CHECK(table.isEmpty());

    // Not class IN: skipped, the rest still read.
    const QByteArray chaos = response({record(dnsName(u"_adb-tls-connect._tcp.local"_s), 12, 4500,
                                              dnsName(u"x._adb-tls-connect._tcp.local"_s), 3),
                                       record(dnsName(u"host.local"_s), 1, 120, QByteArray("\x0A\x00\x00\x01", 4))});
    const auto decoded = mdns::decode(chaos);
    REQUIRE(decoded);
    REQUIRE(decoded->records.size() == 1);
    CHECK(decoded->records.first().type == quint16(mdns::RecordType::A));

    CHECK_FALSE(mdns::decode(QByteArray()));
    CHECK_FALSE(mdns::decode(QByteArray(11, '\0')));
    CHECK_FALSE(mdns::decode(announcement().left(60)));                         // cut inside a record
    // A pointer that points forward would loop; refused.
    const QByteArray forward = response({record(pointer(40), 12, 100, dnsName(u"a.local"_s))});
    CHECK_FALSE(mdns::decode(forward));
    // An A record of the wrong length is skipped, not misread.
    const QByteArray shortA = response({record(dnsName(u"host.local"_s), 1, 120, QByteArray("\x0A\x00", 2))});
    CHECK(mdns::decode(shortA)->records.isEmpty());
}

TEST_CASE("The question asks for both of adb's service types") {
    const QByteArray packet = mdns::query({mdns::kConnectService, mdns::kPairingService});
    const auto decoded = mdns::decode(packet);
    REQUIRE(decoded);
    CHECK_FALSE(decoded->response);
    CHECK(decoded->questions == QStringList{u"_adb-tls-connect._tcp.local"_s, u"_adb-tls-pairing._tcp.local"_s});
    CHECK(decoded->records.isEmpty());
}
