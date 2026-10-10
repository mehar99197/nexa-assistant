#include "helper/HelperService.h"

#include "core/Logging.h"
#include "helper/JobManager.h"
#include "helper/LoginSessions.h"
#include "helper/PolkitAuthorizer.h"
#include "helper/SessionGrants.h"
#include "helper/UinputDevice.h"
#include "proto/HelperTypes.h"
#include "proto/KeyMap.h"

#include <QDBusConnectionInterface>
#include <QDBusReply>

#include <linux/input-event-codes.h>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// polkit denial has its own name so the client can tell "you said no" from "it went wrong".
QLatin1StringView errorNameFor(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidArgument:
        return helper::kErrorInvalidArgument;
    case ErrorCode::PermissionDenied:
        return helper::kErrorNoInputDevice;     // only /dev/uinput reports this here
    default:
        return helper::kErrorFailed;
    }
}

Result<int> buttonCode(uint button) {
    switch (button) {
    case helper::ButtonLeft:
        return BTN_LEFT;
    case helper::ButtonRight:
        return BTN_RIGHT;
    case helper::ButtonMiddle:
        return BTN_MIDDLE;
    default:
        return Error{ErrorCode::InvalidArgument, u"Button %1 does not exist; use 1, 2 or 3."_s.arg(button)};
    }
}

// One login, named so calls can queue behind the prompt that is already on screen.
QString loginKey(const LoginSession &session) {
    return u"%1:%2"_s.arg(session.uid).arg(session.id);
}

void sendResult(const QDBusConnection &bus, const QDBusMessage &request, const Result<QVariantList> &result) {
    if (!result) {
        qCWarning(lcApp).noquote() << "failed:" << result.error().message;
        bus.send(request.createErrorReply(errorNameFor(result.error().code), result.error().message));
        return;
    }
    bus.send(request.createReply(result.value()));
}

} // namespace

HelperService::HelperService(PolkitAuthorizer *authorizer, LoginSessions *sessions, SessionGrants *grants,
                             JobManager *jobs, UinputDevice *input, QObject *parent)
    : QObject(parent), authorizer_(authorizer), sessions_(sessions), grants_(grants), jobs_(jobs), input_(input) {
    connect(jobs_, &JobManager::outputChunk, this, &HelperService::OutputChunk);
    connect(jobs_, &JobManager::finished, this, &HelperService::Finished);
}

LoginSession HelperService::callerSession() const {
    QDBusConnectionInterface *daemon = connection().interface();
    if (daemon == nullptr) return {};
    // The bus learned the caller's user id from the kernel when it connected. Nothing in the
    // message can change it, which is what makes this safe to authorise against.
    const QDBusReply<uint> uid = daemon->serviceUid(message().service());
    if (!uid.isValid()) return {};
    return sessions_->displaySessionOf(uid.value());
}

void HelperService::whenAllowed(std::function<Result<QVariantList>()> work) {
    emit called();
    const QDBusMessage request = message();
    const QDBusConnection bus = connection();
    const QString caller = request.service();
    setDelayedReply(true);

    const LoginSession session = callerSession();
    if (!session.isValid()) {
        bus.send(request.createErrorReply(
            helper::kErrorDenied,
            tr("No graphical login was found for the caller, so there is nothing to authorise against.")));
        return;
    }
    if (!session.isAtTheScreen()) {
        bus.send(request.createErrorReply(
            helper::kErrorDenied, tr("Only the login session at this computer's own screen may do this.")));
        return;
    }

    if (grants_->has(session)) {
        qCDebug(lcApp).noquote() << "login session" << session.id << "is already authorised";
        sendResult(bus, request, work());
        return;
    }

    // One prompt, however many calls arrive while it is on screen. Without this, a handful of
    // quick actions in a row would each put up their own password prompt.
    const QString key = loginKey(session);
    const bool promptIsUp = waiting_.contains(key);
    waiting_[key].push_back(Pending{request, bus, std::move(work)});
    if (promptIsUp) return;

    authorizer_->check(helper::kActionControl, caller, [this, session, key](bool allowed, const QString &reason) {
        // Remembered against the login, so the next Nexa process is not a stranger to it.
        if (allowed) grants_->add(session);
        const std::vector<Pending> pending = waiting_.take(key);
        for (const Pending &call : pending) {
            if (!allowed) {
                call.bus.send(call.request.createErrorReply(helper::kErrorDenied, reason));
                continue;
            }
            sendResult(call.bus, call.request, call.work());
        }
    });
}

void HelperService::Ping() {
    emit called();      // not privileged: it only says the helper is running
}

QString HelperService::Version() {
    emit called();
    return QString::fromLatin1(NEXA_VERSION);
}

