#include "core/intent/IntentRouter.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Wake words and politeness that carry no meaning for matching ("hey nexa, please open …").
const QRegularExpression &leadingFiller() {
    static const QRegularExpression expression(
        u"^(?:(?:hey|hi|ok|okay|yo)\\s+nexa|nexa|please|pls|kindly|can you|could you|would you|will you)"
        "(?:[\\s,:!.]+|$)"_s,
        QRegularExpression::CaseInsensitiveOption);
    return expression;
}

const QRegularExpression &trailingFiller() {
    static const QRegularExpression expression(u"[\\s,]+(?:please|pls|for me|now|thanks|thank you)[\\s.!?]*$"_s,
                                               QRegularExpression::CaseInsensitiveOption);
    return expression;
}

} // namespace

IntentRouter::IntentRouter(ToolAvailable available) : available_(std::move(available)) {}

void IntentRouter::add(std::unique_ptr<IIntentHandler> handler) {
    handlers_.push_back(std::move(handler));
}

IntentInput IntentRouter::prepare(const QString &text) {
    QString raw = text.trimmed();
    for (bool changed = true; changed;) {
        const QString before = raw;
        raw.remove(leadingFiller());
        raw = raw.trimmed();
        changed = raw != before;
    }

    QString stripped = raw;
    for (bool changed = true; changed;) {
        const QString before = stripped;
        stripped.remove(leadingFiller());
        stripped.remove(trailingFiller());
        stripped = stripped.trimmed();
        changed = stripped != before;
    }

    QString normalized = stripped.toLower().simplified();
    while (!normalized.isEmpty() && QStringView(u".!?").contains(normalized.back())) normalized.chop(1);
    return IntentInput{stripped, normalized.trimmed(), raw};
}

bool IntentRouter::isCompound(const QString &normalized) {
    static const QRegularExpression expression(u"\\b(?:and|then|after that|also|afterwards)\\b|[,;&]"_s);
    return expression.match(normalized).hasMatch();
}

std::optional<IntentMatch> IntentRouter::route(const QString &text) const {
    const IntentInput input = prepare(text);
    if (input.normalized.isEmpty()) return std::nullopt;
    for (const auto &handler : handlers_) {
        std::optional<IntentMatch> match = handler->match(input);
        if (!match) continue;
        if (!match->panic && !match->verbatimPayload && isCompound(input.normalized)) return std::nullopt;
        if (!match->toolName.isEmpty() && available_ && !available_(match->toolName)) continue;
        return match;
    }
    return std::nullopt;
}

} // namespace nexa
