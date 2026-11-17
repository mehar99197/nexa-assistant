#pragma once

#include "core/mission/IInputLock.h"

#include <atomic>
#include <thread>

namespace nexa {

// Best-effort Windows Hardcore Mode input lock using Win32 WH_KEYBOARD_LL and WH_MOUSE_LL
// hooks on a dedicated message-pump thread.
//
// Injected automation events from Nexa itself (LLKHF_INJECTED / LLMHF_INJECTED from SendInput)
// are passed through unchanged, while physical keyboard/mouse events are swallowed while locked.
// Windows limitation: Ctrl+Alt+Del (SAS) and UAC secure desktop prompts cannot be intercepted by
// user-mode hooks — this is OS-enforced and documented in Hardcore Mode's banner.
class WinInputLock : public IInputLock {
    Q_OBJECT
public:
    explicit WinInputLock(QObject *parent = nullptr);
    ~WinInputLock() override;

    bool canLock(QString *why = nullptr) const override;
    void lock(std::function<void(Result<QStringList>)> done) override;
    void unlock() override;

private:
    std::atomic<bool> locked_{false};
    std::atomic<unsigned long> hookThreadId_{0};
    std::thread hookThread_;
};

} // namespace nexa
