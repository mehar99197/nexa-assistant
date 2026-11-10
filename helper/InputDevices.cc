#include "helper/InputDevices.h"

#include <QStringList>

#include <linux/input-event-codes.h>

#include <initializer_list>

using namespace Qt::StringLiterals;

namespace nexa::inputdevices {

namespace {

bool has(const std::vector<bool> &bits, int bit) {
    return bit >= 0 && size_t(bit) < bits.size() && bits[size_t(bit)];
}

bool hasAll(const std::vector<bool> &bits, std::initializer_list<int> wanted) {
    for (const int bit : wanted) {
        if (!has(bits, bit)) return false;
    }
    return true;
}

bool hasAny(const std::vector<bool> &bits, std::initializer_list<int> wanted) {
    for (const int bit : wanted) {
        if (has(bits, bit)) return true;
    }
    return false;
}

} // namespace

std::vector<bool> parseBitmap(const QString &sysfsWords, int wordBits) {
    const QStringList words = sysfsWords.simplified().split(u' ', Qt::SkipEmptyParts);
    if (words.isEmpty() || wordBits <= 0 || wordBits > 64) return {};
    std::vector<bool> bits(size_t(words.size()) * size_t(wordBits), false);
    for (qsizetype index = 0; index < words.size(); ++index) {
        bool ok = false;
        const quint64 word = words.at(index).toULongLong(&ok, 16);
        if (!ok) return {};
        // The last word written is word 0, bits 0 … wordBits - 1.
        const size_t base = size_t(words.size() - 1 - index) * size_t(wordBits);
        for (int bit = 0; bit < wordBits; ++bit) {
            if ((word >> bit) & 1U) bits[base + size_t(bit)] = true;
        }
    }
    return bits;
}

Kind classify(const Caps &caps) {
    if (caps.isVirtual) return Kind::Other;
    const std::vector<bool> ev = parseBitmap(caps.ev);
    const std::vector<bool> key = parseBitmap(caps.key);
    if (!has(ev, EV_KEY)) return Kind::Other;
    if (hasAll(key, {KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER})) return Kind::Keyboard;
    if (has(ev, EV_REL) && hasAll(parseBitmap(caps.rel), {REL_X, REL_Y}) && has(key, BTN_LEFT)) return Kind::Pointer;
    if (has(ev, EV_ABS) && hasAll(parseBitmap(caps.abs), {ABS_X, ABS_Y})
        && hasAny(key, {BTN_TOUCH, BTN_TOOL_FINGER, BTN_LEFT})) {
        return Kind::Pointer;
    }
    return Kind::Other;
}

} // namespace nexa::inputdevices
