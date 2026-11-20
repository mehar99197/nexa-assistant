#pragma once

// The root helper's lock as hardcore mode sees it, for tests: it answers at once, refuses, or answers when told to.

#include "core/mission/IInputLock.h"

#include <optional>

namespace nexa::test {

using namespace Qt::StringLiterals;

class FakeLock : public IInputLock {
public:
    bool canLock(QString *why) const override {
        if (why && !available) *why = u"The root helper is not installed."_s;
        return available;
    }
    void lock(std::function<void(Result<QStringList>)> done) override {
        ++locks;
        if (holdReply) {
            pending = std::move(done);
            return;
        }
        if (refuse) {
            done(*refuse);
            return;
        }
        held = true;
        done(QStringList{u"AT Translated Set 2 keyboard"_s, u"SynPS/2 Synaptics TouchPad"_s});
    }
    void unlock() override {
        ++unlocks;
        held = false;
    }
    void answer() {         // the held reply arrives
        held = true;
        auto done = std::move(pending);
        pending = nullptr;
        done(QStringList{u"AT Translated Set 2 keyboard"_s});
    }

    bool available{true};
    bool holdReply{false};
    std::optional<Error> refuse;
    std::function<void(Result<QStringList>)> pending;
    bool held{false};
    int locks{0};
    int unlocks{0};
};

} // namespace nexa::test
