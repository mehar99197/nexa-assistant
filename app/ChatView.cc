#include "app/ChatView.h"

#include "ui/NexaTheme.h"

#include <QAbstractTextDocumentLayout>
#include <QFontDatabase>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRenderIntervalMs = 100;
constexpr int kMessageSpacing = 22;
constexpr int kRoleProperty = QTextFormat::UserProperty + 11;
constexpr int kUserRole = 1;
constexpr int kMetaRole = 2;
constexpr int kStepRole = 3;
constexpr int kBubblePadX = 12;
constexpr int kBubblePadY = 7;
constexpr int kBubbleRadius = 12;

bool isCodeBlock(const QTextBlock &block) {
    const QTextBlockFormat format = block.blockFormat();
    return format.hasProperty(QTextFormat::BlockCodeFence) || format.nonBreakableLines();
}

// QTextCursor::insertMarkdown merges the first Markdown block into the (empty) block at the cursor
// and keeps that block's format: a leading code fence or heading loses its formatting, and a
// leading list gets an empty paragraph in front. Repair the first block from a parsed copy.
void insertMarkdown(QTextCursor cursor, const QString &markdown) {
    const int start = cursor.position();
    QTextDocument parsed;
    parsed.setMarkdown(markdown.left(2'000));  // the first block is all we need
    const QTextBlock firstParsed = parsed.firstBlock();
    cursor.insertMarkdown(markdown);

    QTextCursor fix(cursor.document());
    fix.setPosition(start);
    if (firstParsed.textList()) {
        if (fix.block().text().isEmpty() && fix.movePosition(QTextCursor::NextBlock)) fix.deletePreviousChar();
        return;
    }
    QTextBlockFormat format = firstParsed.blockFormat();
    format.clearProperty(QTextFormat::ObjectIndex);
    fix.setBlockFormat(format);
}

QString sourceLabel(const QString &source) {
    if (source == u"voice"_s) return u"voice"_s;
    if (source == u"phone"_s) return u"phone"_s;
    if (source == u"phone-voice"_s) return u"phone · voice"_s;
    return {};
}

} // namespace

ChatView::ChatView(QWidget *parent) : QTextBrowser(parent) {
    setOpenExternalLinks(true);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(16);
    renderTimer_.setSingleShot(true);
    renderTimer_.setInterval(kRenderIntervalMs);
    connect(&renderTimer_, &QTimer::timeout, this, &ChatView::renderStream);
    // The dark tokens until the stylesheet says otherwise.
    const ThemeTokens tokens = NexaTheme::tokens(ThemeScheme::Dark);
    userBubble_ = tokens.panel;
    meta_ = tokens.muted;
    accent_ = tokens.idle;
    error_ = tokens.error;
    code_ = tokens.panel;
}

QFont ChatView::monoFont(double scale) const {
    QFont mono = monoFamily_.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QFont(monoFamily_);
    mono.setPointSizeF(font().pointSizeF() * scale);
    return mono;
}

void ChatView::addUserMessage(const QString &text, const QString &source) {
    insertMeta(tr("You"), sourceLabel(source), true);
    QTextCursor cursor = paragraphCursor();
    QTextBlockFormat block;
    block.setAlignment(Qt::AlignRight);
    // A bubble takes at most about two thirds of the width; the rest wraps.
    block.setLeftMargin(std::max(0, int(viewport()->width() * 0.3)));
    block.setRightMargin(kBubblePadX + 4);
    block.setTopMargin(kBubblePadY + 2);
    block.setBottomMargin(kBubblePadY + 2);
    block.setProperty(kRoleProperty, kUserRole);
    cursor.setBlockFormat(block);
    QString oneBlock = text;
    oneBlock.replace(u'\n', QChar::LineSeparator);  // one block, one bubble
    cursor.insertText(oneBlock, QTextCharFormat());
    scrollToBottom();
}

void ChatView::beginAssistantMessage() {
    const bool follow = isAtBottom();
    route_.clear();
    actionCount_ = 0;
    fastMs_ = -1;
    replyHeader_ = insertMeta(u"Nexa"_s, {}, false);
    inReply_ = true;
    openSegment();
    if (follow) scrollToBottom();
}

void ChatView::setReplyRoute(const QString &route) {
    if (!inReply_ || route_ == route) return;
    route_ = route;
    rewriteReplyHeader();
}

void ChatView::appendAssistantDelta(const QString &delta) {
    if (!inReply_) return;
    if (streamStart_ < 0) openSegment();
    streamText_ += delta;
    if (!renderTimer_.isActive()) renderTimer_.start();
}

void ChatView::addAction(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs) {
    if (!inReply_) return;
    const bool follow = isAtBottom();
    ++actionCount_;
    closeSegment();
    if (route_ == u"fast"_s && fastMs_ < 0) {
        // Done without the model: what the tool reports is the answer.
        fastMs_ = elapsedMs;
        if (ok) {
            paragraphCursor().insertText(summary, QTextCharFormat());
        } else {
            insertNote(u"✗ "_s + summary, error_, false);
        }
        rewriteReplyHeader();
    } else {
        QTextCursor cursor = paragraphCursor();
        QTextBlockFormat block;
        block.setTopMargin(3);
        block.setBottomMargin(3);
        block.setProperty(kRoleProperty, kStepRole);
        cursor.setBlockFormat(block);
        QTextCharFormat mark;
        mark.setFont(monoFont(0.85));
        mark.setForeground(ok ? accent_ : error_);
        cursor.insertText(ok ? u"✓ "_s : u"✗ "_s, mark);
        QTextCharFormat name;
        name.setFont(monoFont(0.85));
        name.setForeground(palette().color(QPalette::Text));
        cursor.insertText(tool, name);
        QTextCharFormat rest;
        rest.setFont(monoFont(0.85));
        rest.setForeground(ok ? meta_ : error_);
        cursor.insertText(u" · %1 · %2 ms"_s.arg(summary).arg(elapsedMs), rest);
        if (route_ == u"agent"_s) rewriteReplyHeader();
    }
    openSegment();  // "…" while the model works on the result
    if (follow) scrollToBottom();
}

void ChatView::endAssistantMessage() {
    closeSegment();
    inReply_ = false;
    rewriteReplyHeader();
    replyHeader_ = -1;
}

void ChatView::openSegment() {
    streamStart_ = paragraphCursor().position();
    streamText_.clear();
    renderStream();  // shows the "…" placeholder
}

void ChatView::closeSegment() {
    if (streamStart_ < 0) return;
    renderTimer_.stop();
    if (streamText_.isEmpty()) {
        removeStreamedRange();  // drop the placeholder
    } else {
        renderStream();
    }
    streamStart_ = -1;
}

void ChatView::addNotice(const QString &text) {
    insertNote(text, meta_);
}

void ChatView::addError(const QString &text) {
    insertNote(text, error_);
}

void ChatView::clearTranscript() {
    renderTimer_.stop();
    streamStart_ = -1;
    replyHeader_ = -1;
    inReply_ = false;
    streamText_.clear();
    clear();
}

QTextCursor ChatView::endCursor() const {
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    return cursor;
}

QTextCursor ChatView::paragraphCursor() {
    QTextCursor cursor = endCursor();
    if (!cursor.block().text().isEmpty()) {
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        return cursor;
    }
    if (QTextList *list = cursor.currentList()) list->remove(cursor.block());
    cursor.setBlockFormat(QTextBlockFormat());
    cursor.setBlockCharFormat(QTextCharFormat());
    cursor.setCharFormat(QTextCharFormat());
    return cursor;
}

int ChatView::insertMeta(const QString &who, const QString &detail, bool right) {
    const bool first = document()->isEmpty();
    QTextCursor cursor = paragraphCursor();
    QTextBlockFormat block;
    block.setTopMargin(first ? 2 : kMessageSpacing);
    block.setBottomMargin(2);
    block.setAlignment(right ? Qt::AlignRight : Qt::AlignLeft);
    if (right) block.setRightMargin(4);
    block.setProperty(kRoleProperty, kMetaRole);
    cursor.setBlockFormat(block);
    const int position = cursor.block().position();
    writeMeta(cursor, who, detail);
    return position;
}

void ChatView::writeMeta(QTextCursor cursor, const QString &who, const QString &detail) {
    QTextCharFormat whoFormat;
    whoFormat.setFont(monoFont(0.78));
    whoFormat.setFontCapitalization(QFont::AllUppercase);
    whoFormat.setFontLetterSpacingType(QFont::PercentageSpacing);
    whoFormat.setFontLetterSpacing(110);
    whoFormat.setForeground(meta_);
    cursor.insertText(who, whoFormat);
    if (detail.isEmpty()) return;
    QTextCharFormat detailFormat;
    detailFormat.setFont(monoFont(0.78));
    detailFormat.setForeground(meta_);
    cursor.insertText(u" "_s + detail, detailFormat);
}

QString ChatView::replyDetail() const {
    if (route_ == u"fast"_s) return fastMs_ >= 0 ? tr("fast path · %1 ms").arg(fastMs_) : tr("fast path");
    if (route_ == u"agent"_s) return actionCount_ > 0 ? tr("agent · %n tool call(s)", nullptr, actionCount_) : tr("agent");
    return {};
}

void ChatView::rewriteReplyHeader() {
    if (replyHeader_ < 0) return;
    const QTextBlock block = document()->findBlock(replyHeader_);
    if (!block.isValid() || block.blockFormat().intProperty(kRoleProperty) != kMetaRole) return;
    const int before = document()->characterCount();
    QTextCursor cursor(block);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    writeMeta(cursor, u"Nexa"_s, replyDetail());
    // Everything after the header moved by the change in its length.
    const int shift = document()->characterCount() - before;
    if (streamStart_ > replyHeader_) streamStart_ += shift;
}

void ChatView::insertNote(const QString &text, const QColor &color, bool italic) {
    const bool follow = isAtBottom();
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontItalic(italic);
    paragraphCursor().insertText(text, format);
    if (follow) scrollToBottom();
}

void ChatView::removeStreamedRange() {
    QTextCursor cursor(document());
    cursor.setPosition(streamStart_);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
}

void ChatView::renderStream() {
    if (streamStart_ < 0) return;
    const bool follow = isAtBottom();
    removeStreamedRange();
    QTextCursor cursor = paragraphCursor();
    if (streamText_.isEmpty()) {
        QTextCharFormat placeholder;
        placeholder.setForeground(meta_);
        cursor.insertText(u"…"_s, placeholder);
    } else {
        insertMarkdown(cursor, streamText_);
        styleCode(streamStart_);
    }
    if (follow) scrollToBottom();
}

void ChatView::styleCode(int from) {
    // Collect first: changing formats splits and merges fragments, which invalidates iterators.
    QList<int> codeBlocks;
    QList<std::pair<int, int>> inlineCode;
    for (QTextBlock block = document()->findBlock(from); block.isValid(); block = block.next()) {
        if (isCodeBlock(block)) {
            codeBlocks.append(block.position());
            continue;
        }
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().fontFixedPitch()) {
                inlineCode.append({fragment.position(), fragment.position() + fragment.length()});
            }
        }
    }

    QTextCursor cursor(document());
    for (const int position : codeBlocks) {
        cursor.setPosition(position);
        QTextBlockFormat format = cursor.blockFormat();
        format.setBackground(code_);
        format.setLeftMargin(8);
        format.setRightMargin(8);
        cursor.setBlockFormat(format);
    }
    QTextCharFormat tint;
    tint.setBackground(code_);
    for (const auto &[start, end] : inlineCode) {
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(tint);
    }
}

