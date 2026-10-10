#pragma once

#include <QObject>

#include <functional>
#include <vector>

namespace nexa {

// Observer hub for "stop everything": every component that can run something long (streams,
// tools, child processes; later TTS, voice and adb jobs) registers a handler.
class PanicController : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    void addHandler(std::function<void()> handler) { handlers_.push_back(std::move(handler)); }
    void panic();

signals:
    void panicked();

private:
    std::vector<std::function<void()>> handlers_;
};

} // namespace nexa
