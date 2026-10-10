#include "app/PhoneLink.h"

#include "core/Logging.h"
#include "remote/PairingManager.h"
#include "remote/RemoteServer.h"
#include "system/android/AdbShell.h"
#include "system/android/AndroidDevice.h"

#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

PhoneLink::PhoneLink(AndroidDevice *phone, PairingManager *pairing, RemoteServer *remote, QObject *parent)
    : QObject(parent), phone_(phone), pairing_(pairing), remote_(remote) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] {
        finish(Error{ErrorCode::Timeout,
                     locked_ ? tr("The phone stayed locked, so Nexa Phone could not start: unlock it and pair again.")
                             : tr("The phone did not pair within %1 s. Nexa Phone on the phone says why; or pair it "
                                  "there by hand with the code Settings → Remote shows.")
                                   .arg(kPairTimeoutMs / 1000)});
    });
}

void PhoneLink::pair(Done done) {
    if (pending_) {
        done(Error{ErrorCode::InvalidArgument, tr("The phone is being paired already.")});
        return;
    }
    pending_ = std::move(done);
    if (remote_->isRunning()) {
        findPhone();
        return;
    }
    // Just switched on, the server needs a moment to listen.
    emit progress(tr("Waiting for the web remote to start…"));
    QTimer::singleShot(2000, this, [this] {
        if (!remote_->isRunning()) {
            finish(Error{ErrorCode::NotSupported,
                         tr("The web remote is off. Switch it on in Settings → Remote, then pair the phone again.")});
            return;
        }
        findPhone();
    });
}

void PhoneLink::findPhone() {
    emit progress(tr("Looking for the phone over adb…"));
    phone_->withDevice([this](Result<AdbDevice> device) {
        if (!device) {
            finish(device.error());
            return;
        }
        device_ = device.value();
        // localhost:7777 on the phone → the web remote's loopback port here, through adb.
        phone_->reverse([this](Result<QString> reversed) {
            if (!reversed) {
                finish(reversed.error());
                return;
            }
            resolveApp();
        });
    });
}

void PhoneLink::resolveApp() {
    phone_->shell(u"cmd package resolve-activity --brief %1"_s.arg(kPackage), 10'000,
                  [this](Result<AdbClient::Output> output) {
        if (!output) {
            finish(output.error());
            return;
        }
        const QString activity = output.value().text().section(u'\n', -1).trimmed();
        if (!activity.startsWith(QString(kPackage) + u'/')) {
            finish(Error{ErrorCode::NotFound,
                         tr("Nexa Phone is not installed on the phone (%1). scripts/build-phone-apk.sh builds it and "
                            "`adb install -r build-android/nexa-phone.apk` puts it there.")
                             .arg(device_.displayModel())});
            return;
        }
        checkScreen(activity);
    });
}

// An app started behind the lock screen is paused at once, and Qt for Android holds a paused app
// still — it would start only when the user unlocks. So: wake a sleeping screen, and say so.
void PhoneLink::checkScreen(const QString &activity) {
    phone_->shell(u"dumpsys power | grep -m1 'mWakefulness='; dumpsys trust | grep -m1 -o 'deviceLocked=[01]'"_s,
                  10'000, [this, activity](Result<AdbClient::Output> output) {
        const QString said = output ? output.value().text() : QString();
        const bool asleep = said.contains(u"Dozing"_s) || said.contains(u"Asleep"_s);
        locked_ = asleep || said.contains(u"deviceLocked=1"_s);
        launchApp(activity, asleep);
    });
}

void PhoneLink::launchApp(const QString &activity, bool asleep) {
    const QString code = pairing_->startPairing();
    const QString address = u"localhost:%1"_s.arg(RemoteServer::kPhonePort);
    const QString name = tr("Nexa app on %1").arg(device_.displayModel());
    // A fresh start, so the app reads the launch intent. Nothing waits for its first frame here:
    // behind a lock screen that frame comes only when the user unlocks.
    const QString command = u"%1am force-stop %2; am start -n %3 --es nexa.address %4 --es nexa.code %5 --es nexa.name %6"_s
                                .arg(asleep ? u"input keyevent KEYCODE_WAKEUP; "_s : QString(), kPackage,
                                     adbshell::quote(activity), adbshell::quote(address), adbshell::quote(code),
                                     adbshell::quote(name));
    qCInfo(lcApp).noquote() << "phone link: starting Nexa Phone on" << device_.displayModel()
                            << (locked_ ? "(locked) with a pairing code" : "with a pairing code");
    emit progress(locked_ ? tr("Nexa Phone is starting on the locked phone with code %1 — unlock the phone and it pairs "
                               "by itself.")
                                .arg(code)
                          : tr("Starting Nexa Phone on the phone with code %1…").arg(code));
    phone_->shell(command, 30'000, [this, code](Result<AdbClient::Output> output) {
        if (!output) {
            finish(output.error());
            return;
        }
        const QString said = (output.value().text() + u' ' + output.value().errorText()).simplified();
        if (said.contains(u"Error"_s)) {
            finish(Error{ErrorCode::Io, tr("The phone did not start Nexa Phone: %1").arg(said.left(200))});
            return;
        }
        awaitPairing(code);
    });
}

void PhoneLink::awaitPairing(const QString &code) {
    qCInfo(lcApp) << "phone link: Nexa Phone was started, waiting for it to pair";
    emit progress(locked_ ? tr("Nexa Phone is waiting on the locked phone: unlock it and it pairs by itself (code %1).")
                                .arg(code)
                          : tr("Nexa Phone is up on the phone; waiting for it to pair with code %1…").arg(code));
    const int before = remote_->clientCount();
    clientWatch_ = connect(remote_, &RemoteServer::clientsChanged, this, [this, before](int count) {
        if (count > before) finish(tr("Nexa Phone is paired and connected to this PC."));
    });
    codeWatch_ = connect(pairing_, &PairingManager::codeChanged, this, [this](const QString &now) {
        if (now.isEmpty()) emit progress(tr("The phone took the code; connecting…"));
    });
    timer_.start(locked_ ? kLockedPairTimeoutMs : kPairTimeoutMs);
}

void PhoneLink::finish(Result<QString> result) {
    disconnect(clientWatch_);
    disconnect(codeWatch_);
    timer_.stop();
    if (result) {
        qCInfo(lcApp).noquote() << "phone link:" << result.value();
    } else {
        qCWarning(lcApp).noquote() << "phone link:" << result.error().message;
    }
    if (Done done = std::exchange(pending_, nullptr)) done(std::move(result));
}

} // namespace nexa
