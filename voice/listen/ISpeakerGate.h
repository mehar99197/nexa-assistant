#pragma once

#include <QByteArray>
#include <QObject>

#include <functional>

namespace nexa {

// Whose voice it is (Strategy): what is said to Nexa hands-free — after its name, in a follow-up, over its speech — goes
// on only when it is the owner's. Push-to-talk is never checked: pressing the key says who it is.
class ISpeakerGate : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // Requests are checked at all: a voice is enrolled, and only it is to be listened to.
    virtual bool active() const = 0;
    // `done(owner, score)` on this object's thread, later; an inactive gate lets everything through at once.
    virtual void check(const QByteArray &pcm16k, std::function<void(bool owner, double score)> done) = 0;
};

} // namespace nexa
