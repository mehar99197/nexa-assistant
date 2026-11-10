#pragma once

#include "core/Result.h"
#include "helper/LoginSession.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <vector>

class QDBusServiceWatcher;
class QFileSystemWatcher;

namespace nexa {

class DeviceLock;
class JobManager;
class LoginSessions;
class PolkitAuthorizer;
class SessionGrants;
class UinputDevice;

// What org.nexa.Helper actually does. The adaptor generated from proto/org.nexa.Helper.xml calls
// these slots, so the service cannot drift from the published contract.
//
// Every privileged method answers late. The order is always the same: ask the bus which user is
// calling, ask logind which graphical login that user has and whether it is the one on the screen,
// and only then either act — because this login has already been authorised — or put polkit's
// password prompt on screen and wait as long as the user takes. A refusal comes back as
// org.nexa.Helper.Error.Denied.
//
// The password is asked for once per login, not once per caller: see SessionGrants for why that
// needs saying, and for what is and is not remembered.
//
// Hardcore mode's lock (LockInput) is held for one caller, and ends by itself — InputUnlocked says why — when that
// caller leaves the bus, misses its heartbeat (a LockInput every few seconds) for kHeartbeatMs, or its login stops
// being the one at the screen. A keyboard or pointer that appears meanwhile is locked as it arrives.
class HelperService : public QObject, protected QDBusContext {
    Q_OBJECT
public:
    static constexpr int kHeartbeatMs = 30'000;

    HelperService(PolkitAuthorizer *authorizer, LoginSessions *sessions, SessionGrants *grants, JobManager *jobs,
                  UinputDevice *input, DeviceLock *lock, QObject *parent = nullptr);
    ~HelperService() override;

    bool isInputLocked() const;

public slots:
    void Ping();
    QString Version();
    bool IsAuthorized();
    void ForgetAuthorization();
    qulonglong RunCommand(const QStringList &argv, const QString &cwd, const QStringList &env, int timeoutMs);
    void CancelJob(qulonglong jobId);
    QString TypeText(const QString &utf8);
    void PressKeys(const QList<uint> &keycodes);
    void MouseMove(int dx, int dy);
    void MouseMoveAbs(int x, int y, int screenW, int screenH);
    void MouseButton(uint button, bool down);
    void MouseScroll(int dx, int dy);
    QStringList LockInput();
    void UnlockInput();

signals:
    void OutputChunk(qulonglong jobId, uchar stream, const QByteArray &data);
    void Finished(qulonglong jobId, int exitCode, const QString &status);
    void InputUnlocked(const QString &reason);
    void called();      // any call arrived: the idle timer starts again

private:
    // A call held while the password prompt is on screen.
    struct Pending {
        QDBusMessage request;
        QDBusConnection bus;
        std::function<Result<QVariantList>()> work;
    };

    // Authorises the caller's login, then runs `work` and replies with whatever it returns.
    void whenAllowed(std::function<Result<QVariantList>()> work);
    // The caller's graphical login. The user id comes from the bus, so it cannot be faked; an
    // invalid result means there is nobody to ask.
    LoginSession callerSession() const;
    // The lock ends: the devices back, the watchers gone; `byItself` says so on the bus.
    void endLock(const QString &reason, bool byItself);
    void watchLockHolder(const QDBusConnection &bus, const QString &holder);

    PolkitAuthorizer *authorizer_;
    LoginSessions *sessions_;
    SessionGrants *grants_;
    JobManager *jobs_;
    UinputDevice *input_;
    DeviceLock *lock_;
    QTimer heartbeat_;                              // the holder's LockInput calls keep it from running out
    QDBusServiceWatcher *holderWatcher_{nullptr};   // the holder leaving the bus
    QFileSystemWatcher *hotplug_{nullptr};          // /dev/input: a device plugged in while locked
    // Calls that arrived while one password prompt was already on screen, by login. They are all
    // answered by that one prompt; otherwise a handful of quick actions means a handful of prompts.
    QHash<QString, std::vector<Pending>> waiting_;
};

} // namespace nexa
