#include "voice/pipeline/WakeWordDetector.h"

#include "voice/wake/WakePhrases.h"

#include <QRegularExpression>

#include <algorithm>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Longest first, so "hello" is not read as "he" + "llo" and "okay" not as "ok" + "ay".
const QStringList kGreetings{u"hello"_s, u"okay"_s, u"hey"_s, u"ok"_s, u"hi"_s, u"yo"_s};

struct Token {
    QString letters;    // lower-case letters (and apostrophes)
    qsizetype end{0};   // where the token ends in the transcript
    bool endsClause{false};
};

std::vector<Token> tokens(const QString &transcript) {
    static const QRegularExpression word(u"\\S+"_s);
    std::vector<Token> list;
    for (QRegularExpressionMatchIterator it = word.globalMatch(transcript); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString text = match.captured();
        QString letters;
        for (const QChar c : text.toLower()) {
            if (c.isLetter() || c == u'\'') letters.append(c);
        }
        list.push_back({.letters = letters,
                        .end = match.capturedEnd(),
                        .endsClause = QStringView(u".,!?;:").contains(text.back())});
    }
    return list;
}

int editDistance(const QString &a, const QString &b) {
    std::vector<int> row(size_t(b.size() + 1));
    for (qsizetype j = 0; j <= b.size(); ++j) row[size_t(j)] = int(j);
    for (qsizetype i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = int(i);
        for (qsizetype j = 1; j <= b.size(); ++j) {
            const int above = row[size_t(j)];
            row[size_t(j)] = std::min({above + 1, row[size_t(j - 1)] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[size_t(b.size())];
}

// What follows the name's last word, without the punctuation and spaces in between.
QString commandAfter(const QString &transcript, qsizetype end) {
    static const QRegularExpression leading(u"^[\\s\\p{P}]+"_s);
    QString command = transcript.mid(end);
    command.remove(leading);
    return command.trimmed();
}

} // namespace

WakeWordDetector::WakeWordDetector(const QString &name)
    : name_(name.trimmed().isEmpty() ? u"Nexa"_s : name.trimmed()),
      spellings_(WakePhrases::spellings(name_)) {
    nameWords_ = int(std::max<qsizetype>(1, spellings_.value(0).split(u' ', Qt::SkipEmptyParts).size()));
    nameKey_ = soundKey(spellings_.value(0));
}

QString WakeWordDetector::soundKey(const QString &word) {
    QString key;
    for (const QChar c : word.toLower()) {
        if (c >= u'a' && c <= u'z') key.append(c);
    }
    key.replace(u"ck"_s, u"k"_s).replace(u"ph"_s, u"f"_s).replace(u"x"_s, u"ks"_s);
    key.replace(u'q', u'k').replace(u'c', u'k').replace(u'z', u's');
    QString single;
    for (const QChar c : std::as_const(key)) {
        if (single.isEmpty() || single.back() != c) single.append(c);
    }
    QString sound = single.left(1);
    for (const QChar c : single.mid(1)) {
        if (!QStringView(u"aeiouy").contains(c)) sound.append(c);
    }
    return sound;
}

std::optional<WakeWordDetector::Match> WakeWordDetector::find(const QString &transcript) const {
    const std::vector<Token> words = tokens(transcript);
    // Sounds like the name: the same sounds, or one away — and then starting as the name does ("Max", "Rex" are not
    // "Nexa"), not shorter ("Nick"), and not adding a hard stop ("next" is "Nexa" and a t) — a recogniser's slips of the
    // name ("Nexus", "Nexer") stay in. A short name exactly.
    const auto soundsLikeName = [this](const QString &letters) {
        const QString key = soundKey(letters);
        if (key == nameKey_) return true;
        if (nameKey_.size() < 3 || key.isEmpty() || key.front() != nameKey_.front()) return false;
        if (key.size() < nameKey_.size() || editDistance(key, nameKey_) > 1) return false;
        if (key.size() > nameKey_.size()) {
            qsizetype at = 0;
            while (at < nameKey_.size() && key.at(at) == nameKey_.at(at)) ++at;
            if (QStringView(u"tdpbkg").contains(key.at(at))) return false;
        }
        return true;
    };
    // The words from `first` to `last` (inclusive), as the letters of one word.
    const auto joined = [&words](size_t first, size_t last) {
        QString letters;
        for (size_t k = first; k <= last; ++k) letters += words[k].letters;
        return letters;
    };

    for (size_t i = 0; i < words.size(); ++i) {
        const QString &letters = words[i].letters;
        // A greeting on its own, then the name's words — or one more when a recogniser split the name and left a
        // fragment of a letter or two ("Nex a", "Neck sa").
        if (kGreetings.contains(letters)) {
            for (size_t last = i + 1; last < words.size() && last <= i + size_t(nameWords_) + 1; ++last) {
                if (last > i + size_t(nameWords_) && words[last].letters.size() > 2) break;
                if (soundsLikeName(joined(i + 1, last))) {
                    return Match{.command = commandAfter(transcript, words[last].end), .firstWord = int(i), .greeted = true};
                }
            }
        }
        // A greeting glued to the name ("heynecks").
        for (const QString &greeting : kGreetings) {
            const QString rest = letters.mid(greeting.size());
            if (letters.startsWith(greeting) && rest.size() > 1 && nameWords_ == 1 && soundsLikeName(rest)) {
                return Match{.command = commandAfter(transcript, words[i].end), .firstWord = int(i), .greeted = true};
            }
        }
        // The name itself, where an address starts.
        const bool startsAddress = i == 0 || words[i - 1].endsClause;
        if (startsAddress && i + size_t(nameWords_) <= words.size()) {
            QStringList said;
            for (size_t k = i; k < i + size_t(nameWords_); ++k) said.append(words[k].letters);
            if (spellings_.contains(said.join(u' '))) {
                const size_t last = i + size_t(nameWords_) - 1;
                return Match{.command = commandAfter(transcript, words[last].end), .firstWord = int(i), .greeted = false};
            }
        }
    }
    return std::nullopt;
}

bool WakeWordDetector::mentions(const QString &text) const {
    const std::vector<Token> words = tokens(text);
    for (size_t i = 0; i + size_t(nameWords_) <= words.size(); ++i) {
        QStringList said;
        QString letters;
        for (size_t k = i; k < i + size_t(nameWords_); ++k) {
            said.append(words[k].letters);
            letters += words[k].letters;
        }
        if (spellings_.contains(said.join(u' ')) || soundKey(letters) == nameKey_) return true;
    }
    return false;
}

QString WakeWordDetector::commandOf(const QString &transcript) const {
    const std::optional<Match> match = find(transcript);
    return match && match->firstWord == 0 ? match->command : transcript.trimmed();
}

QString WakeWordDetector::recognizerPrompt() const {
    return u"Hey %1."_s.arg(name_);
}

} // namespace nexa
