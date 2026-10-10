#include "app/ChatView.h"

#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRenderIntervalMs = 100;
constexpr int kMessageSpacing = 18;

QColor assistantColor() { return QColor(0x2b, 0xb3, 0xa0); }
QColor errorColor() { return QColor(0xe5, 0x53, 0x4b); }

// Tint for code: a step lighter than the base colour on dark themes, darker on light ones.
QColor codeBackground(const QPalette &palette) {
    const QColor base = palette.color(QPalette::Base);
    return base.lightness() < 128 ? base.lighter(150) : base.darker(106);
}

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

} // namespace

ChatView::ChatView(QWidget *parent) : QTextBrowser(parent) {
    setOpenExternalLinks(true);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(12);
    renderTimer_.setSingleShot(true);
    renderTimer_.setInterval(kRenderIntervalMs);
    connect(&renderTimer_, &QTimer::timeout, this, &ChatView::renderStream);
}

void ChatView::addUserMessage(const QString &text) {
    insertHeader(tr("You"), palette().color(QPalette::Highlight));
    paragraphCursor().insertText(text, QTextCharFormat());
    scrollToBottom();
}

void ChatView::beginAssistantMessage() {
    const bool follow = isAtBottom();
    insertHeader(u"Nexa"_s, assistantColor());
    inReply_ = true;
    openSegment();
    if (follow) scrollToBottom();
}

void ChatView::appendAssistantDelta(const QString &delta) {
    if (!inReply_) return;
    if (streamStart_ < 0) openSegment();
    streamText_ += delta;
    if (!renderTimer_.isActive()) renderTimer_.start();
}

void ChatView::addStep(const QString &description, bool ok) {
    if (!inReply_) return;
    closeSegment();
    insertNote((ok ? u"✓ "_s : u"✗ "_s) + description, ok ? assistantColor() : errorColor(), false);
    openSegment();  // "…" while the model works on the result
}

void ChatView::endAssistantMessage() {
    closeSegment();
    inReply_ = false;
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
    insertNote(text, palette().color(QPalette::PlaceholderText));
}

void ChatView::addError(const QString &text) {
    insertNote(text, errorColor());
}

void ChatView::clearTranscript() {
    renderTimer_.stop();
    streamStart_ = -1;
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

void ChatView::insertHeader(const QString &title, const QColor &color) {
    const bool first = document()->isEmpty();
    QTextCursor cursor = paragraphCursor();
    QTextBlockFormat block;
    block.setTopMargin(first ? 0 : kMessageSpacing);
    cursor.setBlockFormat(block);
    QTextCharFormat format;
    format.setFontWeight(QFont::Bold);
    format.setForeground(color);
    cursor.insertText(title, format);
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
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
        placeholder.setForeground(palette().color(QPalette::PlaceholderText));
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

    const QColor background = codeBackground(palette());
    QTextCursor cursor(document());
    for (const int position : codeBlocks) {
        cursor.setPosition(position);
        QTextBlockFormat format = cursor.blockFormat();
        format.setBackground(background);
        format.setLeftMargin(6);
        format.setRightMargin(6);
        cursor.setBlockFormat(format);
    }
    QTextCharFormat tint;
    tint.setBackground(background);
    for (const auto &[start, end] : inlineCode) {
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(tint);
    }
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
