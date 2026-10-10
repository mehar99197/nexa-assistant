#pragma once

#include "core/Result.h"

#include <QDBusContext>
#include <QDBusMessage>
#include <QObject>
#include <QStringList>

#include <functional>

namespace nexa {

class JobManager;
class PolkitAuthorizer;
class UinputDevice;

// What org.nexa.Helper actually does. The adaptor generated from proto/org.nexa.Helper.xml calls
// these slots, so the service cannot drift from the published contract.
//
// Every privileged method answers late: it takes the caller's unique bus name from the message,
// asks polkit (which may put a password prompt on screen and take as long as the user does), and
// only then does the work and replies. A refusal comes back as org.nexa.Helper.Error.Denied.
class HelperService : public QObject, protected QDBusContext {
    Q_OBJECT
public:
    HelperService(PolkitAuthorizer *authorizer, JobManager *jobs, UinputDevice *input, QObject *parent = nullptr);

public slots:
    void Ping();
    QString Version();
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
    // Asks polkit for `action`, then runs `work` and replies with whatever it returns.
    void whenAllowed(QLatin1StringView action, std::function<Result<QVariantList>()> work);

    PolkitAuthorizer *authorizer_;
    JobManager *jobs_;
    UinputDevice *input_;
};

} // namespace nexa
