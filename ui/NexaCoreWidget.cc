#include "ui/NexaCoreWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <vector>

namespace nexa {

namespace {

// The dark palette's values, so the core looks right before (or without) a stylesheet.
constexpr const char *kDefaultColors[kCoreModeCount] = {"#46B7B0", "#6FD38A", "#5BE3D6", "#9DB4FF", "#B39DFF",
                                                        "#F0B24B", "#7BE8C0", "#E6FFFB", "#FF7A66"};

QColor mixColor(const QColor &a, const QColor &b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

} // namespace

NexaCoreWidget::NexaCoreWidget(QWidget *parent)
    : QWidget(parent),
      panel_(QStringLiteral("#151A21")),
      line_(QStringLiteral("#2A323D")),
      muted_(QStringLiteral("#98A4B3")),
      text_(QStringLiteral("#E7ECF2")),
      ground_(QStringLiteral("#0D1014")) {
    for (int i = 0; i < kCoreModeCount; ++i) colors_[i] = QColor(QLatin1StringView(kDefaultColors[i]));
    setAttribute(Qt::WA_TranslucentBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    clock_.start();
    connect(&timer_, &QTimer::timeout, this, &NexaCoreWidget::tick);
    restartTimer();
}

void NexaCoreWidget::setMode(CoreMode mode) {
    pending_.reset();
    if (mode == mode_) return;
    // The fast-path snap lasts a few milliseconds in reality; it stays on screen for its ping,
    // unless something the user must see at once takes over.
    const bool holding = mode_ == CoreMode::FastPath && clock_.elapsed() - modeStartMs_ < kFastPathHoldMs;
    const bool urgent = mode == CoreMode::Listening || mode == CoreMode::Offline || mode == CoreMode::Speaking;
    if (holding && !urgent) {
        pending_ = mode;
        return;
    }
    applyMode(mode);
}

void NexaCoreWidget::applyMode(CoreMode mode) {
    previous_ = mode_;
    mode_ = mode;
    modeStartMs_ = clock_.elapsed();
    fadeStartMs_ = modeStartMs_;
    restartTimer();
    update();
    emit modeChanged(mode);
}

void NexaCoreWidget::setLevel(double level) {
    level_ = std::clamp(level, 0.0, 1.0);
}

void NexaCoreWidget::setStyle(CoreStyle style) {
    if (style_ == style) return;
    style_ = style;
    restartTimer();
    update();
}

void NexaCoreWidget::setCoreStyleIndex(int index) {
    if (index >= 0 && index < kCoreStyleCount) setStyle(static_cast<CoreStyle>(index));
}

void NexaCoreWidget::setShape(CoreShape shape) {
    if (shape_ == shape) return;
    shape_ = shape;
    update();
}

void NexaCoreWidget::setCoreShapeIndex(int index) {
    if (index >= 0 && index < kCoreShapeCount) setShape(static_cast<CoreShape>(index));
}

void NexaCoreWidget::setLitNodes(CoreNodes nodes) {
    if (lit_ == nodes) return;
    lit_ = nodes;
    update();
}

void NexaCoreWidget::setShowNodes(bool show) {
    nodes_ = show;
    update();
}

void NexaCoreWidget::setShowLabels(bool show) {
    labels_ = show;
    update();
}

void NexaCoreWidget::setDiameter(int pixels) {
    diameter_ = pixels;
    if (pixels > 0) {
        setFixedSize(pixels, pixels);
    } else {
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    updateGeometry();
}

void NexaCoreWidget::setAnimated(bool animated) {
    animated_ = animated;
    restartTimer();
}

QColor NexaCoreWidget::colorFor(CoreMode mode) const {
    return colors_[static_cast<int>(mode)];
}

void NexaCoreWidget::setColor(int index, const QColor &colour) {
    colors_[index] = colour;
    update();
}

QSize NexaCoreWidget::sizeHint() const {
    return diameter_ > 0 ? QSize(diameter_, diameter_) : QSize(96, 96);
}

QSize NexaCoreWidget::minimumSizeHint() const {
    return diameter_ > 0 ? QSize(diameter_, diameter_) : QSize(24, 24);
}

void NexaCoreWidget::restartTimer() {
    if (!animated_ || !isVisible()) {
        timer_.stop();
        return;
    }
    const bool fading = fadeStartMs_ >= 0;
    const int hz = fading ? 60 : coreStyleFrameRate(style_, mode_);
    timer_.start(1000 / hz);
}

void NexaCoreWidget::tick() {
    const qint64 now = clock_.elapsed();
    if (pending_ && now - modeStartMs_ >= kFastPathHoldMs) {
        const CoreMode next = *pending_;
        pending_.reset();
        applyMode(next);
    }
    if (fadeStartMs_ >= 0 && now - fadeStartMs_ > kCrossFadeMs) {
        fadeStartMs_ = -1;
        restartTimer();
    }
    // A minimised window still counts as visible; painting it would only cost cores.
    if (window() && window()->isMinimized()) return;
    update();
}

void NexaCoreWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    restartTimer();
}

void NexaCoreWidget::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    timer_.stop();
}

CoreNodes NexaCoreWidget::litFor(CoreMode mode) const {
    switch (mode) {
    case CoreMode::Listening:
    case CoreMode::Speaking:
    case CoreMode::Transcribing:
    case CoreMode::WakeWord:
        return VoiceNode;
    case CoreMode::Acting:
    case CoreMode::FastPath:
        return lit_ != NoNode ? lit_ : (PcNode | ToolsNode);
    default:
        return NoNode;
    }
}

void NexaCoreWidget::paintFrame(QPainter &painter, const QRectF &rect) const {
    // Not animated: one still frame of the state at rest, the same every time (the phone's brand
    // mark, snapshots, tests). Otherwise this moment of the motion.
    const double t = animated_ ? seconds() : 0.0;
    const double sinceMode = animated_ ? (clock_.elapsed() - modeStartMs_) / 1000.0 : 10.0;
    double blend = 1.0;
    if (animated_ && fadeStartMs_ >= 0) blend = easeCross(double(clock_.elapsed() - fadeStartMs_) / kCrossFadeMs);

    std::vector<CoreLayer> layers;
    const CoreFrame current = coreFrame(mode_, shownLevel_, t, sinceMode, style_);
    layers.push_back({mode_, current, colorFor(mode_), blend});
    double coreScale = current.coreScale;
    QColor accent = colorFor(mode_);
    CoreNodes lit = litFor(mode_);
    if (blend < 1.0) {
        const CoreFrame old = coreFrame(previous_, shownLevel_, t, sinceMode + kCrossFadeMs / 1000.0, style_);
        layers.push_back({previous_, old, colorFor(previous_), 1.0 - blend});
        coreScale = old.coreScale + (current.coreScale - old.coreScale) * blend;
        accent = mixColor(colorFor(previous_), colorFor(mode_), blend);
        if (blend < 0.5) lit |= litFor(previous_);
    }
    accent = NexaCorePainter::shiftedHue(accent, current.hueShift);

    CorePaint paint;
    paint.accent = accent;
    paint.wake = wakeColor();
    paint.panel = panel_;
    paint.line = line_;
    paint.muted = muted_;
    paint.text = text_;
    paint.ground = ground_;
    paint.glyph = ground_;
    // A lit node takes the colour of what lights it: the state while it is about the voice, the
    // action colour while tools run.
    switch (mode_) {
    case CoreMode::Listening:
    case CoreMode::Speaking:
    case CoreMode::Transcribing:
    case CoreMode::WakeWord:
        paint.act = accent;
        break;
    default:
        paint.act = actColor();
        break;
    }
    NexaCorePainter::paint(painter, rect, paint, CoreGeometry{nodes_, labels_, shape_}, lit, coreScale, layers);
}

void NexaCoreWidget::paintEvent(QPaintEvent *) {
    shownLevel_ = animated_ ? shownLevel_ + (level_ - shownLevel_) * 0.35 : level_;
    QPainter painter(this);
    paintFrame(painter, QRectF(rect()).adjusted(1, 1, -1, -1));
}

QPixmap NexaCoreWidget::snapshot(CoreMode mode, int size, bool nodes, qreal devicePixelRatio) const {
    QPixmap pixmap(QSize(size, size) * devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    CorePaint paint;
    paint.accent = colorFor(mode);
    paint.act = mode == CoreMode::Acting ? actColor() : paint.accent;
    paint.wake = wakeColor();
    paint.panel = panel_;
    paint.line = line_;
    paint.muted = muted_;
    paint.text = text_;
    paint.ground = ground_;
    paint.glyph = ground_;
    // At rest: well into the state, so one-shot motion (the fast-path ping) has passed.
    const CoreFrame frame = coreFrame(mode, 0.0, 0.0, 10.0, style_);
    const CoreLayer layer{mode, frame, paint.accent, 1.0};
    const std::array<CoreLayer, 1> layers{layer};
    NexaCorePainter::paint(painter, QRectF(0, 0, size, size).adjusted(0.5, 0.5, -0.5, -0.5), paint,
                           CoreGeometry{nodes, false, shape_}, litFor(mode), 1.0, layers);
    return pixmap;
}

// ---------------------------------------------------------------------------------------------

NexaCoreButton::NexaCoreButton(QWidget *parent) : NexaCoreWidget(parent) {
    setShowNodes(false);
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
}

void NexaCoreButton::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        NexaCoreWidget::mousePressEvent(event);
        return;
    }
    held_ = true;
    emit pressed();
}

void NexaCoreButton::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !held_) {
        NexaCoreWidget::mouseReleaseEvent(event);
        return;
    }
    held_ = false;
    emit released();
}

void NexaCoreButton::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && !held_) {
        held_ = true;
        emit pressed();
        return;
    }
    NexaCoreWidget::keyPressEvent(event);
}

void NexaCoreButton::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && held_) {
        held_ = false;
        emit released();
        return;
    }
    NexaCoreWidget::keyReleaseEvent(event);
}

} // namespace nexa
