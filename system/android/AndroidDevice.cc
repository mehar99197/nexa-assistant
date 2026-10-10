#include "system/android/AndroidDevice.h"

#include "core/Logging.h"
#include "system/android/AdbShell.h"

#include <QImage>
#include <QPointer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kListTimeoutMs = 5'000;
constexpr int kShellTimeoutMs = 15'000;
constexpr int kScreenshotTimeoutMs = 20'000;
constexpr int kTransferTimeoutMs = 300'000;
constexpr int kConnectTimeoutMs = 20'000;
constexpr qint64 kDevicesFreshMs = 3'000;
constexpr qint64 kAppsFreshMs = 10 * 60'000;
constexpr qint64 kContactsFreshMs = 5 * 60'000;
// What the phone opens for the web remote. Reversed at connect to the remote's loopback port, so
// the page arrives over the adb link — a secure context for the microphone, no certificate to accept.
constexpr int kPhoneRemotePort = 7777;

const QString kNoPhone = u"No phone is connected. Pair it in Settings → Android (on the phone: Developer options → "
                          "Wireless debugging), or plug it in with USB debugging on."_s;

QString said(const AdbClient::Output &output) {
    return (output.text() + u'\n' + output.errorText()).trimmed();
}

} // namespace

QString AndroidDevice::Info::summary() const {
    QStringList parts;
    const QString name = (manufacturer.isEmpty() || model.startsWith(manufacturer, Qt::CaseInsensitive))
                             ? model
                             : manufacturer + u' ' + model;
    parts.append(name.isEmpty() ? serial : name);
    if (!release.isEmpty()) parts.append(u"Android %1"_s.arg(release));
    if (battery >= 0) parts.append(u"battery %1%"_s.arg(battery));
    if (screen.isValid()) parts.append(u"screen %1x%2"_s.arg(screen.width()).arg(screen.height()));
    parts.append(wireless ? u"over Wi-Fi (%1)"_s.arg(serial) : u"over USB"_s);
    return u"Connected phone: "_s + parts.join(u", "_s) + u'.';
}

AndroidDevice::AndroidDevice(AdbClient *adb, QObject *parent) : QObject(parent), adb_(adb) {
    decoder_.setMaxThreadCount(1);
}

AndroidDevice::~AndroidDevice() {
    decoder_.waitForDone();
}

void AndroidDevice::setPreferred(const QString &hardwareSerial) {
    preferred_ = hardwareSerial.trimmed();
    forgetDevices();
}

void AndroidDevice::forgetDevices() {
    devicesAge_.invalidate();
}

void AndroidDevice::withDevice(std::function<void(Result<AdbDevice>)> done) {
    if (devicesAge_.isValid() && devicesAge_.elapsed() < kDevicesFreshMs) {
        chooseFrom(devices_, std::move(done));
        return;
    }
    adb_->run({u"devices"_s, u"-l"_s}, kListTimeoutMs, [this, done](Result<AdbClient::Output> output) {
        if (!output) {
            done(output.error());
            return;
        }
        devices_ = adb::parseDevices(output.value().text());
        devicesAge_.start();
        chooseFrom(devices_, done);
    });
}

