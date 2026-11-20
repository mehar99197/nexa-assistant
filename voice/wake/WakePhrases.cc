#include "voice/wake/WakePhrases.h"

#include "voice/wake/WordPieces.h"

#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kMostSoundAlikes = 3;
const QStringList kGreetings{u"hey"_s, u"ok"_s, u"okay"_s, u"hi"_s};

// Lower case, hyphens as spaces, only letters, spaces and apostrophes, single spaces.
QString cleaned(const QString &name) {
    QString text;
    for (const QChar c : name.toLower()) {
        if (c.isLetter() || c == u'\'') {
            text.append(c);
        } else if (c.isSpace() || c == u'-') {
            text.append(u' ');
        }
    }
    return text.simplified();
}

} // namespace

Result<void> WakePhrases::check(const QString &name) {
    const QString trimmed = name.trimmed();
    const auto refuse = [](const QString &message) { return Error{ErrorCode::InvalidArgument, message}; };
    if (trimmed.size() < 2 || trimmed.size() > 24) return refuse(u"Use 2–24 characters."_s);
    if (std::count_if(trimmed.cbegin(), trimmed.cend(), [](QChar c) { return c.isLetter(); }) < 2) {
        return refuse(u"Use at least two letters."_s);
    }
    for (const QChar c : trimmed) {
        if (c.isLetter() && c.unicode() > 0x7F) {
            return refuse(u"Use Latin letters (a–z) so the name can be spotted instantly."_s);
        }
    }
    for (const QChar c : trimmed) {
        if (!c.isLetter() && !c.isSpace() && c != u'-' && c != u'\'') {
            return refuse(u"Use letters, spaces, hyphens and apostrophes only."_s);
        }
    }
    if (cleaned(trimmed).split(u' ', Qt::SkipEmptyParts).size() > 3) return refuse(u"Use at most three words."_s);
    return {};
}

QStringList WakePhrases::spellings(const QString &name) {
    const QString base = cleaned(name);
    QStringList spellings{base};
    // Spellings an English speaker reads the same way; each rule is applied on its own to the name.
    static const QRegularExpression hardC(u"c(?=[aou])"_s);
    const QStringList variants{
        QString(base).replace(u"x"_s, u"cks"_s),
        QString(base).replace(u"x"_s, u"ks"_s),
        QString(base).replace(u"ph"_s, u"f"_s),
        QString(base).replace(u"q"_s, u"k"_s),
        QString(base).replace(hardC, u"k"_s),
    };
    for (const QString &variant : variants) {
        if (spellings.size() > kMostSoundAlikes) break;
        if (!spellings.contains(variant)) spellings.append(variant);
    }
    return spellings;
}

Result<QString> WakePhrases::keywordLines(const QString &name, const WordPieces &pieces) {
    if (const Result<void> checked = check(name); !checked) return checked.error();
    QStringList lines;
    for (const QString &spelling : spellings(name)) {
        QStringList forms;
        for (const QString &greeting : kGreetings) forms.append(greeting + u' ' + spelling);
        forms.append(spelling);
        for (const QString &form : forms) {
            // The label names what was heard; a space in it would end the line for the spotter.
            lines.append(pieces.split(form).join(u' ') + u" @"_s + form.toUpper().replace(u' ', u'_'));
        }
    }
    return lines.join(u'\n');
}

bool WakePhrases::isGreeted(const QString &label) {
    return std::any_of(kGreetings.cbegin(), kGreetings.cend(),
                       [&label](const QString &greeting) { return label.startsWith(greeting.toUpper() + u'_'); });
}

WakeSensitivity WakePhrases::sensitivityFrom(const QString &value) {
    if (value == u"low"_s) return WakeSensitivity::Low;
    if (value == u"high"_s) return WakeSensitivity::High;
    return WakeSensitivity::Normal;
}

SpotterTuning WakePhrases::tuning(WakeSensitivity sensitivity) {
    switch (sensitivity) {
    case WakeSensitivity::Low: return {.boost = 1.0f, .threshold = 0.25f, .sileroThreshold = 0.6f, .startMarginDb = 16.0};
    case WakeSensitivity::High: return {.boost = 3.0f, .threshold = 0.05f, .sileroThreshold = 0.4f, .startMarginDb = 10.0};
    case WakeSensitivity::Normal: break;
    }
    return {.boost = 2.0f, .threshold = 0.10f, .sileroThreshold = 0.5f, .startMarginDb = 13.0};
}

} // namespace nexa
