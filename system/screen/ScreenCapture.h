#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <functional>

namespace nexa {

// Facade over the org.freedesktop.portal.Screenshot portal — the way to capture the screen on
// GNOME Wayland (GNOME asks the user once). Decoding and JPEG encoding run off the GUI thread.
class ScreenCapture : public QObject {
    Q_OBJECT
public:
    struct Shot {
        QByteArray jpeg;
        QSize imageSize;            // size of `jpeg`
        QSize realSize;             // physical screen pixels
        double scale{1.0};          // real pixels per image pixel
    };
    using Done = std::function<void(Result<Shot>)>;

    explicit ScreenCapture(QObject *parent = nullptr);
    ~ScreenCapture() override;

    void capture(Done done);        // one capture at a time
    void cancel();

    // Downscale so the long side is ≤ maxSide, then JPEG-encode (pure).
    static Shot encode(const QImage &image, int maxSide = 1280, int quality = 80);

    // Geometry of the most recent screenshot, so the pointer tools can turn a coordinate read off
    // that image into a screen pixel. An invalid size until one has been taken.
    QSize lastScreenSize() const { return lastScreenSize_; }
    double lastScale() const { return lastScale_; }

private slots:
    void onResponse(uint response, const QVariantMap &results);

private:
    void subscribe(const QString &path);
    void unsubscribe();
    void finish(Result<Shot> result);

    Done pending_;
    QSize lastScreenSize_;
    double lastScale_{1.0};
    QString requestPath_;
    QTimer timeout_;
    QThreadPool pool_;
};

} // namespace nexa
