#include "app/PhonePanel.h"

#include "system/android/AndroidDevice.h"
#include "system/android/PhoneMirror.h"
#include "ui/NexaTheme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kPadX = 14;
constexpr int kCorner = 14;         // a phone's rounded screen

void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

// The picture of the phone's screen, scaled to fit, with the corners of a phone.
class PhonePanel::ScreenView : public QWidget {
public:
    explicit ScreenView(PhonePanel *panel) : QWidget(panel), panel_(panel) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(120, 200);
    }

    void setFrame(const QImage &frame) {
        frame_ = frame;
        update();
    }
    void setPlaceholder(const QString &text) {
        placeholder_ = text;
        update();
    }
    bool hasFrame() const { return !frame_.isNull(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRect box = rect().adjusted(kPadX, 4, -kPadX, -4);
        if (frame_.isNull()) {
            painter.setPen(panel_->mutedColor());
            painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
            return;
        }
        const QSize fitted = frame_.size().scaled(box.size(), Qt::KeepAspectRatio);
        QRect target(QPoint(), fitted);
        target.moveCenter(box.center());
        QPainterPath clip;
        clip.addRoundedRect(target, kCorner, kCorner);
        painter.save();
        painter.setClipPath(clip);
        painter.drawImage(target, frame_);
        painter.restore();
        painter.setPen(QPen(panel_->lineColor(), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(target).adjusted(0.5, 0.5, -0.5, -0.5), kCorner, kCorner);
    }

private:
    PhonePanel *panel_;
    QImage frame_;
    QString placeholder_;
};

PhonePanel::PhonePanel(AndroidDevice *phone, QWidget *parent)
    : QDockWidget(tr("Phone"), parent), phone_(phone), mirror_(new PhoneMirror(phone, this)) {
    setObjectName(u"phonePanel"_s);
    setFeatures(QDockWidget::DockWidgetClosable);
    const ThemeTokens tokens = NexaTheme::tokens(ThemeScheme::Dark);
    muted_ = tokens.muted;
    line_ = tokens.line;
    accent_ = tokens.idle;
    error_ = tokens.error;

    // The title: what this is, and which phone.
    auto *title = new QWidget(this);
    title->setObjectName(u"dockTitle"_s);
    auto *titleLayout = new QHBoxLayout(title);
    titleLayout->setContentsMargins(kPadX, 12, kPadX, 12);
    auto *text = new QLabel(tr("Phone"), title);
    text->setObjectName(u"dockTitleText"_s);
    note_ = new QLabel(title);
    note_->setObjectName(u"dockTitleNote"_s);
    titleLayout->addWidget(text);
    titleLayout->addStretch();
    titleLayout->addWidget(note_);
    setTitleBarWidget(title);

    auto *body = new QWidget(this);
    body->setObjectName(u"phoneBody"_s);
    auto *column = new QVBoxLayout(body);
    column->setContentsMargins(0, 10, 0, 10);
    column->setSpacing(8);
    info_ = new QLabel(body);
    info_->setObjectName(u"phoneInfo"_s);
    info_->setWordWrap(true);
    info_->setContentsMargins(kPadX, 0, kPadX, 0);
    column->addWidget(info_);
    screen_ = new ScreenView(this);
    column->addWidget(screen_, 1);

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(kPadX, 0, kPadX, 0);
    bottom->setSpacing(8);
    status_ = new QLabel(body);
    status_->setObjectName(u"phoneStatus"_s);
    bottom->addWidget(status_, 1);
    pause_ = new QToolButton(body);
    pause_->setObjectName(u"phoneButton"_s);
    pause_->setCheckable(true);
    pause_->setCursor(Qt::PointingHandCursor);
    pause_->setText(tr("Pause"));
    pause_->setToolTip(tr("Stop refreshing the picture (the phone is asked for it about twice a second)"));
    connect(pause_, &QToolButton::toggled, this, &PhonePanel::setPaused);
    bottom->addWidget(pause_);
    column->addLayout(bottom);
    setWidget(body);

    connect(mirror_, &PhoneMirror::frame, this, &PhonePanel::showFrame);
    connect(mirror_, &PhoneMirror::failed, this, [this](const QString &message) { showStatus(message, true); });
    connect(this, &QDockWidget::visibilityChanged, this, [this] { updateMirror(); });
    if (phone_) {
        connect(phone_, &AndroidDevice::deviceChanged, this, &PhonePanel::showDevice);
        showDevice(phone_->currentDescription());
    } else {
        showDevice({});
    }
}

void PhonePanel::setPaused(bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    if (pause_->isChecked() != paused) pause_->setChecked(paused);
    pause_->setText(paused ? tr("Live") : tr("Pause"));
    updateMirror();
}

void PhonePanel::showFrame(const QImage &frame, qint64 elapsedMs) {
    screen_->setFrame(frame);
    showStatus(elapsedMs > 0 ? tr("Live · %1 s per frame").arg(elapsedMs / 1000.0, 0, 'f', 1) : tr("Live"), false);
}

void PhonePanel::showDevice(const QString &description) {
    const bool connected = !description.isEmpty();
    note_->setText(connected ? description : tr("none connected"));
    if (connected != connected_) {
        connected_ = connected;
        screen_->setFrame({});
        if (connected) {
            screen_->setPlaceholder(tr("Waiting for the phone's screen…"));
            refreshInfo();
        } else {
            info_->setText(tr("No phone is connected. Settings → Android shows how to connect one."));
            screen_->setPlaceholder(tr("No phone"));
            showStatus({}, false);
        }
        updateMirror();
        emit phoneChanged(connected);
    }
}

void PhonePanel::refreshInfo() {
    if (!phone_ || !connected_) return;
    phone_->info([this](Result<AndroidDevice::Info> info) {
        if (!connected_) return;
        if (!info) {
            info_->setText(info.error().message);
            return;
        }
        const AndroidDevice::Info &value = info.value();
        QStringList parts;
        const QString name = (value.manufacturer.isEmpty() || value.model.startsWith(value.manufacturer, Qt::CaseInsensitive))
                                 ? value.model
                                 : value.manufacturer + u' ' + value.model;
        parts.append(name.isEmpty() ? value.serial : name);
        if (!value.release.isEmpty()) parts.append(tr("Android %1").arg(value.release));
        if (value.battery >= 0) parts.append(tr("battery %1%").arg(value.battery));
        if (value.screen.isValid()) parts.append(u"%1×%2"_s.arg(value.screen.width()).arg(value.screen.height()));
        info_->setText(parts.join(u" · "_s));
    });
}

void PhonePanel::updateMirror() {
    const bool wanted = connected_ && !paused_ && isVisible();
    mirror_->setActive(wanted);
    if (connected_ && paused_) showStatus(tr("Paused"), false);
}

void PhonePanel::showStatus(const QString &text, bool isError) {
    status_->setText(text);
    status_->setProperty("error", isError);
    repolish(status_);
}

} // namespace nexa
