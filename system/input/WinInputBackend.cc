#include "system/input/WinInputBackend.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QHash>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

#ifdef Q_OS_WIN
// Maps Linux EV_KEY codes (matching proto/KeyMap.cc) to Windows virtual key codes.
WORD vkFromEvKey(int code) {
    static const QHash<int, WORD> map = [] {
        QHash<int, WORD> m;
        m.insert(1 /* KEY_ESC */, VK_ESCAPE);
        m.insert(2 /* KEY_1 */, '1');
        m.insert(3 /* KEY_2 */, '2');
        m.insert(4 /* KEY_3 */, '3');
        m.insert(5 /* KEY_4 */, '4');
        m.insert(6 /* KEY_5 */, '5');
        m.insert(7 /* KEY_6 */, '6');
        m.insert(8 /* KEY_7 */, '7');
        m.insert(9 /* KEY_8 */, '8');
        m.insert(10 /* KEY_9 */, '9');
        m.insert(11 /* KEY_0 */, '0');
        m.insert(12 /* KEY_MINUS */, VK_OEM_MINUS);
        m.insert(13 /* KEY_EQUAL */, VK_OEM_PLUS);
        m.insert(14 /* KEY_BACKSPACE */, VK_BACK);
        m.insert(15 /* KEY_TAB */, VK_TAB);
        m.insert(16 /* KEY_Q */, 'Q');
        m.insert(17 /* KEY_W */, 'W');
        m.insert(18 /* KEY_E */, 'E');
        m.insert(19 /* KEY_R */, 'R');
        m.insert(20 /* KEY_T */, 'T');
        m.insert(21 /* KEY_Y */, 'Y');
        m.insert(22 /* KEY_U */, 'U');
        m.insert(23 /* KEY_I */, 'I');
        m.insert(24 /* KEY_O */, 'O');
        m.insert(25 /* KEY_P */, 'P');
        m.insert(26 /* KEY_LEFTBRACE */, VK_OEM_4);
        m.insert(27 /* KEY_RIGHTBRACE */, VK_OEM_6);
        m.insert(28 /* KEY_ENTER */, VK_RETURN);
        m.insert(29 /* KEY_LEFTCTRL */, VK_LCONTROL);
        m.insert(30 /* KEY_A */, 'A');
        m.insert(31 /* KEY_S */, 'S');
        m.insert(32 /* KEY_D */, 'D');
        m.insert(33 /* KEY_F */, 'F');
        m.insert(34 /* KEY_G */, 'G');
        m.insert(35 /* KEY_H */, 'H');
        m.insert(36 /* KEY_J */, 'J');
        m.insert(37 /* KEY_K */, 'K');
        m.insert(38 /* KEY_L */, 'L');
        m.insert(39 /* KEY_SEMICOLON */, VK_OEM_1);
        m.insert(40 /* KEY_APOSTROPHE */, VK_OEM_7);
        m.insert(41 /* KEY_GRAVE */, VK_OEM_3);
        m.insert(42 /* KEY_LEFTSHIFT */, VK_LSHIFT);
        m.insert(43 /* KEY_BACKSLASH */, VK_OEM_5);
        m.insert(44 /* KEY_Z */, 'Z');
        m.insert(45 /* KEY_X */, 'X');
        m.insert(46 /* KEY_C */, 'C');
        m.insert(47 /* KEY_V */, 'V');
        m.insert(48 /* KEY_B */, 'B');
        m.insert(49 /* KEY_N */, 'N');
        m.insert(50 /* KEY_M */, 'M');
        m.insert(51 /* KEY_COMMA */, VK_OEM_COMMA);
        m.insert(52 /* KEY_DOT */, VK_OEM_PERIOD);
        m.insert(53 /* KEY_SLASH */, VK_OEM_2);
        m.insert(54 /* KEY_RIGHTSHIFT */, VK_RSHIFT);
        m.insert(56 /* KEY_LEFTALT */, VK_LMENU);
        m.insert(57 /* KEY_SPACE */, VK_SPACE);
        m.insert(58 /* KEY_CAPSLOCK */, VK_CAPITAL);
        for (int n = 0; n < 10; ++n) m.insert(59 + n, WORD(VK_F1 + n));
        m.insert(69 /* KEY_NUMLOCK */, VK_NUMLOCK);
        m.insert(70 /* KEY_SCROLLLOCK */, VK_SCROLL);
        m.insert(87 /* KEY_F11 */, VK_F11);
        m.insert(88 /* KEY_F12 */, VK_F12);
        m.insert(97 /* KEY_RIGHTCTRL */, VK_RCONTROL);
        m.insert(99 /* KEY_SYSRQ */, VK_SNAPSHOT);
        m.insert(100 /* KEY_RIGHTALT */, VK_RMENU);
        m.insert(102 /* KEY_HOME */, VK_HOME);
        m.insert(103 /* KEY_UP */, VK_UP);
        m.insert(104 /* KEY_PAGEUP */, VK_PRIOR);
        m.insert(105 /* KEY_LEFT */, VK_LEFT);
        m.insert(106 /* KEY_RIGHT */, VK_RIGHT);
        m.insert(107 /* KEY_END */, VK_END);
        m.insert(108 /* KEY_DOWN */, VK_DOWN);
        m.insert(109 /* KEY_PAGEDOWN */, VK_NEXT);
        m.insert(110 /* KEY_INSERT */, VK_INSERT);
        m.insert(111 /* KEY_DELETE */, VK_DELETE);
        m.insert(119 /* KEY_PAUSE */, VK_PAUSE);
        m.insert(125 /* KEY_LEFTMETA */, VK_LWIN);
        m.insert(126 /* KEY_RIGHTMETA */, VK_RWIN);
        m.insert(127 /* KEY_COMPOSE */, VK_APPS);
        for (int n = 0; n < 12; ++n) m.insert(183 + n, WORD(VK_F13 + n));
        return m;
    }();
    return map.value(code, 0);
}