void AndroidDevice::chooseFrom(const QList<AdbDevice> &devices, std::function<void(Result<AdbDevice>)> done) {
    QList<AdbDevice> ready;
    bool unauthorized = false;
    QStringList waiting;
    for (const AdbDevice &device : devices) {
        if (device.isReady()) {
            ready.append(device);
            continue;
        }
        unauthorized = unauthorized || device.state == u"unauthorized";
        waiting.append(u"%1 (%2)"_s.arg(device.displayModel(), device.state));
    }
    if (ready.isEmpty()) {
        adopt({});
        if (unauthorized) {
            done(Error{ErrorCode::PermissionDenied,
                       u"The phone has not allowed this computer yet: unlock it and accept the debugging prompt."_s});
        } else if (!waiting.isEmpty()) {
            done(Error{ErrorCode::NotSupported, u"The phone is not ready: %1."_s.arg(waiting.join(u", "_s))});
        } else {
            done(Error{ErrorCode::NotSupported, kNoPhone});
        }
        return;
    }

    const AdbDevice *chosen = ready.size() == 1 ? &ready.first() : nullptr;
    for (const AdbDevice &device : ready) {
        if (chosen) break;
        const bool isPreferred = !preferred_.isEmpty()
                                 && (device.serial == preferred_ || adb::serialFromServiceName(device.serial) == preferred_);
        if (isPreferred || device.serial == currentSerial_) chosen = &device;
    }
    if (!chosen) {
        QStringList names;
        for (const AdbDevice &device : ready) names.append(u"%1 (%2)"_s.arg(device.displayModel(), device.serial));
        done(Error{ErrorCode::InvalidArgument, u"Several phones are connected — %1. Choose one in Settings → Android."_s
                                                   .arg(names.join(u", "_s))});
        return;
    }
    const AdbDevice device = *chosen;
    adopt(device);
    done(device);
}

void AndroidDevice::adopt(const AdbDevice &device) {
    if (device.serial == currentSerial_) return;
    currentSerial_ = device.serial;
    // What was learnt about the previous phone says nothing about this one.
    apps_.clear();
    appsAge_.invalidate();
    contacts_.clear();
    contactsAge_.invalidate();
    screen_ = {};
    lastScreenSize_ = {};
    lastScale_ = 1.0;
    description_ = device.serial.isEmpty()
                       ? QString()
                       : u"%1 %2"_s.arg(device.displayModel(), device.isWireless() ? u"over Wi-Fi"_s : u"over USB"_s);
    qCInfo(lcApp).noquote() << "phone:" << (description_.isEmpty() ? u"none"_s : description_);
    emit deviceChanged(description_);
    if (!device.serial.isEmpty()) applyReverse(device.serial);
}

void AndroidDevice::setReversePort(int pcPort) {
    if (pcPort == reversePort_) return;
    reversePort_ = pcPort;
    if (!currentSerial_.isEmpty()) applyReverse(currentSerial_);
}

void AndroidDevice::applyReverse(const QString &serial) {
    adb_->runOn(serial, {u"reverse"_s, u"tcp:%1"_s.arg(kPhoneRemotePort), u"tcp:%1"_s.arg(reversePort_)},
                kListTimeoutMs, [](Result<AdbClient::Output> output) {
        if (!output || output.value().exitCode != 0) qCDebug(lcApp) << "adb reverse failed";
    });
}

void AndroidDevice::info(std::function<void(Result<Info>)> done) {
    // One round trip for everything: each value on its own line.
    const QString command = u"echo manufacturer=$(getprop ro.product.manufacturer); echo model=$(getprop ro.product.model); "
                            "echo release=$(getprop ro.build.version.release); echo hardware=$(getprop ro.serialno); "
                            "dumpsys battery | grep -m1 ' level:'; wm size"_s;
    withDevice([this, command, done](Result<AdbDevice> device) {
        if (!device) {
            done(device.error());
            return;
        }
        shell(command, kShellTimeoutMs, [this, device = device.value(), done](Result<AdbClient::Output> output) {
            if (!output) {
                done(output.error());
                return;
            }
            const QString text = output.value().text();
            const QHash<QString, QString> properties = adb::parseProperties(text);
            Info info;
            info.serial = device.serial;
            info.wireless = device.isWireless();
            info.manufacturer = properties.value(u"manufacturer"_s);
            // getprop prints an empty value rather than nothing, so "missing" means "empty" here.
            info.model = properties.value(u"model"_s);
            if (info.model.isEmpty()) info.model = device.displayModel();
            info.release = properties.value(u"release"_s);
            info.hardwareSerial = properties.value(u"hardware"_s);
            info.battery = adb::parseBatteryLevel(text).value_or(-1);
            info.screen = adb::parseWmSize(text).value_or(QSize());
            if (info.screen.isValid()) screen_ = info.screen;
            done(info);
        });
    });
}

