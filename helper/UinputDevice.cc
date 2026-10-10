#include "helper/UinputDevice.h"

#include "core/Logging.h"

#include <QThread>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kAbsoluteMax = 65535;     // the absolute device's own axis range
// A key held for no time at all can be dropped by a toolkit that polls; this is about as fast as
// a quick human keystroke.
constexpr int kKeyHoldUs = 2'000;
constexpr int kBetweenKeysUs = 2'000;
// Freshly created devices need a moment before the compositor has them open, or the first events
// go nowhere.
constexpr int kSettleMs = 250;

Error systemError(const QString &what) {
    return Error{ErrorCode::Io, u"%1: %2"_s.arg(what, QString::fromLocal8Bit(std::strerror(errno)))};
}

// Fills in the identity a device reports to libinput and udev.
Result<void> setUpDevice(int fd, const char *name, quint16 product) {
    uinput_setup setup{};
    // BUS_USB, not BUS_VIRTUAL: the compositor's input stack takes pointer devices on the virtual
    // bus but never acts on their motion, while a keyboard on the same bus works. Every other
    // uinput tool reports USB, so this follows them.
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x4e78;           // "Nx"
    setup.id.product = product;
    setup.id.version = 1;
    std::strncpy(setup.name, name, sizeof setup.name - 1);
    if (::ioctl(fd, UI_DEV_SETUP, &setup) < 0) return systemError(u"UI_DEV_SETUP"_s);
    if (::ioctl(fd, UI_DEV_CREATE) < 0) return systemError(u"UI_DEV_CREATE"_s);
    return {};
}

Result<int> openUinput() {
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            return Error{ErrorCode::PermissionDenied, u"/dev/uinput is not accessible — the helper must run as root."_s};
        }
        if (errno == ENOENT) {
            return Error{ErrorCode::NotFound, u"/dev/uinput is missing — load the uinput kernel module."_s};
        }
        return systemError(u"opening /dev/uinput"_s);
    }
    return fd;
}

} // namespace

UinputDevice::~UinputDevice() {
    close();
}

int UinputDevice::toAbsolute(int position, int screenSize) {
    if (screenSize <= 1) return 0;
    const int clamped = std::clamp(position, 0, screenSize - 1);
    // Round to the nearest step so the last pixel reaches the end of the axis.
    return int((qint64(clamped) * kAbsoluteMax + (screenSize - 1) / 2) / (screenSize - 1));
}

Result<void> UinputDevice::open() {
    if (isOpen()) return {};
    if (const Result<void> keyboard = createKeyboard(); !keyboard) {
        close();
        return keyboard;
    }
    if (const Result<void> mouse = createMouse(); !mouse) {
        close();
        return mouse;
    }
    if (const Result<void> pointer = createAbsolutePointer(); !pointer) {
        close();
        return pointer;
    }
    QThread::msleep(kSettleMs);
    qCInfo(lcApp) << "virtual input devices created";
    return {};
}

void UinputDevice::close() {
    lastPointer_ = -1;
    for (int *fd : {&keyboard_, &mouse_, &absolute_}) {
        if (*fd < 0) continue;
        ::ioctl(*fd, UI_DEV_DESTROY);
        ::close(*fd);
        *fd = -1;
    }
}

Result<void> UinputDevice::createKeyboard() {
    Result<int> fd = openUinput();
    if (!fd) return fd.error();
    keyboard_ = fd.value();
    if (::ioctl(keyboard_, UI_SET_EVBIT, EV_KEY) < 0) return systemError(u"UI_SET_EVBIT EV_KEY"_s);
    // Exactly the keys KeyMap can produce, so the device does not claim keys Nexa cannot send.
    for (const int code : keymap::allCodes()) {
        if (::ioctl(keyboard_, UI_SET_KEYBIT, code) < 0) return systemError(u"UI_SET_KEYBIT"_s);
    }
    return setUpDevice(keyboard_, "Nexa Virtual Keyboard", 0x0001);
}

Result<void> UinputDevice::createMouse() {
    Result<int> fd = openUinput();
    if (!fd) return fd.error();
    mouse_ = fd.value();
    if (::ioctl(mouse_, UI_SET_EVBIT, EV_KEY) < 0) return systemError(u"UI_SET_EVBIT EV_KEY"_s);
    for (const int code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        if (::ioctl(mouse_, UI_SET_KEYBIT, code) < 0) return systemError(u"UI_SET_KEYBIT"_s);
    }
    if (::ioctl(mouse_, UI_SET_EVBIT, EV_REL) < 0) return systemError(u"UI_SET_EVBIT EV_REL"_s);
    for (const int code : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL}) {
        if (::ioctl(mouse_, UI_SET_RELBIT, code) < 0) return systemError(u"UI_SET_RELBIT"_s);
    }
    return setUpDevice(mouse_, "Nexa Virtual Mouse", 0x0002);
}