void ChatView::paintEvent(QPaintEvent *event) {
    {
        // The bubbles go behind the text: the viewport's background is already painted, the
        // document's text comes after.
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(userBubble_);
        const QPointF offset(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        const QAbstractTextDocumentLayout *layout = document()->documentLayout();
        const QRectF visible = event->rect();
        for (QTextBlock block = document()->firstBlock(); block.isValid(); block = block.next()) {
            if (block.blockFormat().intProperty(kRoleProperty) != kUserRole) continue;
            const QRectF bounds = layout->blockBoundingRect(block).translated(offset);
            if (bounds.bottom() < visible.top() || bounds.top() > visible.bottom()) continue;
            const QTextLayout *lines = block.layout();
            QRectF text;
            for (int i = 0; i < lines->lineCount(); ++i) text |= lines->lineAt(i).naturalTextRect();
            if (text.isEmpty()) continue;
            text.translate(lines->position() + offset);
            painter.drawRoundedRect(text.adjusted(-kBubblePadX, -kBubblePadY, kBubblePadX, kBubblePadY),
                                    kBubbleRadius, kBubbleRadius);
        }
    }
    QTextBrowser::paintEvent(event);
}

bool ChatView::isAtBottom() const {
    const QScrollBar *bar = verticalScrollBar();
    return bar->value() >= bar->maximum() - 8;
}

void ChatView::scrollToBottom() {
    // The document is laid out lazily; scroll once the new size is known.
    QTimer::singleShot(0, this, [this] { verticalScrollBar()->setValue(verticalScrollBar()->maximum()); });
}

} // namespace nexa