void AndroidDevice::shell(const QString &command, int timeoutMs,
                          std::function<void(Result<AdbClient::Output>)> done) {
    withDevice([this, command, timeoutMs, done](Result<AdbDevice> device) {
        if (!device) {
            done(device.error());
            return;
        }
        adb_->runOn(device.value().serial, {u"shell"_s, command}, timeoutMs,
                    [this, done](Result<AdbClient::Output> output) {
            if (!output) {
                done(output.error());
                return;
            }
            if (const std::optional<Error> error = AdbClient::errorFrom(output.value())) {
                forgetDevices();        // the phone may have gone away
                done(*error);
                return;
            }
            done(std::move(output));
        });
    });
}

void AndroidDevice::act(const QString &command, const QString &success, Done done) {
    shell(command, kShellTimeoutMs, [success, done](Result<AdbClient::Output> output) {
        if (!output) {
            done(output.error());
            return;
        }
        const QString words = said(output.value());
        // `am start` reports a failure as "Error: …" and still exits 0.
        if (output.value().exitCode != 0 || words.contains(u"Error:"_s) || words.contains(u"Exception"_s)) {
            done(Error{ErrorCode::Io, words.isEmpty() ? u"The phone refused that (exit code %1)."_s.arg(output.value().exitCode)
                                                      : u"The phone said: %1"_s.arg(words.left(600))});
            return;
        }
        done(success);
    });
}

void AndroidDevice::launchableApps(std::function<void(Result<QStringList>)> done) {
    if (appsAge_.isValid() && appsAge_.elapsed() < kAppsFreshMs && !apps_.isEmpty()) {
        done(apps_);
        return;
    }
    // `cmd package` talks to the package service directly; `pm` would start a Java process first.
    shell(u"cmd package query-activities --components -a android.intent.action.MAIN -c android.intent.category.LAUNCHER"_s,
          kShellTimeoutMs, [this, done](Result<AdbClient::Output> output) {
        if (!output) {
            done(output.error());
            return;
        }
        const QStringList components = adb::parseLauncherComponents(output.value().text());
        if (components.isEmpty()) {
            done(Error{ErrorCode::NotSupported,
                       u"The phone did not list its apps (Android 7 or newer is needed): %1"_s.arg(said(output.value()).left(300))});
            return;
        }
        apps_ = components;
        appsAge_.start();
        done(components);
    });
}

void AndroidDevice::openApp(const QString &name, Done done) {
    launchableApps([this, name, done](Result<QStringList> apps) {
        if (!apps) {
            done(apps.error());
            return;
        }
        const std::optional<phonelookup::AppTarget> target = phonelookup::matchApp(name, apps.value());
        if (!target) {
            const QStringList close = phonelookup::closestApps(name, apps.value());
            // NotFound: the fast path hands this to the model, which can look through android_list_apps.
            done(Error{ErrorCode::NotFound, u"No app on the phone matches \"%1\"%2"_s.arg(
                                                name, close.isEmpty() ? QString() : u". Closest: "_s + close.join(u", "_s))});
            return;
        }
        const QString command = target->component.isEmpty() ? u"am start "_s + target->intentArguments
                                                            : u"am start -n "_s + adbshell::quote(target->component);
        act(command, u"Opened %1 on the phone."_s.arg(target->label), done);
    });
}

void AndroidDevice::screenSize(std::function<void(Result<QSize>)> done) {
    if (screen_.isValid()) {
        done(screen_);
        return;
    }
    shell(u"wm size"_s, kShellTimeoutMs, [this, done](Result<AdbClient::Output> output) {
        if (!output) {
            done(output.error());
            return;
        }
        const std::optional<QSize> size = adb::parseWmSize(output.value().text());
        if (!size) {
            done(Error{ErrorCode::Io, u"The phone did not say how big its screen is."_s});
            return;
        }
        screen_ = *size;
        done(*size);
    });
}