bool HelperService::IsAuthorized() {
    emit called();
    // Not privileged: it says only whether this login has already answered the prompt, which is
    // what the caller would find out by making a call anyway.
    const LoginSession session = callerSession();
    return session.isAtTheScreen() && grants_->has(session);
}

void HelperService::ForgetAuthorization() {
    emit called();
    // Giving up an authorisation needs no authorisation — and it has to work even from a session
    // that is no longer the one on the screen, so only the login itself has to make sense here.
    const LoginSession session = callerSession();
    if (!session.isValid()) return;
    const bool dropped = grants_->forget(session);
    // polkit may still be holding its own short-lived grant for this caller; without clearing that
    // too, "forget" would look as though it had not worked.
    authorizer_->revoke(message().service());
    if (dropped) {
        qCInfo(lcApp).noquote() << "authorisation for login session" << session.id << "given up on request";
    }
}

qulonglong HelperService::RunCommand(const QStringList &argv, const QString &cwd, const QStringList &env,
                                     int timeoutMs) {
    whenAllowed([this, argv, cwd, env, timeoutMs]() -> Result<QVariantList> {
        const Result<quint64> job = jobs_->start(argv, cwd, env, timeoutMs);
        if (!job) return job.error();
        return QVariantList{QVariant::fromValue(qulonglong(job.value()))};
    });
    return 0;           // the real reply is sent once polkit has answered
}

void HelperService::CancelJob(qulonglong jobId) {
    whenAllowed([this, jobId]() -> Result<QVariantList> {
        const Result<void> cancelled = jobs_->cancel(jobId);
        if (!cancelled) {
            // A job that is already gone deserves its own name, not a generic failure.
            return Error{cancelled.error().code == ErrorCode::NotFound ? ErrorCode::InvalidArgument
                                                                       : cancelled.error().code,
                         cancelled.error().message};
        }
        return QVariantList{};
    });
}

QString HelperService::TypeText(const QString &utf8) {
    whenAllowed([this, utf8]() -> Result<QVariantList> {
        if (utf8.isEmpty()) return Error{ErrorCode::InvalidArgument, u"There is no text to type."_s};
        const TypePlan plan = keymap::forText(utf8);
        if (plan.strokes.isEmpty()) {
            return Error{ErrorCode::InvalidArgument,
                         u"None of that text can be typed on a US layout: %1"_s.arg(plan.skipped)};
        }
        if (const Result<void> typed = input_->type(plan.strokes); !typed) return typed.error();
        return QVariantList{plan.skipped};
    });
    return {};
}

void HelperService::PressKeys(const QList<uint> &keycodes) {
    whenAllowed([this, keycodes]() -> Result<QVariantList> {
        if (keycodes.isEmpty()) return Error{ErrorCode::InvalidArgument, u"No keys were given."_s};
        if (keycodes.size() > 8) return Error{ErrorCode::InvalidArgument, u"That is too many keys at once."_s};
        QList<int> codes;
        for (const uint code : keycodes) {
            if (code == 0 || code > KEY_MAX) {
                return Error{ErrorCode::InvalidArgument, u"%1 is not a key code."_s.arg(code)};
            }
            codes.append(int(code));
        }
        if (const Result<void> pressed = input_->pressChord(codes); !pressed) return pressed.error();
        return QVariantList{};
    });
}

void HelperService::MouseMove(int dx, int dy) {
    whenAllowed([this, dx, dy]() -> Result<QVariantList> {
        if (const Result<void> moved = input_->moveBy(dx, dy); !moved) return moved.error();
        return QVariantList{};
    });
}

void HelperService::MouseMoveAbs(int x, int y, int screenW, int screenH) {
    whenAllowed([this, x, y, screenW, screenH]() -> Result<QVariantList> {
        if (screenW < 2 || screenH < 2) {
            return Error{ErrorCode::InvalidArgument,
                         u"The screen size %1x%2 cannot be right."_s.arg(screenW).arg(screenH)};
        }
        if (const Result<void> moved = input_->moveTo(x, y, screenW, screenH); !moved) return moved.error();
        return QVariantList{};
    });
}

void HelperService::MouseButton(uint button, bool down) {
    whenAllowed([this, button, down]() -> Result<QVariantList> {
        const Result<int> code = buttonCode(button);
        if (!code) return code.error();
        if (const Result<void> clicked = input_->button(code.value(), down); !clicked) return clicked.error();
        return QVariantList{};
    });
}

void HelperService::MouseScroll(int dx, int dy) {
    whenAllowed([this, dx, dy]() -> Result<QVariantList> {
        if (const Result<void> scrolled = input_->scroll(dx, dy); !scrolled) return scrolled.error();
        return QVariantList{};
    });
}

} // namespace nexa