bool isExtendedKey(WORD vk) {
    switch (vk) {
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_SNAPSHOT:
        return true;
    default:
        return false;
    }
}
bool isProcessElevated(HANDLE proc) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(proc, TOKEN_QUERY, &token) || !token) return false;
    TOKEN_ELEVATION elev{};
    DWORD size = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size);
    ::CloseHandle(token);
    return ok != FALSE && elev.TokenIsElevated != 0;
}

bool isForegroundWindowElevated() {
    const HWND fg = ::GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) return false;
    if (isProcessElevated(::GetCurrentProcess())) return false;
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return ::GetLastError() == ERROR_ACCESS_DENIED;
    const bool elevated = isProcessElevated(proc);
    ::CloseHandle(proc);
    return elevated;
}

Result<QString> sendInputsChecked(std::vector<INPUT> &inputs) {
    if (inputs.empty()) return QString();
    ::SetLastError(0);
    const UINT expected = static_cast<UINT>(inputs.size());
    const UINT sent = ::SendInput(expected, inputs.data(), sizeof(INPUT));
    if (sent == expected) return QString();
    const DWORD err = ::GetLastError();
    if (isForegroundWindowElevated()) {
        return Error{ErrorCode::PermissionDenied,
                     u"Windows blocked input (UIPI): the active window is running as Administrator while Nexa is not. "
                     "Run Nexa as Administrator to control elevated windows."_s};
    }
    return Error{ErrorCode::Io,
                 u"Windows blocked input (SendInput sent %1 of %2 events, Win32 error %3)."_s
                     .arg(sent)
                     .arg(expected)
                     .arg(err)};
}
#endif

} // namespace

WinInputBackend::WinInputBackend(QObject *parent) : IInputBackend(parent) {}

QString WinInputBackend::name() const {
    return u"Windows SendInput"_s;
}

QString WinInputBackend::reach() const {
    return u"all visible desktop windows"_s;
}

