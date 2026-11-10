#pragma once

#include <QColor>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>

namespace nexa {

// Read-only transcript in the design's shapes: what you said sits in a bubble on the right under
// a small "YOU" (with where it came from — voice, phone); Nexa's answer sits on the left under
// "NEXA fast path · 2 ms" or "NEXA agent · 2 tool calls". Finished messages are inserted once; the
// reply being streamed is re-rendered in place from Markdown at most 10 times per second. The
// bubbles are painted behind the document, so selecting and copying work as in any text view.
// Colours are Q_PROPERTYs set from theme.qss.
class ChatView : public QTextBrowser {
    Q_OBJECT
    Q_PROPERTY(QColor userBubbleColor READ userBubbleColor WRITE setUserBubbleColor)
    Q_PROPERTY(QColor metaColor READ metaColor WRITE setMetaColor)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
    Q_PROPERTY(QColor errorColor READ errorColor WRITE setErrorColor)
    Q_PROPERTY(QColor codeColor READ codeColor WRITE setCodeColor)
    Q_PROPERTY(QString monoFamily READ monoFamily WRITE setMonoFamily)
    Q_PROPERTY(int bubbleStyle READ bubbleStyle WRITE setBubbleStyle)     // 0 bubbles, 1 lines, 2 cards
    Q_PROPERTY(int bubbleRadius READ bubbleRadius WRITE setBubbleRadius)
public:
    explicit ChatView(QWidget *parent = nullptr);

    // `attachments`: the line under the words naming the files given with them ("📎 notes.txt (3 KB)"), if any.
    void addUserMessage(const QString &text, const QString &source, const QString &attachments = QString());
    void beginAssistantMessage();
    void setReplyRoute(const QString &route);       // "fast" (no model) or "agent"
    void setReplyVia(const QString &via);           // a fallback provider answered the reply
    void appendAssistantDelta(const QString &delta);
    // A tool ran. On the fast path its summary is the answer; in an agent reply it is a step line.
    void addAction(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);
    void endAssistantMessage();
    void addNotice(const QString &text);
    void addError(const QString &text);
    void clearTranscript();

    QColor userBubbleColor() const { return userBubble_; }
    QColor metaColor() const { return meta_; }
    QColor accentColor() const { return accent_; }
    QColor errorColor() const { return error_; }
    QColor codeColor() const { return code_; }
    QString monoFamily() const { return monoFamily_; }
    void setUserBubbleColor(const QColor &c) { userBubble_ = c; viewport()->update(); }
    void setMetaColor(const QColor &c) { meta_ = c; }
    void setAccentColor(const QColor &c) { accent_ = c; }
    void setErrorColor(const QColor &c) { error_ = c; }
    void setCodeColor(const QColor &c) { code_ = c; }
    void setMonoFamily(const QString &family) { monoFamily_ = family; }
    int bubbleStyle() const { return bubbleStyle_; }
    void setBubbleStyle(int style) { bubbleStyle_ = style; viewport()->update(); }
    int bubbleRadius() const { return bubbleRadius_; }
    void setBubbleRadius(int radius) { bubbleRadius_ = radius; viewport()->update(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QTextCursor endCursor() const;
    // A cursor in an empty block at the end, with default formats (nothing inherits a list,
    // heading or code-block style from the previous message).
    QTextCursor paragraphCursor();
    // "YOU  voice" on the right, "NEXA  agent · 2 tool calls" on the left; returns the block's position.
    int insertMeta(const QString &who, const QString &detail, bool right);
    void writeMeta(QTextCursor cursor, const QString &who, const QString &detail);
    void rewriteReplyHeader();
    QString replyDetail() const;
    void insertNote(const QString &text, const QColor &color, bool italic = true);
    void openSegment();
    void closeSegment();
    void removeStreamedRange();
    void renderStream();
    // Gives Markdown code blocks and inline code (from `from` to the end) a tinted background.
    void styleCode(int from);
    bool isAtBottom() const;
    void scrollToBottom();
    QFont monoFont(double scale) const;

    QTimer renderTimer_;
    QString streamText_;
    int streamStart_{-1};           // start of the open text segment, -1 when none
    bool inReply_{false};
    int replyHeader_{-1};           // position of the reply's meta line, rewritten as the route becomes known
    QString route_;
    QString via_;
    int actionCount_{0};
    qint64 fastMs_{-1};
    QColor userBubble_;
    QColor meta_;
    QColor accent_;
    QColor error_;
    QColor code_;
    QString monoFamily_;
    int bubbleStyle_{0};
    int bubbleRadius_{12};
};

} // namespace nexa
