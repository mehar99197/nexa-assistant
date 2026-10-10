#pragma once

#include "core/Result.h"
#include "helper/LoginSession.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>
#include <vector>

namespace nexa {

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
class HelperService : public QObject, protected QDBusContext {
    Q_OBJECT
public:
    HelperService(PolkitAuthorizer *authorizer, LoginSessions *sessions, SessionGrants *grants, JobManager *jobs,
                  UinputDevice *input, QObject *parent = nullptr);

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

signals:
    void OutputChunk(qulonglong jobId, uchar stream, const QByteArray &data);
    void Finished(qulonglong jobId, int exitCode, const QString &status);
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

    PolkitAuthorizer *authorizer_;
    LoginSessions *sessions_;
    SessionGrants *grants_;
    JobManager *jobs_;
    UinputDevice *input_;
    // Calls that arrived while one password prompt was already on screen, by login. They are all
    // answered by that one prompt; otherwise a handful of quick actions means a handful of prompts.
    QHash<QString, std::vector<Pending>> waiting_;
};

} // namespace nexa
