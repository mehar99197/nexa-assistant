#include "core/intent/MemoryIntents.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// The words as said, without the full stop at the end.
QString cleaned(QString text) {
    text = text.trimmed();
    while (!text.isEmpty() && QStringView(u".!").contains(text.back())) text.chop(1);
    return text.trimmed();
}

} // namespace

std::optional<IntentMatch> MemoryIntent::match(const IntentInput &input) const {
    static const QRegularExpression whatAboutMe(
        u"^(?:what (?:do|did) you (?:remember|know) about me|what have you (?:remembered|saved|learned|learnt) about me"
        "|what do you remember)$"_s);
    if (whatAboutMe.match(input.normalized).hasMatch()) {
        return IntentMatch{.toolName = u"search_memory"_s, .arguments = QJsonObject{}};
    }

    const QString said = input.raw.trimmed();
    if (said.endsWith(u'?')) return std::nullopt;     // "remember that time we met?" is a question

    static const QRegularExpression remember(
        u"^(?:(?:remember|keep in mind|don'?t forget|do not forget)\\s+that\\s+(.+)"
        "|remember\\s*:\\s*(.+)"
        "|remember\\s+((?:my|our|i'm|i am)\\s+.+))$"_s,
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    // A moment, not a fact: "remember that time we went to Lahore".
    static const QRegularExpression event(u"^(?:time|day|night|week|month|year|moment|trip|song|movie|film|place)\\b"_s,
                                          QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = remember.match(said); m.hasMatch()) {
        const QString fact = cleaned(m.captured(1) + m.captured(2) + m.captured(3));   // one of the three matched
        if (fact.isEmpty() || event.match(fact).hasMatch()) return std::nullopt;
        return IntentMatch{.toolName = u"remember"_s,
                           .arguments = QJsonObject{{u"text"_s, fact}, {u"origin"_s, u"asked"_s}},
                           .verbatimPayload = true};
    }

    static const QRegularExpression forget(
        u"^forget\\s+(?:that|about|what\\s+i\\s+(?:said|told\\s+you)\\s+about)\\s+(.+)$"_s,
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    // "forget about it", "forget about everything": a shrug, not a memory to remove.
    static const QRegularExpression vague(u"^(?:it|that|this|them|him|her|everything|all|all of it|all that|me|you)$"_s,
                                          QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = forget.match(said); m.hasMatch()) {
        const QString what = cleaned(m.captured(1));
        if (what.isEmpty() || vague.match(what).hasMatch()) return std::nullopt;
        return IntentMatch{.toolName = u"forget"_s, .arguments = QJsonObject{{u"query"_s, what}}, .verbatimPayload = true};
    }
    return std::nullopt;
}

} // namespace nexa