void WinInputBackend::typeText(const QString &text, Done done) {
#ifdef Q_OS_WIN
    if (text.isEmpty()) {
        done(QString());
        return;
    }
    const std::u16string utf16 = text.toStdU16String();
    std::vector<INPUT> inputs;
    inputs.reserve(utf16.size() * 2);
    const WORD enterScan = static_cast<WORD>(::MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    for (const char16_t ch : utf16) {
        if (ch == u'\r') continue;
        if (ch == u'\n') {
            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wVk = VK_RETURN;
            down.ki.wScan = enterScan;
            INPUT up = down;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
            inputs.push_back(down);
            inputs.push_back(up);
            continue;
        }
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = WORD(ch);
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(down);
        inputs.push_back(up);
    }
    done(sendInputsChecked(inputs));
#else
    Q_UNUSED(text);
    done(Error{ErrorCode::NotSupported, u"WinInputBackend is only available on Windows."_s});
#endif
}

void WinInputBackend::pressKeys(const QList<int> &keyCodes, Done done) {
#ifdef Q_OS_WIN
    std::vector<WORD> vks;
    vks.reserve(size_t(keyCodes.size()));
    for (const int code : keyCodes) {
        const WORD vk = vkFromEvKey(code);
        if (vk != 0) vks.push_back(vk);
    }
    if (vks.empty()) {
        done(Error{ErrorCode::InvalidArgument, u"No supported Windows virtual keys in chord."_s});
        return;
    }
    std::vector<INPUT> inputs;
    inputs.reserve(vks.size() * 2);
    for (const WORD vk : vks) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = isExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0;
        inputs.push_back(in);
    }
    for (auto it = vks.rbegin(); it != vks.rend(); ++it) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = *it;
        in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(*it, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = KEYEVENTF_KEYUP | (isExtendedKey(*it) ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(in);
    }
    done(sendInputsChecked(inputs));
#else
    Q_UNUSED(keyCodes);
    done(Error{ErrorCode::NotSupported, u"WinInputBackend is only available on Windows."_s});
#endif
}

void WinInputBackend::mouseMove(int dx, int dy, Done done) {
#ifdef Q_OS_WIN
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    std::vector<INPUT> inputs{in};
    done(sendInputsChecked(inputs));
#else
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    done(Error{ErrorCode::NotSupported, u"WinInputBackend is only available on Windows."_s});
#endif
}

void WinInputBackend::mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) {
#ifdef Q_OS_WIN
    const int w = std::max(1, screenWidth > 0 ? screenWidth : GetSystemMetrics(SM_CXSCREEN));
    const int h = std::max(1, screenHeight > 0 ? screenHeight : GetSystemMetrics(SM_CYSCREEN));
    const int cx = std::clamp(x, 0, w - 1);
    const int cy = std::clamp(y, 0, h - 1);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>(std::lround((double(cx) * 65535.0) / double(std::max(1, w - 1))));
    in.mi.dy = static_cast<LONG>(std::lround((double(cy) * 65535.0) / double(std::max(1, h - 1))));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    std::vector<INPUT> inputs{in};
    done(sendInputsChecked(inputs));
#else
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(screenWidth);
    Q_UNUSED(screenHeight);
    done(Error{ErrorCode::NotSupported, u"WinInputBackend is only available on Windows."_s});
#endif
}

void WinInputBackend::mouseClick(int button, int count, Done done) {
#ifdef Q_OS_WIN
    DWORD downFlag = MOUSEEVENTF_LEFTDOWN;
    DWORD upFlag = MOUSEEVENTF_LEFTUP;
    if (button == 2) {
        downFlag = MOUSEEVENTF_RIGHTDOWN;
        upFlag = MOUSEEVENTF_RIGHTUP;
    } else if (button == 3) {
        downFlag = MOUSEEVENTF_MIDDLEDOWN;
        upFlag = MOUSEEVENTF_MIDDLEUP;
    }
    const int clicks = std::clamp(count, 1, 3);
    std::vector<INPUT> inputs;
    inputs.reserve(size_t(clicks) * 2);
    for (int i = 0; i < clicks; ++i) {
        INPUT down{};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = downFlag;
        INPUT up{};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = upFlag;
        inputs.push_back(down);
        inputs.push_back(up);
    }
    done(sendInputsChecked(inputs));
#else
    Q_UNUSED(button);
    Q_UNUSED(count);
    done(Error{ErrorCode::NotSupported, u"WinInputBackend is only available on Windows."_s});
#endif
}

void WinInputBackend::mouseScroll(int dx, int dy, Done done) {
#ifdef Q_OS_WIN
    std::vector<INPUT> inputs;
    if (dy != 0) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = DWORD(-dy * WHEEL_DELTA);
        inputs.push_back(in);
    }
    if (dx != 0) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = DWORD(dx * WHEEL_DELTA);
        inputs.push_back(in);
    }
    done(sendInputsChecked(inputs));
#else
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    done(Error{ErrorCode::NotSupported, u"WinInputBackend is only available on Windows."_s});
#endif
}

} // namespace nexa
