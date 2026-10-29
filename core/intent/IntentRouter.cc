#include "core/intent/IntentRouter.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Wake words — "Nexa" and the name the user chose — and politeness that carry no meaning for matching
// ("hey jarvis, please open …").
const QRegularExpression &leadingFiller(const QString &assistantName) {
    thread_local QString builtFor;
    thread_local QRegularExpression expression;
    if (builtFor != assistantName || !expression.isValid() || expression.pattern().isEmpty()) {
        QStringList names{u"nexa"_s};
        const QString name = assistantName.simplified().toLower();
        if (!name.isEmpty() && name != u"nexa"_s) {
            names.prepend(QRegularExpression::escape(name).replace(u"\\ "_s, u"\\s+"_s));
        }
        const QString wake = u"(?:%1)"_s.arg(names.join(u'|'));
        expression = QRegularExpression(
            u"^(?:(?:hey|hi|ok|okay|yo)\\s+%1|%1|please|pls|kindly|can you|could you|would you|will you)"
            "(?:[\\s,:!.]+|$)"_s.arg(wake),
            QRegularExpression::CaseInsensitiveOption);
        builtFor = assistantName;
    }
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

IntentInput IntentRouter::prepare(const QString &text, const QString &assistantName) {
    const QRegularExpression &leading = leadingFiller(assistantName);
    QString raw = text.trimmed();
    for (bool changed = true; changed;) {
        const QString before = raw;
        raw.remove(leading);
        raw = raw.trimmed();
        changed = raw != before;
    }

    QString stripped = raw;
    for (bool changed = true; changed;) {
        const QString before = stripped;
        stripped.remove(leading);
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
    const IntentInput input = prepare(text, assistantName_);
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
