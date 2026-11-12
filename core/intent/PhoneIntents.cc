#include "core/intent/PhoneIntents.h"

#include <QJsonObject>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// "on my phone", "from the phone", "using my mobile".
const QString kOnPhone = u"\\s+(?:on|from|using|with|in)\\s+(?:my|the)\\s+(?:phone|mobile|android)"_s;
// "my phone", "the phone's", "phone".
const QString kPhone = u"(?:(?:my|the)\\s+)?(?:phone|mobile)(?:'s)?"_s;

QRegularExpressionMatch whole(const QString &pattern, const QString &text, bool caseInsensitive = false) {
    const QRegularExpression expression(u"^(?:"_s + pattern + u")$"_s,
                                        caseInsensitive ? QRegularExpression::CaseInsensitiveOption
                                                        : QRegularExpression::NoPatternOption);
    return expression.match(text);
}

IntentMatch tool(const QString &name, const QJsonObject &arguments) {
    return IntentMatch{.toolName = name, .arguments = arguments};
}

// A picture of the phone for the model to answer about — or its screen as text, for a model without eyes.
IntentMatch askAfter(const QString &name, const QString &prompt) {
    return IntentMatch{.toolName = name, .forwardToAgent = true, .agentPrompt = prompt,
                       .textToolName = name == u"android_screenshot"_s ? u"android_snapshot"_s : QString()};
}

std::optional<IntentMatch> screen(const IntentInput &input) {
    const QString &text = input.normalized;
    if (whole(u"what(?:'s| is)\\s+on\\s+"_s + kPhone + u"(?:\\s+screen)?|what(?:'s| is)\\s+on\\s+"_s + kPhone
                  + u"\\s+right now"_s,
              text)
            .hasMatch()) {
        return askAfter(u"android_screenshot"_s, input.text);
    }
    if (whole(u"(?:see|look at|check|show me|describe)\\s+"_s + kPhone + u"(?:\\s+screen)?"_s
                  + u"|(?:take\\s+)?(?:a\\s+)?screen\\s?shot\\s+(?:of\\s+)?"_s + kPhone + u"(?:\\s+screen)?"_s
                  + u"|"_s + kPhone + u"\\s+screen\\s?shot"_s,
              text)
            .hasMatch()) {
        return askAfter(u"android_screenshot"_s, u"Describe what is on my phone's screen."_s);
    }
    return std::nullopt;
}

std::optional<IntentMatch> openApp(const IntentInput &input) {
    static const QString app = u"(?:the\\s+|my\\s+)?(.+?)(?:\\s+app)?"_s;
    const QString &text = input.normalized;
    QRegularExpressionMatch m = whole(u"(?:on|in)\\s+"_s + kPhone + u"\\s*,?\\s+(?:open|launch|start)\\s+"_s + app, text);
    if (!m.hasMatch()) m = whole(u"(?:open|launch|start)\\s+"_s + app + kOnPhone, text);
    if (!m.hasMatch()) return std::nullopt;
    return tool(u"android_open_app"_s, {{u"name"_s, m.captured(1)}});
}

std::optional<IntentMatch> volume(const IntentInput &input) {
    const QString &text = input.normalized;
    static const QString percent = u"(\\d{1,3})\\s*(?:%|percent)?"_s;
    if (QRegularExpressionMatch m = whole(u"(?:set\\s+)?"_s + kPhone + u"\\s+volume\\s+(?:to\\s+|at\\s+)?"_s + percent
                                              + u"|(?:set\\s+)?(?:the\\s+)?volume\\s+(?:to\\s+|at\\s+)?"_s + percent + kOnPhone,
                                          text);
        m.hasMatch()) {
        const QString level = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        return tool(u"android_volume"_s, {{u"level"_s, qMin(level.toInt(), 100)}});
    }
    if (QRegularExpressionMatch m = whole(kPhone + u"\\s+volume\\s+(up|down)|turn\\s+(up|down)\\s+(?:the\\s+)?volume"_s + kOnPhone
                                              + u"|turn\\s+"_s + kPhone + u"\\s+(up|down)"_s,
                                          text);
        m.hasMatch()) {
        const bool up = m.captured(1) == u"up" || m.captured(2) == u"up" || m.captured(3) == u"up";
        return tool(u"android_volume"_s, {{u"change"_s, up ? 10 : -10}});
    }
    return std::nullopt;
}