void AndroidDevice::screenshot(std::function<void(Result<Shot>)> done) {
    withDevice([this, done](Result<AdbDevice> device) {
        if (!device) {
            done(device.error());
            return;
        }
        // exec-out, not shell: the PNG must arrive byte for byte.
        adb_->runOn(device.value().serial, {u"exec-out"_s, u"screencap"_s, u"-p"_s}, kScreenshotTimeoutMs,
                    [this, done](Result<AdbClient::Output> output) {
            if (!output) {
                done(output.error());
                return;
            }
            if (const std::optional<Error> error = AdbClient::errorFrom(output.value())) {
                done(*error);
                return;
            }
            const QByteArray png = output.value().standardOutput;
            const QPointer<AndroidDevice> self(this);
            decoder_.start([self, png, done] {
                const QImage image = QImage::fromData(png, "PNG");
                Result<Shot> shot = image.isNull()
                                        ? Result<Shot>(Error{ErrorCode::Io, u"The phone sent no readable screenshot."_s})
                                        : Result<Shot>(ScreenCapture::encode(image));
                QMetaObject::invokeMethod(self, [self, shot = std::move(shot), done]() mutable {
                    if (!self) return;
                    if (shot) {
                        self->lastScreenSize_ = shot.value().realSize;
                        self->lastScale_ = shot.value().scale;
                        self->screen_ = shot.value().realSize;
                    }
                    done(std::move(shot));
                }, Qt::QueuedConnection);
            });
        });
    });
}

void AndroidDevice::contacts(std::function<void(Result<QList<phonelookup::Contact>>)> done) {
    if (contactsAge_.isValid() && contactsAge_.elapsed() < kContactsFreshMs) {
        done(contacts_);
        return;
    }
    shell(u"content query --uri content://com.android.contacts/data/phones --projection display_name:data1"_s,
          kShellTimeoutMs, [this, done](Result<AdbClient::Output> output) {
        if (!output) {
            done(output.error());
            return;
        }
        const QString text = output.value().text();
        if (text.contains(u"Permission Denial"_s) || text.contains(u"SecurityException"_s)) {
            done(Error{ErrorCode::PermissionDenied, u"The phone does not let adb read its contacts."_s});
            return;
        }
        QList<phonelookup::Contact> list;
        for (const QStringList &row : adb::parseContentRows(text, {u"display_name"_s, u"data1"_s})) {
            list.append({row.value(0), row.value(1)});
        }
        contacts_ = list;
        contactsAge_.start();
        done(list);
    });
}

void AndroidDevice::resolveNumber(const QString &numberOrName, std::function<void(Result<phonelookup::Contact>)> done) {
    if (const QString number = adbshell::phoneNumber(numberOrName); !number.isEmpty()) {
        done(phonelookup::Contact{number, number});
        return;
    }
    contacts([numberOrName, done](Result<QList<phonelookup::Contact>> list) {
        if (!list) {
            done(Error{list.error().code, u"\"%1\" is not a phone number, and the contacts could not be read: %2"_s.arg(
                                              numberOrName, list.error().message)});
            return;
        }
        const std::optional<phonelookup::Contact> found = phonelookup::matchContact(numberOrName, list.value());
        if (!found) {
            done(Error{ErrorCode::NotFound, u"No single contact matches \"%1\"."_s.arg(numberOrName)});
            return;
        }
        const QString number = adbshell::phoneNumber(found->number);
        if (number.isEmpty()) {
            done(Error{ErrorCode::InvalidArgument, u"%1's number (%2) cannot be dialled."_s.arg(found->name, found->number)});
            return;
        }
        done(phonelookup::Contact{found->name, number});
    });
}

