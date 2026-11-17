#include "MessageDelegate.h"

#include <QAbstractItemView>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>

#include <algorithm>

namespace nexa {

namespace {

// ChatPage::Kind, by value: User, FromPc, Nexa, Step, StepFailed, Heard, Error.
constexpr int kUser = 0;
constexpr int kFromPc = 1;
constexpr int kStep = 3;
constexpr int kStepFailed = 4;
constexpr int kHeard = 5;
constexpr int kError = 6;

constexpr int kMargin = 12;         // inset from the list's edges
constexpr int kPadX = 13;           // inside a bubble
constexpr int kPadY = 9;
constexpr int kGap = 3;             // above and below a line
constexpr double kBubbleShare = 0.85;   // a bubble takes at most this much of the width

bool hasBubble(int kind) {
    return kind == kUser || kind == kFromPc;
}

bool isSmall(int kind) {
    return kind == kStep || kind == kStepFailed || kind == kHeard;
}

} // namespace

MessageDelegate::MessageDelegate(QAbstractItemView *view)
    : QStyledItemDelegate(view), view_(view), tokens_(NexaTheme::tokens(ThemeScheme::Dark)) {}

void MessageDelegate::setTokens(const ThemeTokens &tokens) {
    tokens_ = tokens;
    if (view_) view_->viewport()->update();
}

void MessageDelegate::setDesign(BubbleStyle bubbles, int radius, FontFace face) {
    bubbles_ = bubbles;
    radius_ = std::clamp(radius, 0, 22);
    face_ = face;
    if (view_) {
        view_->doItemsLayout();
        view_->viewport()->update();
    }
}

MessageDelegate::Shape MessageDelegate::shape(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const int kind = index.data(KindRole).toInt();
    const QString text = index.data(Qt::DisplayRole).toString();
    // sizeHint() may be asked without a rectangle: the list's own width is the one that counts.
    const int width = option.rect.width() > 0 ? option.rect.width() : (view_ ? view_->viewport()->width() : 320);
    const int usable = std::max(40, width - 2 * kMargin);
    const int left = option.rect.left() + kMargin;
    const int top = option.rect.top() + kGap;

    Shape shape;
    shape.font = option.font;
    if (isSmall(kind)) {
        shape.font = NexaTheme::scaled(option.font, 0.85, QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    } else if (face_ != FontFace::Sans) {
        shape.font.setFamily(NexaDesigns::fontFamily(face_));
    }
    const QFontMetrics metrics(shape.font);
    const bool userMsg = hasBubble(kind);
    const bool isNexa = kind == 2;
    if (bubbles_ == BubbleStyle::Cards && (userMsg || isNexa)) {
        const int maxText = std::max(20, usable - 2 * kPadX);
        const QRect wrapped = metrics.boundingRect(QRect(0, 0, maxText, 100000), Qt::TextWordWrap, text);
        const int boxHeight = wrapped.height() + 2 * kPadY;
        shape.bubble = QRect(left, top, usable, boxHeight);
        shape.text = shape.bubble.adjusted(kPadX, kPadY, -kPadX, -kPadY);
        shape.height = boxHeight + 2 * kGap;
    } else if (userMsg && bubbles_ == BubbleStyle::Lines) {
        const int maxText = std::max(20, int(usable * kBubbleShare) - kPadX);
        const QRect wrapped = metrics.boundingRect(QRect(0, 0, maxText, 100000), Qt::TextWordWrap, text);
        const int textWidth = std::max(20, wrapped.width());
        shape.text = QRect(left + usable - kPadX - textWidth, top + 2, textWidth, wrapped.height());
        shape.height = wrapped.height() + 4 + 2 * kGap;
    } else if (userMsg) {
        const int maxText = std::max(20, int(usable * kBubbleShare) - 2 * kPadX);
        const QRect wrapped = metrics.boundingRect(QRect(0, 0, maxText, 100000), Qt::TextWordWrap, text);
        const int boxWidth = wrapped.width() + 2 * kPadX;
        const int boxHeight = wrapped.height() + 2 * kPadY;
        shape.bubble = QRect(left + usable - boxWidth, top, boxWidth, boxHeight);
        shape.text = shape.bubble.adjusted(kPadX, kPadY, -kPadX, -kPadY);
        shape.height = boxHeight + 2 * kGap;
    } else {
        const QRect wrapped = metrics.boundingRect(QRect(0, 0, usable, 100000), Qt::TextWordWrap, text);
        shape.text = QRect(left, top, usable, wrapped.height());
        shape.height = wrapped.height() + 2 * kGap;
    }
    return shape;
}

void MessageDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const int kind = index.data(KindRole).toInt();
    const Shape s = shape(option, index);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (!s.bubble.isEmpty()) {
        painter->setPen(QPen(tokens_.line, 1));
        painter->setBrush(kind == kUser ? tokens_.panel : tokens_.panelRaised);
        painter->drawRoundedRect(s.bubble, radius_, radius_);
    } else if (bubbles_ == BubbleStyle::Lines && hasBubble(kind)) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(kind == kUser ? tokens_.idle : tokens_.line);
        painter->drawRoundedRect(QRect(s.text.right() + 7, s.text.top(), 3, s.text.height()), 1.5, 1.5);
    }
    QColor colour = tokens_.text;
    if (kind == kStep || kind == kHeard) colour = tokens_.muted;
    if (kind == kStepFailed || kind == kError) colour = tokens_.error;
    painter->setFont(s.font);
    painter->setPen(colour);
    painter->drawText(s.text, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, index.data(Qt::DisplayRole).toString());
    painter->restore();
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const Shape s = shape(option, index);
    const int width = option.rect.width() > 0 ? option.rect.width() : (view_ ? view_->viewport()->width() : 320);
    return {width, s.height};
}

} // namespace nexa