std::optional<IntentMatch> media(const IntentInput &input) {
    const QString &text = input.normalized;
    static const QString thing = u"(?:\\s+(?:the\\s+)?(?:music|song|track|video|it))?"_s;
    QString action;
    if (QRegularExpressionMatch m = whole(u"(play|resume|pause|stop)"_s + thing + kOnPhone + u"|(play|resume|pause|stop)\\s+"_s
                                              + kPhone + u"|"_s + kPhone + u"\\s+(play|resume|pause|stop)"_s,
                                          text);
        m.hasMatch()) {
        action = m.captured(1) + m.captured(2) + m.captured(3);
        if (action == u"resume") action = u"play"_s;
    } else if (whole(u"(?:skip|next)(?:\\s+(?:the\\s+)?(?:song|track))?"_s + kOnPhone + u"|"_s + kPhone + u"\\s+(?:next|skip)"_s,
                     text)
                   .hasMatch()) {
        action = u"next"_s;
    } else if (whole(u"(?:previous|last)(?:\\s+(?:song|track))?"_s + kOnPhone + u"|"_s + kPhone + u"\\s+previous"_s, text)
                   .hasMatch()) {
        action = u"previous"_s;
    } else {
        return std::nullopt;
    }
    return tool(u"android_media"_s, {{u"action"_s, action}});
}

std::optional<IntentMatch> call(const IntentInput &input) {
    const QString &text = input.normalized;
    // A number needs no "on my phone": nobody chats about calling 0300 1234567.
    static const QString number = u"(\\+?\\d[\\d\\s\\-().]{1,20}\\d)"_s;
    if (QRegularExpressionMatch m = whole(u"(?:call|dial|ring|phone)\\s+"_s + number + u"(?:"_s + kOnPhone + u")?"_s, text);
        m.hasMatch()) {
        IntentMatch match = tool(u"android_call"_s, {{u"to"_s, m.captured(1).trimmed()}});
        match.verbatimPayload = true;
        return match;
    }
    // A name does: "call it a day" is not a phone call.
    if (QRegularExpressionMatch m = whole(u"(?:call|ring|phone)\\s+(.+?)"_s + kOnPhone, text);
        m.hasMatch()) {
        return tool(u"android_call"_s, {{u"to"_s, m.captured(1).trimmed()}});
    }
    return std::nullopt;
}

std::optional<IntentMatch> message(const IntentInput &input) {
    // On the raw text: the message keeps its capitals, its punctuation and a final "now" or
    // "please". The phone must be named, and the message introduced, or "text me later" would
    // become an SMS.
    static const QString saying = u"\\s+(?:saying|that says|with the message|to say)\\s*:?\\s+(.+)"_s;
    QRegularExpressionMatch m = whole(u"(?:text|sms|message)\\s+(.+?)"_s + kOnPhone + saying, input.raw, true);
    if (!m.hasMatch()) {
        m = whole(u"send\\s+(?:a\\s+)?(?:text|sms|message)\\s+to\\s+(.+?)"_s + kOnPhone + saying, input.raw, true);
    }
    if (!m.hasMatch()) return std::nullopt;
    IntentMatch match = tool(u"android_sms_send"_s, {{u"to"_s, m.captured(1).trimmed()}, {u"message"_s, m.captured(2).trimmed()}});
    match.verbatimPayload = true;   // the message is the user's words, "and" included
    return match;
}

std::optional<IntentMatch> inbox(const IntentInput &input) {
    const QString &text = input.normalized;
    if (whole(u"(?:read|check|show)\\s+(?:me\\s+)?(?:my\\s+)?(?:phone\\s+)?notifications(?:"_s + kOnPhone + u")?"_s
                  + u"|"_s + kPhone + u"\\s+notifications|(?:any|what are the)\\s+notifications"_s + kOnPhone,
              text)
            .hasMatch()) {
        return askAfter(u"android_notifications"_s, u"What are the notifications on my phone? Keep it short."_s);
    }
    if (whole(u"(?:read|check|show)\\s+(?:me\\s+)?(?:my\\s+)?(?:new\\s+|latest\\s+|recent\\s+)?(?:texts|sms|text messages)(?:"_s
                  + kOnPhone + u")?|(?:any\\s+)?new\\s+(?:texts|sms|text messages)"_s + kOnPhone,
              text)
            .hasMatch()) {
        return askAfter(u"android_sms_read"_s, u"What are my latest text messages? Keep it short."_s);
    }
    return std::nullopt;
}

std::optional<IntentMatch> keys(const IntentInput &input) {
    const QString &text = input.normalized;
    if (whole(u"lock\\s+"_s + kPhone + u"|lock(?:\\s+the\\s+screen)?"_s + kOnPhone, text).hasMatch()) {
        return tool(u"android_key"_s, {{u"key"_s, u"lock"_s}});
    }
    if (QRegularExpressionMatch m = whole(u"(?:go\\s+|press\\s+)?(home|back)"_s + kOnPhone + u"|"_s + kPhone + u"\\s+(home|back)"_s,
                                          text);
        m.hasMatch()) {
        return tool(u"android_key"_s, {{u"key"_s, m.captured(1) + m.captured(2)}});
    }
    return std::nullopt;
}

} // namespace

std::optional<IntentMatch> PhoneIntent::match(const IntentInput &input) const {
    for (const auto handler : {screen, openApp, volume, media, call, message, inbox, keys}) {
        if (std::optional<IntentMatch> found = handler(input)) return found;
    }
    return std::nullopt;
}

} // namespace nexa
