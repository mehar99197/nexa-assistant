#include "system/input/XTestInputBackend.h"

#include "core/Logging.h"
#include "proto/KeyMap.h"

#include <QProcessEnvironment>

#include <linux/input-event-codes.h>

// X11's headers define macros such as None and Status, so they come last, after every Qt header.
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Linux evdev codes are what KeyMap produces; X keycodes are those plus 8 on Linux.
constexpr int kEvdevToX11 = 8;
constexpr unsigned int kX11LeftShift = KEY_LEFTSHIFT + kEvdevToX11;

// Our buttons are 1 left, 2 right, 3 middle; X11 numbers the middle button 2 and the right 3.
int toX11Button(int button) {
    switch (button) {
    case 2:
        return 3;
    case 3:
        return 2;
    default:
        return 1;
    }
}

} // namespace

struct XTestInputBackend::Display {
    ::Display *handle{nullptr};

    ~Display() {
        if (handle) XCloseDisplay(handle);
    }
};

XTestInputBackend::XTestInputBackend(QObject *parent) : IInputBackend(parent) {}

XTestInputBackend::~XTestInputBackend() = default;

bool XTestInputBackend::isWaylandSession() {
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    return environment.value(u"XDG_SESSION_TYPE"_s).compare(u"wayland"_s, Qt::CaseInsensitive) == 0
        || !environment.value(u"WAYLAND_DISPLAY"_s).isEmpty();
}

bool XTestInputBackend::isUsable(QString *reason) {
    ::Display *handle = XOpenDisplay(nullptr);
    if (!handle) {
        if (reason) *reason = tr("there is no X display to inject into");
        return false;
    }
    int major = 0;
    int minor = 0;
    int firstEvent = 0;
    int firstError = 0;
    const bool present = XTestQueryExtension(handle, &firstEvent, &firstError, &major, &minor);
    XCloseDisplay(handle);
    if (!present && reason) *reason = tr("the X server has no XTEST extension");
    return present;
}

QString XTestInputBackend::name() const {
    return u"XTEST"_s;
}

QString XTestInputBackend::reach() const {
    return isWaylandSession() ? tr("X11 windows only — not native Wayland windows") : tr("every window");
}

Result<XTestInputBackend::Display *> XTestInputBackend::display() {
    if (display_ && display_->handle) return display_.get();
    display_ = std::make_unique<Display>();
    display_->handle = XOpenDisplay(nullptr);
    if (!display_->handle) {
        display_.reset();
        return Error{ErrorCode::NotSupported, tr("Cannot open the X display.")};
    }
    return display_.get();
}

void XTestInputBackend::typeText(const QString &text, Done done) {
    const Result<Display *> open = display();
    if (!open) {
        done(open.error());
        return;
    }
    const TypePlan plan = keymap::forText(text);
    if (plan.strokes.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, tr("None of that text can be typed on a US layout.")});
        return;
    }
    ::Display *handle = open.value()->handle;
    bool shiftHeld = false;
    for (const KeyStroke &stroke : plan.strokes) {
        if (stroke.shift != shiftHeld) {
            XTestFakeKeyEvent(handle, kX11LeftShift, stroke.shift ? True : False, 0);
            shiftHeld = stroke.shift;
        }
        XTestFakeKeyEvent(handle, unsigned(stroke.code + kEvdevToX11), True, 0);
        XTestFakeKeyEvent(handle, unsigned(stroke.code + kEvdevToX11), False, 0);
    }
    if (shiftHeld) XTestFakeKeyEvent(handle, kX11LeftShift, False, 0);
    XFlush(handle);
    done(plan.isComplete() ? QString()
                           : tr("These characters are not on a US keyboard and were left out: %1")
                                 .arg(plan.skipped));
}

void XTestInputBackend::pressKeys(const QList<int> &keyCodes, Done done) {
    const Result<Display *> open = display();
    if (!open) {
        done(open.error());
        return;
    }
    ::Display *handle = open.value()->handle;
    for (const int code : keyCodes) XTestFakeKeyEvent(handle, unsigned(code + kEvdevToX11), True, 0);
    for (auto code = keyCodes.crbegin(); code != keyCodes.crend(); ++code) {
        XTestFakeKeyEvent(handle, unsigned(*code + kEvdevToX11), False, 0);
    }
    XFlush(handle);
    done(QString());
}

void XTestInputBackend::mouseMove(int dx, int dy, Done done) {
    const Result<Display *> open = display();
    if (!open) {
        done(open.error());
        return;
    }
    XTestFakeRelativeMotionEvent(open.value()->handle, dx, dy, 0);
    XFlush(open.value()->handle);
    done(QString());
}

void XTestInputBackend::mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) {
    const Result<Display *> open = display();
    if (!open) {
        done(open.error());
        return;
    }
    ::Display *handle = open.value()->handle;
    // The caller measured on a screen of screenWidth × screenHeight; scale onto this X screen.
    const int actualWidth = DisplayWidth(handle, DefaultScreen(handle));
    const int actualHeight = DisplayHeight(handle, DefaultScreen(handle));
    const int scaledX = screenWidth > 1 ? int(qint64(x) * (actualWidth - 1) / (screenWidth - 1)) : x;
    const int scaledY = screenHeight > 1 ? int(qint64(y) * (actualHeight - 1) / (screenHeight - 1)) : y;
    XTestFakeMotionEvent(handle, DefaultScreen(handle), scaledX, scaledY, 0);
    XFlush(handle);
    done(QString());
}

void XTestInputBackend::mouseClick(int button, int count, Done done) {
    const Result<Display *> open = display();
    if (!open) {
        done(open.error());
        return;
    }
    ::Display *handle = open.value()->handle;
    const unsigned int x11Button = unsigned(toX11Button(button));
    for (int click = 0; click < qBound(1, count, 3); ++click) {
        XTestFakeButtonEvent(handle, x11Button, True, 0);
        XTestFakeButtonEvent(handle, x11Button, False, 0);
    }
    XFlush(handle);
    done(QString());
}

void XTestInputBackend::mouseScroll(int dx, int dy, Done done) {
    const Result<Display *> open = display();
    if (!open) {
        done(open.error());
        return;
    }
    ::Display *handle = open.value()->handle;
    // X11 has no scroll axis: wheel steps are buttons 4/5 up/down and 6/7 left/right.
    const auto click = [handle](unsigned int button, int times) {
        for (int step = 0; step < times; ++step) {
            XTestFakeButtonEvent(handle, button, True, 0);
            XTestFakeButtonEvent(handle, button, False, 0);
        }
    };
    if (dy != 0) click(dy > 0 ? 4 : 5, qAbs(dy));
    if (dx != 0) click(dx > 0 ? 7 : 6, qAbs(dx));
    XFlush(handle);
    done(QString());
}

} // namespace nexa
