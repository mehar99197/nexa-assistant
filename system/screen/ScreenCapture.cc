#include "system/screen/ScreenCapture.h"

#include "core/Logging.h"

#include <QBuffer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QPointer>
#include <QRandomGenerator>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QString kPortal = u"org.freedesktop.portal.Desktop"_s;
const QString kPortalPath = u"/org/freedesktop/portal/desktop"_s;
const QString kRequestInterface = u"org.freedesktop.portal.Request"_s;
// The first capture may wait for GNOME's permission dialog.
constexpr int kTimeoutMs = 60'000;

} // namespace

ScreenCapture::ScreenCapture(QObject *parent) : QObject(parent) {
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        unsubscribe();
        finish(Error{ErrorCode::Timeout, u"The screenshot portal did not answer"_s});
    });
    pool_.setMaxThreadCount(1);
}

ScreenCapture::~ScreenCapture() {
    pool_.waitForDone();
}

ScreenCapture::Shot ScreenCapture::encode(const QImage &image, int maxSide, int quality) {
    Shot shot;
    shot.realSize = image.size();
    QImage scaled = image;
    if (image.width() > maxSide || image.height() > maxSide) {
        scaled = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    scaled = scaled.convertToFormat(QImage::Format_RGB888);
    shot.imageSize = scaled.size();
    shot.scale = scaled.width() > 0 ? double(image.width()) / double(scaled.width()) : 1.0;
    QBuffer buffer(&shot.jpeg);
    buffer.open(QIODevice::WriteOnly);
    scaled.save(&buffer, "JPEG", quality);
    return shot;
}

void ScreenCapture::capture(Done done) {
    if (pending_) {
        done(Error{ErrorCode::Unknown, u"A screenshot is already being taken"_s});
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        done(Error{ErrorCode::NotSupported, u"No D-Bus session bus"_s});
        return;
    }
    pending_ = std::move(done);

    // Subscribe to the Request object before calling, or a fast Response could be missed.
    const QString token = u"nexa%1"_s.arg(QRandomGenerator::global()->generate(), 0, 16);
    const QString sender = bus.baseService().mid(1).replace(u'.', u'_');
    subscribe(u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, token));

    QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, u"org.freedesktop.portal.Screenshot"_s,
                                                       u"Screenshot"_s);
    call << QString() << QVariantMap{{u"handle_token"_s, token}, {u"interactive"_s, false}};
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, kTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        if (!pending_) return;
        const QDBusPendingReply<QDBusObjectPath> handle = *reply;
        if (handle.isError()) {
            unsubscribe();
            finish(Error{ErrorCode::NotSupported, u"Screenshot portal: %1"_s.arg(handle.error().message())});
            return;
        }
        // Older portals pick their own request path.
        if (handle.value().path() != requestPath_) subscribe(handle.value().path());
    });
    timeout_.start(kTimeoutMs);
}

void ScreenCapture::cancel() {
    if (!pending_) return;
    unsubscribe();
    finish(Error{ErrorCode::Cancelled, u"Screenshot cancelled"_s});
}

void ScreenCapture::subscribe(const QString &path) {
    unsubscribe();
    requestPath_ = path;
    QDBusConnection::sessionBus().connect(kPortal, path, kRequestInterface, u"Response"_s, this,
                                          SLOT(onResponse(uint, QVariantMap)));
}

void ScreenCapture::unsubscribe() {
    if (requestPath_.isEmpty()) return;
    QDBusConnection::sessionBus().disconnect(kPortal, requestPath_, kRequestInterface, u"Response"_s, this,
                                             SLOT(onResponse(uint, QVariantMap)));
    requestPath_.clear();
}

void ScreenCapture::onResponse(uint response, const QVariantMap &results) {
    unsubscribe();
    if (!pending_) return;
    if (response != 0) {
        finish(Error{ErrorCode::PermissionDenied, response == 1 ? u"Screenshot permission was denied"_s
                                                                : u"The screenshot failed"_s});
        return;
    }
    const QString file = QUrl(results.value(u"uri"_s).toString()).toLocalFile();
    if (file.isEmpty()) {
        finish(Error{ErrorCode::Io, u"The portal returned no screenshot file"_s});
        return;
    }
    const QPointer<ScreenCapture> self(this);
    pool_.start([this, self, file] {
        const QImage image(file);
        QFile::remove(file);  // screenshots are used once and not kept
        Result<Shot> shot = image.isNull() ? Result<Shot>(Error{ErrorCode::Io, u"Cannot read %1"_s.arg(file)})
                                           : Result<Shot>(encode(image));
        QMetaObject::invokeMethod(this, [self, shot = std::move(shot)]() mutable {
            if (self) self->finish(std::move(shot));
        }, Qt::QueuedConnection);
    });
}

void ScreenCapture::finish(Result<Shot> result) {
    timeout_.stop();
    if (!pending_) return;
    Done done = std::move(pending_);
    pending_ = nullptr;
    if (result) {
        lastScreenSize_ = result.value().realSize;
        lastScale_ = result.value().scale;
    }
    qCDebug(lcApp) << "screenshot" << (result ? u"ok"_s : result.error().message);
    done(std::move(result));
}

} // namespace nexa
