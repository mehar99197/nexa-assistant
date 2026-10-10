#include "helper/HelperService.h"

#include "core/Logging.h"
#include "helper/JobManager.h"
#include "helper/PolkitAuthorizer.h"
#include "helper/UinputDevice.h"
#include "proto/HelperTypes.h"
#include "proto/KeyMap.h"

#include <QDBusConnection>

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

} // namespace

HelperService::HelperService(PolkitAuthorizer *authorizer, JobManager *jobs, UinputDevice *input, QObject *parent)
    : QObject(parent), authorizer_(authorizer), jobs_(jobs), input_(input) {
    connect(jobs_, &JobManager::outputChunk, this, &HelperService::OutputChunk);
    connect(jobs_, &JobManager::finished, this, &HelperService::Finished);
}

void HelperService::whenAllowed(QLatin1StringView action, std::function<Result<QVariantList>()> work) {
    emit called();
    const QDBusMessage request = message();
    const QDBusConnection bus = connection();
    // The subject is the caller's unique bus name as the bus reports it, not anything the caller
    // put in the message.
    const QString caller = request.service();
    setDelayedReply(true);

    authorizer_->check(action, caller, [this, request, bus, work](bool allowed, const QString &reason) mutable {
        if (!allowed) {
            bus.send(request.createErrorReply(helper::kErrorDenied, reason));
            return;
        }
        const Result<QVariantList> result = work();
        if (!result) {
            qCWarning(lcApp).noquote() << "failed:" << result.error().message;
            bus.send(request.createErrorReply(errorNameFor(result.error().code), result.error().message));
            return;
        }
        bus.send(request.createReply(result.value()));
    });
}

void HelperService::Ping() {
    emit called();      // not privileged: it only says the helper is running
}

QString HelperService::Version() {
    emit called();
    return QString::fromLatin1(NEXA_VERSION);
}

qulonglong HelperService::RunCommand(const QStringList &argv, const QString &cwd, const QStringList &env,
                                     int timeoutMs) {
    whenAllowed(helper::kActionRunCommand, [this, argv, cwd, env, timeoutMs]() -> Result<QVariantList> {
        const Result<quint64> job = jobs_->start(argv, cwd, env, timeoutMs);
        if (!job) return job.error();
        return QVariantList{QVariant::fromValue(qulonglong(job.value()))};
    });
    return 0;           // the real reply is sent once polkit has answered
}

void HelperService::CancelJob(qulonglong jobId) {
    whenAllowed(helper::kActionRunCommand, [this, jobId]() -> Result<QVariantList> {
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
    whenAllowed(helper::kActionInjectInput, [this, utf8]() -> Result<QVariantList> {
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
    whenAllowed(helper::kActionInjectInput, [this, keycodes]() -> Result<QVariantList> {
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
    whenAllowed(helper::kActionInjectInput, [this, dx, dy]() -> Result<QVariantList> {
        if (const Result<void> moved = input_->moveBy(dx, dy); !moved) return moved.error();
        return QVariantList{};
    });
}

void HelperService::MouseMoveAbs(int x, int y, int screenW, int screenH) {
    whenAllowed(helper::kActionInjectInput, [this, x, y, screenW, screenH]() -> Result<QVariantList> {
        if (screenW < 2 || screenH < 2) {
            return Error{ErrorCode::InvalidArgument,
                         u"The screen size %1x%2 cannot be right."_s.arg(screenW).arg(screenH)};
        }
        if (const Result<void> moved = input_->moveTo(x, y, screenW, screenH); !moved) return moved.error();
        return QVariantList{};
    });
}

void HelperService::MouseButton(uint button, bool down) {
    whenAllowed(helper::kActionInjectInput, [this, button, down]() -> Result<QVariantList> {
        const Result<int> code = buttonCode(button);
        if (!code) return code.error();
        if (const Result<void> clicked = input_->button(code.value(), down); !clicked) return clicked.error();
        return QVariantList{};
    });
}

void HelperService::MouseScroll(int dx, int dy) {
    whenAllowed(helper::kActionInjectInput, [this, dx, dy]() -> Result<QVariantList> {
        if (const Result<void> scrolled = input_->scroll(dx, dy); !scrolled) return scrolled.error();
        return QVariantList{};
    });
}

} // namespace nexa
