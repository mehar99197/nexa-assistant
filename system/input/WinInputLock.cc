#include "system/input/WinInputLock.h"

#include <QMetaObject>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <chrono>
#include <future>

using namespace Qt::StringLiterals;

namespace nexa {

#ifdef Q_OS_WIN

namespace {

std::atomic<WinInputLock *> gActiveLock{nullptr};
std::atomic<bool> gLocked{false};

LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && gLocked.load(std::memory_order_acquire)) {
        const auto *kbd = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
        const bool injected = kbd && ((kbd->flags & LLKHF_INJECTED) != 0 || (kbd->flags & LLKHF_LOWER_IL_INJECTED) != 0);
        if (!injected) return 1;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && gLocked.load(std::memory_order_acquire)) {
        const auto *mouse = reinterpret_cast<const MSLLHOOKSTRUCT *>(lParam);
        const bool injected =
            mouse && ((mouse->flags & LLMHF_INJECTED) != 0 || (mouse->flags & LLMHF_LOWER_IL_INJECTED) != 0);
        if (!injected) return 1;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace

WinInputLock::WinInputLock(QObject *parent) : IInputLock(parent) {}

WinInputLock::~WinInputLock() {
    WinInputLock::unlock();
}

bool WinInputLock::canLock(QString *why) const {
    if (why) why->clear();
    return true;
}

void WinInputLock::lock(std::function<void(Result<QStringList>)> done) {
    if (locked_.load()) {
        done(QStringList{u"Windows keyboard hook (Ctrl+Alt+Del / UAC remain OS-handled)"_s, u"Windows pointer hook"_s});
        return;
    }
    if (hookThread_.joinable()) hookThread_.join();

    gActiveLock.store(this, std::memory_order_release);
    gLocked.store(true, std::memory_order_release);
    locked_.store(true, std::memory_order_release);

    auto ready = std::make_shared<std::promise<bool>>();
    std::future<bool> readyFuture = ready->get_future();

    hookThread_ = std::thread([this, ready] {
        hookThreadId_.store(GetCurrentThreadId(), std::memory_order_release);
        HHOOK kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
        HHOOK mouseHook = SetWindowsHookExW(WH_MOUSE_LL, lowLevelMouseProc, GetModuleHandleW(nullptr), 0);
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        const bool ok = kbdHook != nullptr && mouseHook != nullptr;
        ready->set_value(ok);
        if (ok) {
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (kbdHook) UnhookWindowsHookEx(kbdHook);
        if (mouseHook) UnhookWindowsHookEx(mouseHook);
        hookThreadId_.store(0, std::memory_order_release);
    });

    const bool ok = readyFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready && readyFuture.get();
    if (!ok) {
        unlock();
        done(Error{ErrorCode::NotSupported, u"Windows refused the low-level input hook."_s});
        return;
    }
    done(QStringList{u"Windows keyboard hook (Ctrl+Alt+Del & UAC handled by Windows)"_s, u"Windows pointer hook"_s});
}

void WinInputLock::unlock() {
    gLocked.store(false, std::memory_order_release);
    locked_.store(false, std::memory_order_release);
    const DWORD tid = hookThreadId_.load(std::memory_order_acquire);
    if (tid != 0) PostThreadMessageW(tid, WM_QUIT, 0, 0);
    if (hookThread_.joinable()) hookThread_.join();
    if (gActiveLock.load(std::memory_order_acquire) == this) gActiveLock.store(nullptr, std::memory_order_release);
}

#else

WinInputLock::WinInputLock(QObject *parent) : IInputLock(parent) {}
WinInputLock::~WinInputLock() = default;

bool WinInputLock::canLock(QString *why) const {
    if (why) *why = u"Win32 input lock is only available on Windows."_s;
    return false;
}

void WinInputLock::lock(std::function<void(Result<QStringList>)> done) {
    done(Error{ErrorCode::NotSupported, u"Win32 input lock is only available on Windows."_s});
}

void WinInputLock::unlock() {}

#endif

} // namespace nexa
