#include "core/Logging.h"
#include "helper/DeviceLock.h"
#include "helper/HelperService.h"
#include "helper/JobManager.h"
#include "helper/LoginSessions.h"
#include "helper/PolkitAuthorizer.h"
#include "helper/SessionGrants.h"
#include "helper/UinputDevice.h"
#include "proto/HelperTypes.h"
#include "proto/KeyMap.h"

#include "helperadaptor.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QTimer>

#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace nexa;

namespace {

// Nothing to do for this long and the helper exits; systemd and D-Bus start it again on the next
// call, so root-owned code is not sitting around when it is not needed.
constexpr int kIdleExitMs = 10 * 60 * 1000;

} // namespace

// nexa-helper: the small root-privileged part of Nexa. It runs on the system bus, authorises the
// caller's login against polkit — once per login, not once per caller — and does three things on
// request: run a command as root, send keyboard and pointer events through /dev/uinput, and
// switch the laptop's own keyboards and pointers off for hardcore mode. It reads no input.
//
// `nexa-helper --release-input` gives back whatever a lock left switched off and exits: the unit
// runs it after every stop, crashes included, so the laptop is never left locked.
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"nexa-helper"_s);
    QCoreApplication::setApplicationVersion(QString::fromLatin1(NEXA_VERSION));

    if (::geteuid() != 0) {
        qCCritical(lcApp) << "nexa-helper must run as root; it is started by D-Bus activation, not by hand.";
        return 1;
    }
    if (QCoreApplication::arguments().contains(u"--release-input"_s)) {
        const int released = DeviceLock::releaseRecorded();
        if (released > 0) qCInfo(lcApp) << "gave back" << released << "locked input device(s)";
        return 0;
    }
    // A helper that died locked, with no ExecStopPost to undo it (started by hand, an old unit file).
    if (const int released = DeviceLock::releaseRecorded(); released > 0) {
        qCWarning(lcApp) << "gave back" << released << "input device(s) a previous helper left locked";
    }
    qDBusRegisterMetaType<QList<uint>>();       // PressKeys' argument

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCCritical(lcApp) << "No D-Bus system bus.";
        return 1;
    }

    PolkitAuthorizer authorizer;
    LoginSessions sessions(bus);
    SessionGrants grants;       // reloaded from /run, so an idle exit does not lose the password
    JobManager jobs;
    UinputDevice input;
    DeviceLock deviceLock;
    HelperService service(&authorizer, &sessions, &grants, &jobs, &input, &deviceLock);
    new OrgNexaHelperAdaptor(&service);         // generated from proto/org.nexa.Helper.xml

    if (!bus.registerObject(helper::kPath, &service)) {
        qCCritical(lcApp) << "Cannot publish" << helper::kPath << ":" << bus.lastError().message();
        return 1;
    }
    // Last, so no call can arrive before the object is there.
    if (!bus.registerService(helper::kService)) {
        qCCritical(lcApp) << "Cannot take the name" << helper::kService << ":" << bus.lastError().message()
                          << "- is another nexa-helper running, or is packaging/org.nexa.Helper.conf missing?";
        return 1;
    }

    QTimer idle;
    idle.setInterval(kIdleExitMs);
    idle.setSingleShot(true);
    QObject::connect(&idle, &QTimer::timeout, &app, [&jobs, &idle, &service] {
        if (!jobs.isIdle() || service.isInputLocked()) {
            idle.start();       // a long command is still running, or the keyboard and mouse are held
            return;
        }
        qCInfo(lcApp) << "idle, exiting";
        QCoreApplication::quit();
    });
    QObject::connect(&service, &HelperService::called, &idle, [&idle] { idle.start(); });
    idle.start();

    qCInfo(lcApp) << "nexa-helper" << QCoreApplication::applicationVersion() << "ready on" << helper::kService
                  << "with" << keymap::allCodes().size() << "keys";
    return app.exec();
}
