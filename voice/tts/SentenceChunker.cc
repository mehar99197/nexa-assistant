#include "voice/tts/SentenceChunker.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// A partial line longer than this is spoken up to its last comma or space, so a long sentence
// does not hold back the speech.
constexpr qsizetype kMaxPartial = 320;
constexpr qsizetype kMinSplit = 80;

bool isTerminator(QChar c) {
    return c == u'.' || c == u'!' || c == u'?' || c == u'…' || c == u'。' || c == u'！' || c == u'？';
}

// Characters that may follow a terminator and still belong to the sentence: ."  !)  ?**
bool isCloser(QChar c) {
    return isTerminator(c) || QStringView(u"\"')]}*_`”’»").contains(c);
}

// Characters to skip when looking at what starts the next sentence.
bool isOpener(QChar c) {
    return c.isSpace() || QStringView(u"*_`#>-\"'“‘«([").contains(c);
}

bool isAbbreviation(QStringView text, qsizetype dot) {
    static const QSet<QString> known{u"mr"_s, u"mrs"_s, u"ms"_s, u"dr"_s, u"prof"_s, u"sr"_s, u"jr"_s,
                                     u"st"_s, u"vs"_s, u"etc"_s, u"e.g"_s, u"i.e"_s, u"no"_s, u"fig"_s,
                                     u"approx"_s, u"inc"_s, u"ltd"_s, u"co"_s, u"mt"_s, u"cf"_s};
    qsizetype start = dot;
    while (start > 0 && (text[start - 1].isLetter() || text[start - 1] == u'.')) --start;
    const QStringView word = text.sliced(start, dot - start);
    if (word.size() == 1 && word[0].isUpper()) return true;     // an initial: "J. Smith"
    return known.contains(word.toString().toLower());
}

// "1." / "12." at the start: list numbering, not a sentence.
bool isListNumber(QStringView text, qsizetype dot) {
    const QStringView before = text.first(dot).trimmed();
    return !before.isEmpty() && before.size() <= 3
        && std::all_of(before.begin(), before.end(), [](QChar c) { return c.isDigit(); });
}

// End (exclusive) of the first complete sentence in `text`, or -1. Unless `final`, a sentence
// only counts once the text after it shows that it really ended (not "e.g. this").
qsizetype sentenceEnd(QStringView text, bool final) {
    const qsizetype size = text.size();
    for (qsizetype i = 0; i < size; ++i) {
        if (!isTerminator(text[i])) continue;
        if (text[i] == u'.' && isListNumber(text, i)) continue;
        qsizetype end = i + 1;
        while (end < size && isCloser(text[end])) ++end;
        if (end == size) return final ? end : -1;
        if (!text[end].isSpace()) continue;            // 3.14, example.com, v1.2
        qsizetype next = end;
        while (next < size && isOpener(text[next])) ++next;
        if (next == size) return final ? end : -1;
        if (text[next].isLower()) continue;            // "e.g. this", "approx. three"
        if (text[i] == u'.' && isAbbreviation(text, i)) continue;
        return end;
    }
    return final && !text.trimmed().isEmpty() ? size : -1;
}

// Where to cut a long partial line: after its last clause separator, else its last space.
qsizetype softBreak(QStringView text) {
    const QStringView window = text.first(kMaxPartial);
    for (const QStringView separator : {u", "_sv, u"; "_sv, u": "_sv, u" – "_sv, u" — "_sv}) {
        const qsizetype at = window.lastIndexOf(separator);
        if (at >= kMinSplit) return at + 1;
    }
    const qsizetype space = window.lastIndexOf(u' ');
    return space >= kMinSplit ? space : kMaxPartial;
}

bool isFence(QStringView line) {
    const QStringView trimmed = line.trimmed();
    return trimmed.startsWith(u"```") || trimmed.startsWith(u"~~~");
}

} // namespace

QStringList SentenceChunker::feed(QStringView delta) {
    QStringList out;
    line_.append(delta);
    qsizetype newline;
    while ((newline = line_.indexOf(u'\n')) >= 0) {
        const QString line = line_.left(newline);
        line_.remove(0, newline + 1);
        takeLine(line, out);
    }
    takePartial(out);
    return out;
}

