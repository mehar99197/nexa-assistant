// system/android/MdnsBrowser on a real multicast socket: a fake phone in the same group hears the
// browser's question and answers; the browser lists it, then forgets it on the goodbye.

#include "system/android/MdnsBrowser.h"
#include "system/android/MdnsPacket.h"

#include "QtStringMakers.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSignalSpy>
#include <QTest>
#include <QUdpSocket>
#include <QtEndian>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

const QHostAddress kGroup(QStringLiteral("224.0.0.251"));

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

QByteArray record(const QString &name, quint16 type, quint32 ttl, const QByteArray &rdata) {
    return dnsName(name) + be16(type) + be16(0x8001) + be32(ttl) + be16(quint16(rdata.size())) + rdata;
}

QByteArray response(const QList<QByteArray> &records) {
    QByteArray packet = be16(0) + be16(0x8400) + be16(0) + be16(quint16(records.size())) + be16(0) + be16(0);
    for (const QByteArray &one : records) packet += one;
    return packet;
}

QList<QNetworkInterface> multicastInterfaces() {
    QList<QNetworkInterface> found;
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags flags = interface.flags();
        if ((flags & QNetworkInterface::IsUp) && (flags & QNetworkInterface::IsRunning)
            && (flags & QNetworkInterface::CanMulticast) && !(flags & QNetworkInterface::IsLoopBack)) {
            found.append(interface);
        }
    }
    return found;
}

} // namespace

TEST_CASE("The browser asks, hears a phone answer, and forgets it on the goodbye") {
    const QList<QNetworkInterface> interfaces = multicastInterfaces();
    if (interfaces.isEmpty()) SKIP("no network interface can multicast here");

    // The fake phone shares the port, as every mDNS responder does.
    QUdpSocket phone;
    REQUIRE(phone.bind(QHostAddress::AnyIPv4, MdnsBrowser::kPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint));
    for (const QNetworkInterface &interface : interfaces) phone.joinMulticastGroup(kGroup, interface);
    const QString instance = u"adb-R58M12345ABC-AbCdEf._adb-tls-connect._tcp.local"_s;
    const QByteArray announcement =
        response({record(u"_adb-tls-connect._tcp.local"_s, 12, 4500, dnsName(instance)),
                  record(instance, 33, 120, be16(0) + be16(0) + be16(37135) + dnsName(u"Android.local"_s)),
                  record(u"Android.local"_s, 1, 120, QByteArray("\xC0\xA8\x00\x2A", 4))});
    int questions = 0;
    QObject::connect(&phone, &QUdpSocket::readyRead, [&] {
        while (phone.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = phone.receiveDatagram();
            const std::optional<mdns::Message> message = mdns::decode(datagram.data());
            if (!message || message->response || !message->questions.contains(mdns::kConnectService)) continue;
            ++questions;
            phone.writeDatagram(announcement, kGroup, MdnsBrowser::kPort);
        }
    });

    MdnsBrowser browser;
    CHECK_FALSE(browser.isActive());
    CHECK(browser.services().isEmpty());
    browser.setActive(true);
    REQUIRE(browser.isActive());
    CHECK(browser.problem().isEmpty());
    QSignalSpy changed(&browser, &MdnsBrowser::changed);
    browser.query();
    REQUIRE(QTest::qWaitFor([&questions] { return questions > 0; }, 5000));
    REQUIRE(QTest::qWaitFor([&browser] { return !browser.services().isEmpty(); }, 5000));
    CHECK(changed.count() == 1);
    const QList<MdnsService> services = browser.services();
    REQUIRE(services.size() == 1);
    CHECK(services.first().name == u"adb-R58M12345ABC-AbCdEf"_s);
    CHECK(services.first().kind == MdnsService::Kind::Connect);
    CHECK(services.first().address() == u"192.168.0.42:37135"_s);

    // The same announcement again changes nothing; the goodbye does.
    phone.writeDatagram(announcement, kGroup, MdnsBrowser::kPort);
    QTest::qWait(300);
    CHECK(changed.count() == 1);
    phone.writeDatagram(response({record(u"_adb-tls-connect._tcp.local"_s, 12, 0, dnsName(instance))}), kGroup,
                        MdnsBrowser::kPort);
    REQUIRE(QTest::qWaitFor([&browser] { return browser.services().isEmpty(); }, 5000));
    CHECK(changed.count() == 2);

    // Inactive, it asks nothing more (the question went out once per interface before, so the
    // count is whatever it is — it must only stop growing).
    browser.setActive(false);
    CHECK_FALSE(browser.isActive());
    const int asked = questions;
    browser.query();
    QTest::qWait(300);
    CHECK(questions == asked);
}