Result<void> UinputDevice::createAbsolutePointer() {
    Result<int> fd = openUinput();
    if (!fd) return fd.error();
    absolute_ = fd.value();
    if (::ioctl(absolute_, UI_SET_EVBIT, EV_KEY) < 0) return systemError(u"UI_SET_EVBIT EV_KEY"_s);
    for (const int code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        if (::ioctl(absolute_, UI_SET_KEYBIT, code) < 0) return systemError(u"UI_SET_KEYBIT"_s);
    }
    if (::ioctl(absolute_, UI_SET_EVBIT, EV_ABS) < 0) return systemError(u"UI_SET_EVBIT EV_ABS"_s);
    // A fixed 0…65535 range on both axes, as virtual-machine mice report. libinput maps such a
    // device onto the output, which is what makes "go to this pixel" possible at all: relative
    // motion would be reshaped by pointer acceleration.
    for (const int code : {ABS_X, ABS_Y}) {
        uinput_abs_setup axis{};
        axis.code = quint16(code);
        axis.absinfo.minimum = 0;
        axis.absinfo.maximum = kAbsoluteMax;
        if (::ioctl(absolute_, UI_ABS_SETUP, &axis) < 0) return systemError(u"UI_ABS_SETUP"_s);
    }
    return setUpDevice(absolute_, "Nexa Virtual Pointer", 0x0003);
}

Result<void> UinputDevice::writeEvent(int fd, int type, int code, int value) {
    if (fd < 0) return Error{ErrorCode::Io, u"The virtual input device is not open."_s};
    input_event event{};
    event.type = quint16(type);
    event.code = quint16(code);
    event.value = value;        // the kernel stamps the time
    if (::write(fd, &event, sizeof event) != ssize_t(sizeof event)) return systemError(u"writing an input event"_s);
    return {};
}

Result<void> UinputDevice::sync(int fd) {
    return writeEvent(fd, EV_SYN, SYN_REPORT, 0);
}

Result<void> UinputDevice::tapKey(int code) {
    if (const Result<void> down = writeEvent(keyboard_, EV_KEY, code, 1); !down) return down;
    if (const Result<void> report = sync(keyboard_); !report) return report;
    ::usleep(kKeyHoldUs);
    if (const Result<void> up = writeEvent(keyboard_, EV_KEY, code, 0); !up) return up;
    return sync(keyboard_);
}

Result<void> UinputDevice::type(const QList<KeyStroke> &strokes) {
    if (const Result<void> ready = open(); !ready) return ready;
    bool shiftHeld = false;
    for (const KeyStroke &stroke : strokes) {
        if (stroke.shift != shiftHeld) {
            if (const Result<void> shift = writeEvent(keyboard_, EV_KEY, KEY_LEFTSHIFT, stroke.shift ? 1 : 0); !shift) {
                return shift;
            }
            if (const Result<void> report = sync(keyboard_); !report) return report;
            shiftHeld = stroke.shift;
        }
        if (const Result<void> key = tapKey(stroke.code); !key) return key;
        ::usleep(kBetweenKeysUs);
    }
    if (!shiftHeld) return {};
    if (const Result<void> shift = writeEvent(keyboard_, EV_KEY, KEY_LEFTSHIFT, 0); !shift) return shift;
    return sync(keyboard_);
}

Result<void> UinputDevice::pressChord(const QList<int> &codes) {
    if (const Result<void> ready = open(); !ready) return ready;
    for (const int code : codes) {
        if (const Result<void> down = writeEvent(keyboard_, EV_KEY, code, 1); !down) return down;
        if (const Result<void> report = sync(keyboard_); !report) return report;
        ::usleep(kBetweenKeysUs);
    }
    ::usleep(kKeyHoldUs);
    // Released in reverse, so the modifiers that opened the chord are the last to go.
    for (auto code = codes.crbegin(); code != codes.crend(); ++code) {
        if (const Result<void> up = writeEvent(keyboard_, EV_KEY, *code, 0); !up) return up;
        if (const Result<void> report = sync(keyboard_); !report) return report;
    }
    return {};
}

Result<void> UinputDevice::moveBy(int dx, int dy) {
    if (const Result<void> ready = open(); !ready) return ready;
    if (dx != 0) {
        if (const Result<void> x = writeEvent(mouse_, EV_REL, REL_X, dx); !x) return x;
    }
    if (dy != 0) {
        if (const Result<void> y = writeEvent(mouse_, EV_REL, REL_Y, dy); !y) return y;
    }
    lastPointer_ = mouse_;
    return sync(mouse_);
}

Result<void> UinputDevice::moveTo(int x, int y, int screenWidth, int screenHeight) {
    if (const Result<void> ready = open(); !ready) return ready;
    if (const Result<void> ax = writeEvent(absolute_, EV_ABS, ABS_X, toAbsolute(x, screenWidth)); !ax) return ax;
    if (const Result<void> ay = writeEvent(absolute_, EV_ABS, ABS_Y, toAbsolute(y, screenHeight)); !ay) return ay;
    lastPointer_ = absolute_;
    return sync(absolute_);
}

Result<void> UinputDevice::button(int code, bool down) {
    if (const Result<void> ready = open(); !ready) return ready;
    // From whichever device last moved the pointer, so the click lands where that motion put it.
    const int device = lastPointer_ >= 0 ? lastPointer_ : mouse_;
    if (const Result<void> event = writeEvent(device, EV_KEY, code, down ? 1 : 0); !event) return event;
    return sync(device);
}

Result<void> UinputDevice::scroll(int dx, int dy) {
    if (const Result<void> ready = open(); !ready) return ready;
    if (dy != 0) {
        if (const Result<void> wheel = writeEvent(mouse_, EV_REL, REL_WHEEL, dy); !wheel) return wheel;
    }
    if (dx != 0) {
        if (const Result<void> wheel = writeEvent(mouse_, EV_REL, REL_HWHEEL, dx); !wheel) return wheel;
    }
    return sync(mouse_);
}

} // namespace nexa
