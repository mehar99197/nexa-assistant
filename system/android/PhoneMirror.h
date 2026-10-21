#pragma once

#include "core/Result.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>

namespace nexa {

class AndroidDevice;

// Observer of the phone's screen for the window's Phone panel: while someone watches, one
// screenshot after another through AndroidDevice, the next asked for a moment after the last one
// arrived, so a slow phone is never asked twice at once. It stops as soon as nobody watches, and
// after a failure it waits longer before trying again. These pictures are the user's own view of
// their phone, like the status bar's — not actions, so they are not audited.
class PhoneMirror : public QObject {
    Q_OBJECT
public:
    explicit PhoneMirror(AndroidDevice *device, QObject *parent = nullptr);

    void setActive(bool active);
    bool isActive() const { return active_; }
    // The pause between one frame arriving and the next being asked for (milliseconds).
    void setInterval(int ms) { intervalMs_ = ms; }
    void refresh();                 // a frame now, if none is on its way

    QImage lastFrame() const { return lastFrame_; }
    QSize screenSize() const { return screenSize_; }    // the phone's real pixels, once known

signals:
    void frame(const QImage &image, qint64 elapsedMs);  // a new picture, and how long the phone took
    void failed(const QString &message);
    void activeChanged(bool active);

private:
    void request();
    void schedule(int delayMs);

    AndroidDevice *device_;
    QTimer timer_;
    QImage lastFrame_;
    QSize screenSize_;
    int intervalMs_{400};
    bool active_{false};
    bool inFlight_{false};
    quint64 generation_{0};         // a frame from before a stop is dropped
};

} // namespace nexa
