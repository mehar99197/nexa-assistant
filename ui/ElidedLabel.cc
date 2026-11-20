#include "ui/ElidedLabel.h"

#include <QEvent>
#include <QFontMetrics>
#include <QTextDocument>

using namespace Qt::StringLiterals;

namespace nexa {

ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent) {
    setTextFormat(Qt::RichText);
    setWordWrap(false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ElidedLabel::setElidedText(const QString &text, const QString &prefixHtml) {
    text_ = text;
    prefix_ = prefixHtml;
    refit();
    updateGeometry();
}

int ElidedLabel::prefixWidth() const {
    if (prefix_.isEmpty()) return 0;
    QTextDocument document;
    document.setDefaultFont(font());
    document.setDocumentMargin(0);
    document.setHtml(prefix_);
    return int(document.idealWidth()) + 1;
}

QSize ElidedLabel::sizeHint() const {
    const QMargins margins = contentsMargins();
    const QFontMetrics metrics(font());
    const int width = prefixWidth() + metrics.horizontalAdvance(text_) + margins.left() + margins.right() + 2;
    return {width, QLabel::sizeHint().height()};
}

QSize ElidedLabel::minimumSizeHint() const {
    // The prefix and the ellipsis: enough to say something is there, never the whole sentence.
    const QMargins margins = contentsMargins();
    const QFontMetrics metrics(font());
    const int width = prefixWidth() + metrics.horizontalAdvance(u"…"_s) * 3 + margins.left() + margins.right();
    return {width, QLabel::minimumSizeHint().height()};
}

void ElidedLabel::resizeEvent(QResizeEvent *event) {
    QLabel::resizeEvent(event);
    refit();
}

void ElidedLabel::changeEvent(QEvent *event) {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) refit();
}

void ElidedLabel::refit() {
    const QFontMetrics metrics(font());
    const int room = std::max(0, contentsRect().width() - prefixWidth() - 2);
    const QString shown = metrics.elidedText(text_, Qt::ElideRight, room);
    elided_ = shown != text_;
    setToolTip(elided_ ? text_ : QString());
    QLabel::setText(prefix_ + shown.toHtmlEscaped());
}

} // namespace nexa