QStringList SentenceChunker::finish() {
    QStringList out;
    if (!line_.isEmpty()) takeLine(std::exchange(line_, {}), out);
    reset();
    return out;
}

void SentenceChunker::reset() {
    line_.clear();
    continuation_ = false;
    inFence_ = false;
}

void SentenceChunker::takeLine(const QString &line, QStringList &out) {
    const bool continuation = std::exchange(continuation_, false);
    if (isFence(line) && !continuation) {
        inFence_ = !inFence_;
        return;
    }
    if (inFence_) return;
    // Headings, list items and table rows are cleaned as a whole line first.
    QString text = speakable(line, continuation);
    for (qsizetype end; !text.isEmpty() && (end = sentenceEnd(text, true)) > 0;) {
        emitSentence(QStringView(text).first(end), out);
        text = text.sliced(end).trimmed();
    }
}

void SentenceChunker::takePartial(QStringList &out) {
    if (inFence_ || line_.isEmpty()) return;
    const QStringView start = QStringView(line_).trimmed();
    // Possibly the start of a code fence or a table row: wait for the whole line.
    if (!continuation_ && (start.startsWith(u'`') || start.startsWith(u'~') || start.startsWith(u'|'))) return;

    for (;;) {
        qsizetype end = sentenceEnd(line_, false);
        if (end < 0 && line_.size() > kMaxPartial) end = softBreak(line_);
        if (end < 0) return;
        const QString sentence = line_.left(end);
        line_.remove(0, end);
        while (!line_.isEmpty() && line_.front().isSpace()) line_.remove(0, 1);
        emitSentence(speakable(sentence, std::exchange(continuation_, true)), out);
    }
}

void SentenceChunker::emitSentence(QStringView text, QStringList &out) {
    const QString sentence = text.trimmed().toString();
    if (!sentence.isEmpty()) out.append(sentence);
}

QString SentenceChunker::speakable(QStringView markdown, bool continuation) {
    static const QRegularExpression heading(u"^#{1,6}\\s+"_s);
    static const QRegularExpression quote(u"^(?:>\\s?)+"_s);
    static const QRegularExpression bullet(u"^[-*+]\\s+(?:\\[[ xX]\\]\\s+)?"_s);
    static const QRegularExpression numbered(u"^\\d{1,3}[.)]\\s+"_s);
    static const QRegularExpression rule(u"^(?:[-*_]\\s*){3,}$"_s);
    static const QRegularExpression tableSeparator(u"^\\|?[\\s:|-]+\\|?$"_s);
    static const QRegularExpression image(u"!\\[([^\\]]*)\\]\\([^)]*\\)"_s);
    static const QRegularExpression link(u"\\[([^\\]]+)\\]\\([^)]*\\)"_s);
    static const QRegularExpression autolink(u"<(https?://[^>\\s]+)>"_s);
    static const QRegularExpression url(u"https?://(?:www\\.)?([^/\\s)\\]>]+)[^\\s)\\]>]*"_s);
    static const QRegularExpression html(u"</?[A-Za-z][^>]*>"_s);
    static const QRegularExpression strongOrStrike(u"\\*\\*|__|~~"_s);
    static const QRegularExpression edgeEmphasis(u"(?<!\\w)[*_]+|[*_]+(?!\\w)"_s);

    QString text = markdown.trimmed().toString();
    if (!continuation) {
        if (rule.match(text).hasMatch()) return {};
        text.remove(heading).remove(quote);
        text.remove(bullet).remove(numbered);
        if (text.startsWith(u'|')) {
            if (tableSeparator.match(text).hasMatch()) return {};
            QStringList cells;
            for (const QString &cell : text.split(u'|', Qt::SkipEmptyParts)) {
                if (!cell.trimmed().isEmpty()) cells.append(cell.trimmed());
            }
            text = cells.join(u", "_s);
        }
    }
    text.replace(image, u"\\1"_s);
    text.replace(link, u"\\1"_s);
    text.replace(autolink, u"\\1"_s);
    text.replace(url, u"\\1"_s);
    text.replace(html, u" "_s);
    text.remove(u'`');
    text.remove(strongOrStrike);
    text.remove(edgeEmphasis);
    return text.simplified();
}

} // namespace nexa
