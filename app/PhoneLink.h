#pragma once

#include "core/Result.h"
#include "system/android/AdbParsers.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

namespace nexa {

class AndroidDevice;
class PairingManager;
class RemoteServer;

// Pairs Nexa Phone on the phone connected through adb without a finger on either side (Mediator
// between AndroidDevice, PairingManager and RemoteServer): starts the ordinary pairing code on
// the PC, makes sure the phone's localhost:7777 leads to the web remote, and starts the app on the
// phone with the address and the code in its launch intent — which only the app named
// org.nexa.phone receives. The adb link is the authority: the user accepted this PC's key on the
// phone. The code stays single-use and short-lived; all that changes is who carries it.
class PhoneLink : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<QString>)>;      // a sentence: what happened
    static constexpr QLatin1StringView kPackage{"org.nexa.phone"};
    static constexpr int kPairTimeoutMs = 90'000;
    // Locked, the phone shows the app only once the user unlocks it: wait most of the code's life.
    static constexpr int kLockedPairTimeoutMs = 240'000;

    PhoneLink(AndroidDevice *phone, PairingManager *pairing, RemoteServer *remote, QObject *parent = nullptr);

    // Progress comes through progress(); `done` runs exactly once.
    void pair(Done done);
    bool isPairing() const { return pending_ != nullptr; }

signals:
    void progress(const QString &message);

private:
    void findPhone();
    void resolveApp();
    void checkScreen(const QString &activity);
    void launchApp(const QString &activity, bool asleep);
    void awaitPairing(const QString &code);
    void finish(Result<QString> result);

    AndroidDevice *phone_;
    PairingManager *pairing_;
    RemoteServer *remote_;
    Done pending_;
    AdbDevice device_;
    bool locked_{false};
    QMetaObject::Connection clientWatch_;
    QMetaObject::Connection codeWatch_;
    QTimer timer_;
};

} // namespace nexa
