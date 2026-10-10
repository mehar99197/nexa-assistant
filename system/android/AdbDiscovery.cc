#include "system/android/AdbDiscovery.h"

#include "core/Logging.h"
#include "system/android/AdbClient.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kPollTimeoutMs = 5'000;
constexpr int kWatchedIntervalMs = 2'500;
constexpr int kBackgroundIntervalMs = 15'000;

bool sameDevices(const QList<AdbDevice> &a, const QList<AdbDevice> &b) {
    return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(), [](const AdbDevice &x, const AdbDevice &y) {
        return x.serial == y.serial && x.state == y.state;
    });
}

bool sameServices(const QList<MdnsService> &a, const QList<MdnsService> &b) {
    return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(), [](const MdnsService &x, const MdnsService &y) {
        return x.name == y.name && x.kind == y.kind && x.address() == y.address();
    });
}

} // namespace

AdbDiscovery::AdbDiscovery(AdbClient *adb, QObject *parent) : QObject(parent), adb_(adb) {
    connect(&timer_, &QTimer::timeout, this, &AdbDiscovery::poll);
}

void AdbDiscovery::setWatched(bool watched) {
    watched_ = watched;
    reschedule();
}

void AdbDiscovery::setBackground(bool enabled) {
    background_ = enabled;
    reschedule();
}

void AdbDiscovery::reschedule() {
    const int interval = watched_ ? kWatchedIntervalMs : background_ ? kBackgroundIntervalMs : 0;
    if (interval == 0) {
        timer_.stop();
        return;
    }
    if (timer_.isActive() && timer_.interval() == interval) return;
    timer_.start(interval);
    poll();
}

void AdbDiscovery::refresh() {
    poll();
}

void AdbDiscovery::setAutoConnect(const QString &hardwareSerial) {
    autoConnect_ = hardwareSerial.trimmed();
    lastAttempt_.clear();
}

QString AdbDiscovery::reconnectAddress(const QString &hardwareSerial, const QList<AdbDevice> &devices,
                                       const QList<MdnsService> &services) {
    if (hardwareSerial.isEmpty()) return {};
    QStringList addresses;
    for (const MdnsService &service : services) {
        if (service.kind == MdnsService::Kind::Connect && adb::serialFromServiceName(service.name) == hardwareSerial) {
            addresses.append(service.address());
        }
    }
    if (addresses.isEmpty()) return {};
    // Connected already, by serial, by mDNS name, or by the very address it advertises?
    for (const AdbDevice &device : devices) {
        if (!device.isReady()) continue;
        if (device.serial == hardwareSerial || adb::serialFromServiceName(device.serial) == hardwareSerial
            || addresses.contains(device.serial)) {
            return {};
        }
    }
    return addresses.first();
}

void AdbDiscovery::poll() {
    if (polling_) return;
    polling_ = true;
    adb_->run({u"devices"_s, u"-l"_s}, kPollTimeoutMs, [this](Result<AdbClient::Output> devicesOutput) {
        QList<AdbDevice> devices = devicesOutput ? adb::parseDevices(devicesOutput.value().text()) : devices_;
        adb_->run({u"mdns"_s, u"services"_s}, kPollTimeoutMs,
                  [this, devices = std::move(devices)](Result<AdbClient::Output> servicesOutput) mutable {
            QList<MdnsService> services =
                servicesOutput ? adb::parseMdnsServices(servicesOutput.value().text()) : QList<MdnsService>{};
            finishPoll(std::move(devices), std::move(services));
        });
    });
}

void AdbDiscovery::finishPoll(QList<AdbDevice> devices, QList<MdnsService> services) {
    polling_ = false;
    const bool different = !sameDevices(devices, devices_) || !sameServices(services, services_);
    devices_ = std::move(devices);
    services_ = std::move(services);
    if (different) emit changed();

    const QString address = reconnectAddress(autoConnect_, devices_, services_);
    if (address.isEmpty() || address == lastAttempt_) return;
    lastAttempt_ = address;
    qCInfo(lcApp).noquote() << "reconnecting to the phone at" << address;
    emit reconnecting(address);
    adb_->run({u"connect"_s, address}, kPollTimeoutMs * 2, [this](Result<AdbClient::Output>) { refresh(); });
}

} // namespace nexa
