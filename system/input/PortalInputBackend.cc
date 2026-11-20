#include "system/input/PortalInputBackend.h"

#include "proto/KeyMap.h"
#include "system/input/RemoteDesktopSession.h"

#include <QPoint>
#include <QSize>

#include "proto/LinuxKeyCodes.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// The session answers with a bare success; the tools want a note, even an empty one.
RemoteDesktopSession::Done finished(IInputBackend::Done done) {
    return [done = std::move(done)](Result<void> result) {
        if (!result) {
            done(result.error());
            return;
        }
        done(QString());
    };
}

} // namespace

PortalInputBackend::PortalInputBackend(RemoteDesktopSession *session, QObject *parent)
    : IInputBackend(parent), session_(session) {}

QString PortalInputBackend::name() const {
    return u"desktop remote-control portal"_s;
}

QString PortalInputBackend::reach() const {
    return tr("every window — the desktop injects the events itself");
}

int PortalInputBackend::evdevButton(int button) {
    switch (button) {
    case 2:
        return BTN_RIGHT;
    case 3:
        return BTN_MIDDLE;
    default:
        return BTN_LEFT;
    }
}

std::pair<int, int> PortalInputBackend::portalScrollSteps(int dx, int dy) {
    return {-dy, dx};
}

PortalInputBackend::Step PortalInputBackend::keyStep(int code, bool down) const {
    return [session = session_, code, down](std::function<void(Result<void>)> next) {
        session->keyboardKey(code, down, std::move(next));
    };
}

PortalInputBackend::Step PortalInputBackend::buttonStep(int code, bool down) const {
    return [session = session_, code, down](std::function<void(Result<void>)> next) {
        session->pointerButton(code, down, std::move(next));
    };
}

void PortalInputBackend::run(std::vector<Step> steps, Done done, QString note) {
    runFrom(std::make_shared<const std::vector<Step>>(std::move(steps)), 0, std::move(done), std::move(note));
}

void PortalInputBackend::runFrom(std::shared_ptr<const std::vector<Step>> steps, std::size_t index, Done done,
                                 QString note) {
    if (index >= steps->size()) {
        done(note);
        return;
    }
    (*steps)[index]([this, steps, index, done, note](Result<void> result) {
        if (!result) {
            done(result.error());
            return;
        }
        runFrom(steps, index + 1, done, note);
    });
}

void PortalInputBackend::typeText(const QString &text, Done done) {
    const TypePlan plan = keymap::forText(text);
    if (plan.strokes.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, tr("None of that text can be typed on a US layout: %1").arg(plan.skipped)});
        return;
    }
    std::vector<Step> steps;
    steps.reserve(std::size_t(plan.strokes.size()) * 2 + 2);
    bool shiftHeld = false;
    for (const KeyStroke &stroke : plan.strokes) {
        if (stroke.shift != shiftHeld) {
            steps.push_back(keyStep(KEY_LEFTSHIFT, stroke.shift));
            shiftHeld = stroke.shift;
        }
        steps.push_back(keyStep(stroke.code, true));
        steps.push_back(keyStep(stroke.code, false));
    }
    if (shiftHeld) steps.push_back(keyStep(KEY_LEFTSHIFT, false));
    const QString note = plan.skipped.isEmpty()
        ? QString()
        : tr("These characters are not on a US keyboard and were left out: %1").arg(plan.skipped);
    run(std::move(steps), std::move(done), note);
}

void PortalInputBackend::pressKeys(const QList<int> &keyCodes, Done done) {
    if (keyCodes.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, tr("No keys were given.")});
        return;
    }
    std::vector<Step> steps;
    steps.reserve(std::size_t(keyCodes.size()) * 2);
    for (const int code : keyCodes) steps.push_back(keyStep(code, true));
    // Released in reverse, so the modifiers that opened the chord are the last to go.
    for (auto code = keyCodes.crbegin(); code != keyCodes.crend(); ++code) steps.push_back(keyStep(*code, false));
    run(std::move(steps), std::move(done));
}

void PortalInputBackend::mouseMove(int dx, int dy, Done done) {
    session_->pointerBy(dx, dy, finished(std::move(done)));
}

void PortalInputBackend::mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) {
    session_->pointerTo(QPoint(x, y), QSize(screenWidth, screenHeight), finished(std::move(done)));
}

void PortalInputBackend::mouseClick(int button, int count, Done done) {
    const int code = evdevButton(button);
    std::vector<Step> steps;
    for (int i = 0; i < qBound(1, count, 3); ++i) {
        steps.push_back(buttonStep(code, true));
        steps.push_back(buttonStep(code, false));
    }
    run(std::move(steps), std::move(done));
}

void PortalInputBackend::mouseScroll(int dx, int dy, Done done) {
    const auto [vertical, horizontal] = portalScrollSteps(dx, dy);
    session_->pointerScroll(vertical, horizontal, finished(std::move(done)));
}

} // namespace nexa
