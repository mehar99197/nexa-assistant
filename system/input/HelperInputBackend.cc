#include "system/input/HelperInputBackend.h"

#include "system/helper/HelperClient.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// A plain Result<void> callback that forwards to a Done with no note.
HelperClient::Done forward(IInputBackend::Done done) {
    return [done = std::move(done)](Result<void> result) {
        if (!result) {
            done(result.error());
            return;
        }
        done(QString());
    };
}

} // namespace

HelperInputBackend::HelperInputBackend(HelperClient *helper, QObject *parent)
    : IInputBackend(parent), helper_(helper) {}

QString HelperInputBackend::name() const {
    return u"root helper (uinput)"_s;
}

QString HelperInputBackend::reach() const {
    return tr("every window, Wayland and X11 alike");
}

void HelperInputBackend::typeText(const QString &text, Done done) {
    helper_->typeText(text, [done = std::move(done)](Result<QString> skipped) {
        if (!skipped) {
            done(skipped.error());
            return;
        }
        if (skipped.value().isEmpty()) {
            done(QString());
            return;
        }
        done(tr("These characters are not on a US keyboard and were left out: %1").arg(skipped.value()));
    });
}

void HelperInputBackend::pressKeys(const QList<int> &keyCodes, Done done) {
    helper_->pressKeys(keyCodes, forward(std::move(done)));
}

void HelperInputBackend::mouseMove(int dx, int dy, Done done) {
    helper_->mouseMove(dx, dy, forward(std::move(done)));
}

void HelperInputBackend::mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) {
    helper_->mouseMoveAbs(x, y, screenWidth, screenHeight, forward(std::move(done)));
}

void HelperInputBackend::mouseScroll(int dx, int dy, Done done) {
    helper_->mouseScroll(dx, dy, forward(std::move(done)));
}

void HelperInputBackend::mouseClick(int button, int count, Done done) {
    clickStep(button, qBound(1, count, 3), true, std::move(done));
}

void HelperInputBackend::clickStep(int button, int remaining, bool down, Done done) {
    helper_->mouseButton(button, down, [this, button, remaining, down, done](Result<void> result) mutable {
        if (!result) {
            done(result.error());
            return;
        }
        if (down) {
            clickStep(button, remaining, false, std::move(done));    // release what was just pressed
            return;
        }
        if (remaining > 1) {
            clickStep(button, remaining - 1, true, std::move(done));
            return;
        }
        done(QString());
    });
}

} // namespace nexa
