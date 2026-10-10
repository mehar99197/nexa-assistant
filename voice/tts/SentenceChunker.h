#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

namespace nexa {

// Turns a streamed Markdown reply into speakable sentences as soon as each one is complete, so
// speech starts with the first sentence instead of after the whole reply (pure). Code blocks are
// skipped, Markdown markup is removed, links and URLs are reduced to their text or host, and each
// heading, list item and table row is spoken as a sentence of its own.
class SentenceChunker {
public:
    // Returns the sentences completed by `delta` (possibly none).
    QStringList feed(QStringView delta);
    // Returns whatever is left; the chunker is empty afterwards.
    QStringList finish();
    void reset();

    // One line (or the rest of one) of Markdown → plain text. `continuation`: the text continues a
    // line whose beginning was already spoken, so it cannot start with a heading or list marker.
    static QString speakable(QStringView markdown, bool continuation = false);

private:
    void takeLine(const QString &line, QStringList &out);
    void takePartial(QStringList &out);
    void emitSentence(QStringView text, QStringList &out);

    QString line_;              // the current, incomplete line
    bool continuation_{false};  // line_ continues a line that was partly spoken already
    bool inFence_{false};       // inside a ``` code block
};

} // namespace nexa