void AndroidDevice::push(const QString &localPath, const QString &remotePath, Done done) {
    withDevice([this, localPath, remotePath, done](Result<AdbDevice> device) {
        if (!device) {
            done(device.error());
            return;
        }
        adb_->runOn(device.value().serial, {u"push"_s, localPath, remotePath}, kTransferTimeoutMs,
                    [localPath, remotePath, done](Result<AdbClient::Output> output) {
            if (!output) {
                done(output.error());
                return;
            }
            if (output.value().exitCode != 0) {
                done(Error{ErrorCode::Io, u"Copying to the phone failed: %1"_s.arg(said(output.value()).left(400))});
                return;
            }
            done(u"Copied %1 to %2 on the phone. %3"_s.arg(localPath, remotePath, output.value().text().section(u'\n', -1)));
        });
    });
}

void AndroidDevice::pull(const QString &remotePath, const QString &localPath, Done done) {
    withDevice([this, localPath, remotePath, done](Result<AdbDevice> device) {
        if (!device) {
            done(device.error());
            return;
        }
        adb_->runOn(device.value().serial, {u"pull"_s, remotePath, localPath}, kTransferTimeoutMs,
                    [localPath, remotePath, done](Result<AdbClient::Output> output) {
            if (!output) {
                done(output.error());
                return;
            }
            if (output.value().exitCode != 0) {
                done(Error{ErrorCode::Io, u"Copying from the phone failed: %1"_s.arg(said(output.value()).left(400))});
                return;
            }
            done(u"Copied %1 from the phone to %2."_s.arg(remotePath, localPath));
        });
    });
}

void AndroidDevice::install(const QString &apkPath, Done done) {
    withDevice([this, apkPath, done](Result<AdbDevice> device) {
        if (!device) {
            done(device.error());
            return;
        }
        adb_->runOn(device.value().serial, {u"install"_s, u"-r"_s, apkPath}, kTransferTimeoutMs,
                    [this, apkPath, done](Result<AdbClient::Output> output) {
            if (!output) {
                done(output.error());
                return;
            }
            const QString words = said(output.value());
            if (output.value().exitCode != 0 || !words.contains(u"Success"_s)) {
                done(Error{ErrorCode::Io, u"Installing %1 failed: %2"_s.arg(apkPath, words.left(400))});
                return;
            }
            appsAge_.invalidate();      // there is a new app to open
            done(u"Installed %1 on the phone."_s.arg(apkPath));
        });
    });
}

void AndroidDevice::pair(const QString &address, const QString &code, Done done) {
    adb_->run({u"pair"_s, address, code}, kConnectTimeoutMs, [address, done](Result<AdbClient::Output> output) {
        if (!output) {
            done(output.error());
            return;
        }
        const QString words = said(output.value());
        if (!words.contains(u"Successfully paired"_s)) {
            done(Error{ErrorCode::PermissionDenied, u"Pairing with %1 failed: %2"_s.arg(address, words.left(300))});
            return;
        }
        done(u"Paired with %1."_s.arg(address));
    });
}

void AndroidDevice::connectTo(const QString &address, Done done) {
    adb_->run({u"connect"_s, address}, kConnectTimeoutMs, [this, address, done](Result<AdbClient::Output> output) {
        if (!output) {
            done(output.error());
            return;
        }
        forgetDevices();
        // adb exits 0 even when it could not connect, so the words decide.
        const QString words = said(output.value());
        if (!words.contains(u"connected to"_s) || words.contains(u"failed"_s) || words.contains(u"cannot"_s)) {
            done(Error{ErrorCode::Io, u"Could not connect to %1: %2"_s.arg(address, words.left(300))});
            return;
        }
        done(u"Connected to %1."_s.arg(address));
    });
}

void AndroidDevice::disconnectAll(Done done) {
    adb_->run({u"disconnect"_s}, kConnectTimeoutMs, [this, done](Result<AdbClient::Output> output) {
        forgetDevices();
        adopt({});
        if (!output) {
            done(output.error());
            return;
        }
        done(u"Disconnected every phone on Wi-Fi."_s);
    });
}

void AndroidDevice::cancelAll() {
    adb_->cancelAll();
}

} // namespace nexa
