// system/android/AdbDiscovery — the pure parts: which address the remembered phone should be
// reconnected through, and adb's list folded together with the browser's.

#include "system/android/AdbDiscovery.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

MdnsService service(const QString &name, MdnsService::Kind kind, const QString &host, int port) {
    MdnsService service;
    service.name = name;
    service.kind = kind;
    service.host = host;
    service.port = port;
    return service;
}

AdbDevice device(const QString &serial, const QString &state = u"device"_s) {
    AdbDevice device;
    device.serial = serial;
    device.state = state;
    return device;
}

} // namespace

TEST_CASE("The remembered phone is reconnected through the address it advertises, unless it is on already") {
    const QList<MdnsService> services{
        service(u"adb-R58M12345ABC-AbCdEf"_s, MdnsService::Kind::Pairing, u"192.168.0.42"_s, 41247),
        service(u"adb-R58M12345ABC-AbCdEf"_s, MdnsService::Kind::Connect, u"192.168.0.42"_s, 37135),
        service(u"adb-OTHER-Qwerty"_s, MdnsService::Kind::Connect, u"192.168.0.43"_s, 37000),
    };
    CHECK(AdbDiscovery::reconnectAddress(u"R58M12345ABC"_s, {}, services) == u"192.168.0.42:37135"_s);
    CHECK(AdbDiscovery::reconnectAddress(u"NOBODY"_s, {}, services).isEmpty());
    CHECK(AdbDiscovery::reconnectAddress({}, {}, services).isEmpty());
    // Already connected: over USB (by serial), over Wi-Fi (by mDNS name or by the address itself).
    CHECK(AdbDiscovery::reconnectAddress(u"R58M12345ABC"_s, {device(u"R58M12345ABC"_s)}, services).isEmpty());
    CHECK(AdbDiscovery::reconnectAddress(u"R58M12345ABC"_s, {device(u"adb-R58M12345ABC-AbCdEf._adb-tls-connect._tcp"_s)}, services).isEmpty());
    CHECK(AdbDiscovery::reconnectAddress(u"R58M12345ABC"_s, {device(u"192.168.0.42:37135"_s)}, services).isEmpty());
    // Listed but not ready (unauthorized, offline): still worth connecting.
    CHECK(AdbDiscovery::reconnectAddress(u"R58M12345ABC"_s, {device(u"R58M12345ABC"_s, u"offline"_s)}, services)
          == u"192.168.0.42:37135"_s);
}

TEST_CASE("adb's services and the browser's are one list without doubles") {
    const QList<MdnsService> fromAdb{service(u"adb-A-x"_s, MdnsService::Kind::Connect, u"192.168.0.42"_s, 37135)};
    const QList<MdnsService> fromBrowser{
        service(u"adb-A-x"_s, MdnsService::Kind::Connect, u"192.168.0.42"_s, 37135),     // the same one
        service(u"adb-A-x"_s, MdnsService::Kind::Pairing, u"192.168.0.42"_s, 41247),     // another kind
        service(u"adb-B-y"_s, MdnsService::Kind::Connect, u"192.168.0.43"_s, 37135),     // another phone
    };
    const QList<MdnsService> merged = AdbDiscovery::merged(fromAdb, fromBrowser);
    REQUIRE(merged.size() == 3);
    CHECK(merged.at(0).name == u"adb-A-x"_s);
    CHECK(merged.at(1).kind == MdnsService::Kind::Pairing);
    CHECK(merged.at(2).name == u"adb-B-y"_s);
    CHECK(AdbDiscovery::merged({}, {}).isEmpty());
    CHECK(AdbDiscovery::merged({}, fromBrowser).size() == 3);
}
