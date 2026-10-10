#pragma once

#include <QColor>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>

namespace nexa {

// Read-only transcript. Finished messages are inserted once; the reply being streamed is
// re-rendered in place from Markdown at most 10 times per second. A reply can interleave text
// segments with tool steps ("✓ open_app — Launched Firefox · 12 ms").
class ChatView : public QTextBrowser {
    Q_OBJECT
public:
    explicit ChatView(QWidget *parent = nullptr);

    void addUserMessage(const QString &text);
    void beginAssistantMessage();
    void appendAssistantDelta(const QString &delta);
    void addStep(const QString &description, bool ok);
    void endAssistantMessage();
    void addNotice(const QString &text);
    void addError(const QString &text);
    void clearTranscript();

private:
    QTextCursor endCursor() const;
    // A cursor in an empty block at the end, with default formats (nothing inherits a list,
    // heading or code-block style from the previous message).
    QTextCursor paragraphCursor();
    void insertHeader(const QString &title, const QColor &color);
    void insertNote(const QString &text, const QColor &color, bool italic = true);
    void openSegment();
    void closeSegment();
    void removeStreamedRange();
    void renderStream();
    // Gives Markdown code blocks and inline code (from `from` to the end) a tinted background.
    void styleCode(int from);
    bool isAtBottom() const;
    void scrollToBottom();

    QTimer renderTimer_;
    QString streamText_;
    int streamStart_{-1};           // start of the open text segment, -1 when none
    bool inReply_{false};
};

} // namespace nexa
