#include "core/tools/TextToolParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QStringView>

#include <algorithm>
#include <array>
#include <optional>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

std::optional<QJsonValue> parseJson(QStringView text) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(text.trimmed().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) return std::nullopt;
    if (document.isObject()) return QJsonValue(document.object());
    if (document.isArray()) return QJsonValue(document.array());
    return std::nullopt;
}

// {"name": …, "arguments": {…}} — or the variants models produce: "tool"/"function" for the name,
// "parameters"/"args"/"input" for the arguments, arguments as a JSON string, or the whole thing
// wrapped as {"function": {"name", "arguments"}}.
std::optional<ToolCall> callFrom(const QJsonObject &object, const QSet<QStringView> &known) {
    QJsonObject source = object;
    if (object.value(u"function"_s).isObject()) source = object.value(u"function"_s).toObject();
    QString name = source.value(u"name"_s).toString();
    if (name.isEmpty()) name = source.value(u"tool"_s).toString();
    if (name.isEmpty() || !known.contains(QStringView(name))) return std::nullopt;

    QJsonValue arguments = source.value(u"arguments"_s);
    for (const QString &key : {u"parameters"_s, u"args"_s, u"input"_s}) {
        if (arguments.isUndefined()) arguments = source.value(key);
    }
    if (arguments.isString()) {
        const std::optional<QJsonValue> inner = parseJson(arguments.toString());
        arguments = inner && inner->isObject() ? *inner : QJsonValue(QJsonObject{});
    }
    if (!arguments.isObject()) arguments = QJsonObject{};
    return ToolCall{QString(), name, QString::fromUtf8(QJsonDocument(arguments.toObject()).toJson(QJsonDocument::Compact))};
}

QList<ToolCall> callsFrom(const QJsonValue &value, const QSet<QStringView> &known) {
    QList<ToolCall> calls;
    if (value.isObject()) {
        if (const std::optional<ToolCall> call = callFrom(value.toObject(), known)) calls.append(*call);
    } else if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            if (!item.isObject()) return {};     // all of it a call, or none of it
            const std::optional<ToolCall> call = callFrom(item.toObject(), known);
            if (!call) return {};
            calls.append(*call);
        }
    }
    return calls;
}

// Reads `name="..."` or `name='...'` out of an XML tag header slice in O(len).
QStringView readNameAttribute(QStringView tagHeader) {
    qsizetype pos = 0;
    while (pos < tagHeader.size()) {
        while (pos < tagHeader.size() && tagHeader.at(pos).isSpace()) ++pos;
        const qsizetype keyStart = pos;
        while (pos < tagHeader.size() && !tagHeader.at(pos).isSpace() && tagHeader.at(pos) != u'=') ++pos;
        const QStringView key = tagHeader.sliced(keyStart, pos - keyStart);
        while (pos < tagHeader.size() && tagHeader.at(pos).isSpace()) ++pos;
        if (pos >= tagHeader.size() || tagHeader.at(pos) != u'=') {
            if (pos == keyStart) ++pos;
            continue;
        }
        ++pos; // skip '='
        while (pos < tagHeader.size() && tagHeader.at(pos).isSpace()) ++pos;
        if (pos >= tagHeader.size()) break;
        const QChar quote = tagHeader.at(pos);
        if (quote != u'"' && quote != u'\'') continue;
        ++pos;
        const qsizetype valStart = pos;
        const qsizetype valEnd = tagHeader.indexOf(quote, valStart);
        if (valEnd < 0) break;
        if (key == u"name") return tagHeader.sliced(valStart, valEnd - valStart);
        pos = valEnd + 1;
    }
    return {};
}

// Parses `<parameter name="...">value</parameter>` (or `<antml:parameter ...>`) inside one `<invoke>` body in O(m).
QJsonObject parseXmlParameters(QStringView invokeBody) {
    QJsonObject args;
    constexpr QStringView kParamOpen = u"<parameter";
    constexpr QStringView kAntmlParamOpen = u"<antml:parameter";
    constexpr QStringView kParamClose = u"</parameter>";
    constexpr QStringView kAntmlParamClose = u"</antml:parameter>";

    qsizetype pos = 0;
    while (pos < invokeBody.size()) {
        const qsizetype p1 = invokeBody.indexOf(kParamOpen, pos);
        const qsizetype p2 = invokeBody.indexOf(kAntmlParamOpen, pos);
        if (p1 < 0 && p2 < 0) break;
        const bool isAntml = p1 < 0 || (p2 >= 0 && p2 < p1);
        const qsizetype openIdx = isAntml ? p2 : p1;
        const qsizetype tagLen = isAntml ? kAntmlParamOpen.size() : kParamOpen.size();
        const qsizetype afterTag = openIdx + tagLen;
        if (afterTag < invokeBody.size()) {
            const QChar next = invokeBody.at(afterTag);
            if (!next.isSpace() && next != u'>' && next != u'/') {
                pos = afterTag;
                continue;
            }
        }
        const qsizetype gt = invokeBody.indexOf(u'>', afterTag);
        if (gt < 0) break;
        const bool selfClosing = gt > afterTag && invokeBody.at(gt - 1) == u'/';
        const QStringView attrs = invokeBody.sliced(afterTag, (selfClosing ? gt - 1 : gt) - afterTag);
        const QStringView paramName = readNameAttribute(attrs);
        if (selfClosing) {
            if (!paramName.isEmpty()) args.insert(paramName.toString(), QString());
            pos = gt + 1;
            continue;
        }
        const QStringView closeTag = isAntml ? kAntmlParamClose : kParamClose;
        const qsizetype closeIdx = invokeBody.indexOf(closeTag, gt + 1);
        if (closeIdx < 0) break;
        const QStringView rawVal = invokeBody.sliced(gt + 1, closeIdx - (gt + 1));
        if (!paramName.isEmpty()) {
            const QStringView trimmed = rawVal.trimmed();
            if (trimmed.startsWith(u'{') || trimmed.startsWith(u'[')) {
                if (const std::optional<QJsonValue> json = parseJson(trimmed)) {
                    args.insert(paramName.toString(), *json);
                } else {
                    args.insert(paramName.toString(), rawVal.toString());
                }
            } else {
                args.insert(paramName.toString(), rawVal.toString());
            }
        }
        pos = closeIdx + closeTag.size();
    }
    return args;
}

// Parses `<invoke ...>...</invoke>` / `<antml:invoke ...>...</antml:invoke>` elements inside `<function_calls>`.
// Returns empty if malformed or any invoke targets an unknown tool (leaving the block visible to the user).
QList<ToolCall> parseFunctionCallsBlock(QStringView blockBody, const QSet<QStringView> &known) {
    constexpr QStringView kInvokeOpen = u"<invoke";
    constexpr QStringView kAntmlInvokeOpen = u"<antml:invoke";
    constexpr QStringView kInvokeClose = u"</invoke>";
    constexpr QStringView kAntmlInvokeClose = u"</antml:invoke>";

    QList<ToolCall> calls;
    qsizetype pos = 0;
    while (pos < blockBody.size()) {
        const qsizetype i1 = blockBody.indexOf(kInvokeOpen, pos);
        const qsizetype i2 = blockBody.indexOf(kAntmlInvokeOpen, pos);
        if (i1 < 0 && i2 < 0) break;
        const bool isAntml = i1 < 0 || (i2 >= 0 && i2 < i1);
        const qsizetype openIdx = isAntml ? i2 : i1;
        const qsizetype tagLen = isAntml ? kAntmlInvokeOpen.size() : kInvokeOpen.size();
        const qsizetype afterTag = openIdx + tagLen;
        if (afterTag < blockBody.size()) {
            const QChar next = blockBody.at(afterTag);
            if (!next.isSpace() && next != u'>' && next != u'/') {
                pos = afterTag;
                continue;
            }
        }
        const qsizetype gt = blockBody.indexOf(u'>', afterTag);
        if (gt < 0) return {};
        const bool selfClosing = gt > afterTag && blockBody.at(gt - 1) == u'/';
        const QStringView attrs = blockBody.sliced(afterTag, (selfClosing ? gt - 1 : gt) - afterTag);
        const QStringView toolName = readNameAttribute(attrs);
        if (toolName.isEmpty() || !known.contains(toolName)) return {};

        QJsonObject args;
        if (selfClosing) {
            pos = gt + 1;
        } else {
            const QStringView closeTag = isAntml ? kAntmlInvokeClose : kInvokeClose;
            const qsizetype closeIdx = blockBody.indexOf(closeTag, gt + 1);
            if (closeIdx < 0) return {};
            args = parseXmlParameters(blockBody.sliced(gt + 1, closeIdx - (gt + 1)));
            pos = closeIdx + closeTag.size();
        }
        calls.append(ToolCall{QString(), toolName.toString(),
                              QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
    }
    return calls;
}

// A JSON Schema property, said in a few words: "string, required — Application name".
QString describe(const QString &name, const QJsonObject &property, bool required) {
    QString text = u"%1: %2"_s.arg(name, property.value(u"type"_s).toString(u"value"_s));
    if (required) text += u", required"_s;
    if (property.contains(u"enum"_s)) {
        QStringList values;
        for (const QJsonValue &value : property.value(u"enum"_s).toArray()) values.append(value.toVariant().toString());
        text += u", one of %1"_s.arg(values.join(u'|'));
    }
    if (property.contains(u"minimum"_s) && property.contains(u"maximum"_s)) {
        text += u", %1…%2"_s.arg(property.value(u"minimum"_s).toDouble()).arg(property.value(u"maximum"_s).toDouble());
    }
    const QString description = property.value(u"description"_s).toString();
    if (!description.isEmpty()) text += u" — "_s + description;
    return text;
}

} // namespace

TextToolParser::Parsed TextToolParser::parse(const QString &reply, const QStringList &known) {
    Parsed parsed;
    QSet<QStringView> knownSet;
    knownSet.reserve(known.size());
    for (const QString &name : known) knownSet.insert(QStringView(name));

    // O(n) single-pass scan over `<tool_call>…</tool_call>`, `<function_calls>…</function_calls>`
    // and `<antml:function_calls>…</antml:function_calls>`.
    constexpr QStringView kToolOpen = u"<tool_call>";
    constexpr QStringView kToolClose = u"</tool_call>";
    constexpr QStringView kFcOpen = u"<function_calls>";
    constexpr QStringView kFcClose = u"</function_calls>";
    constexpr QStringView kAntmlFcOpen = u"<antml:function_calls>";
    constexpr QStringView kAntmlFcClose = u"</antml:function_calls>";

    const QStringView view(reply);
    QString rest;
    rest.reserve(reply.size());
    qsizetype cursor = 0;

    while (cursor < view.size()) {
        enum class Kind { ToolCall, FunctionCalls, AntmlFunctionCalls };
        qsizetype nextIdx = view.size() + 1;
        Kind nextKind = Kind::ToolCall;

        const qsizetype tIdx = view.indexOf(kToolOpen, cursor);
        if (tIdx >= 0 && tIdx < nextIdx) {
            nextIdx = tIdx;
            nextKind = Kind::ToolCall;
        }
        const qsizetype fIdx = view.indexOf(kFcOpen, cursor);
        if (fIdx >= 0 && fIdx < nextIdx) {
            nextIdx = fIdx;
            nextKind = Kind::FunctionCalls;
        }
        const qsizetype aIdx = view.indexOf(kAntmlFcOpen, cursor);
        if (aIdx >= 0 && aIdx < nextIdx) {
            nextIdx = aIdx;
            nextKind = Kind::AntmlFunctionCalls;
        }

        if (nextIdx > view.size()) break;

        QStringView closeTag = kToolClose;
        qsizetype openLen = kToolOpen.size();
        if (nextKind == Kind::FunctionCalls) {
            closeTag = kFcClose;
            openLen = kFcOpen.size();
        } else if (nextKind == Kind::AntmlFunctionCalls) {
            closeTag = kAntmlFcClose;
            openLen = kAntmlFcOpen.size();
        }

        const qsizetype closeIdx = view.indexOf(closeTag, nextIdx + openLen);
        if (closeIdx < 0) break;

        const QStringView body = view.sliced(nextIdx + openLen, closeIdx - (nextIdx + openLen));
        QList<ToolCall> calls;
        if (nextKind == Kind::ToolCall) {
            const std::optional<QJsonValue> json = parseJson(body);
            calls = json ? callsFrom(*json, knownSet) : QList<ToolCall>{};
        } else {
            calls = parseFunctionCallsBlock(body, knownSet);
        }

        const qsizetype blockEnd = closeIdx + closeTag.size();
        if (calls.isEmpty()) {
            rest.append(view.sliced(cursor, blockEnd - cursor));
        } else {
            rest.append(view.sliced(cursor, nextIdx - cursor));
            parsed.calls += calls;
        }
        cursor = blockEnd;
    }
    rest.append(view.sliced(cursor));

    // A reply that is nothing but a call, as some models answer when told to "use a tool".
    if (parsed.calls.isEmpty()) {
        static const QRegularExpression fenced(u"^\\s*```(?:json)?\\s*(.*?)\\s*```\\s*$"_s,
                                               QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch m = fenced.match(reply);
        const QString candidate = m.hasMatch() ? m.captured(1) : reply;
        const QString trimmed = candidate.trimmed();
        if (trimmed.startsWith(u'{') || trimmed.startsWith(u'[')) {
            if (const std::optional<QJsonValue> json = parseJson(trimmed)) {
                parsed.calls = callsFrom(*json, knownSet);
                if (!parsed.calls.isEmpty()) rest.clear();
            }
        }
    }

    for (qsizetype i = 0; i < parsed.calls.size(); ++i) parsed.calls[i].id = u"text_call_%1"_s.arg(i + 1);
    parsed.text = rest.trimmed();
    return parsed;
}

QString TextToolParser::instructions(const QList<ToolSpec> &tools) {
    QStringList lines{
        u"You can act on the user's computer with tools. To use one, write a line exactly like this:"_s,
        u"<tool_call>{\"name\": \"TOOL_NAME\", \"arguments\": {\"ARGUMENT\": \"VALUE\"}}</tool_call>"_s,
        u"You may write several such lines. Write nothing after them: the result of each comes back in a "
        "message that starts with <tool_result name=\"TOOL_NAME\">. Then carry on, or answer normally once "
        "you are done. Never invent a result."_s,
        QString(),
        u"Tools:"_s,
    };
    for (const ToolSpec &tool : tools) {
        lines.append(u"- %1: %2"_s.arg(tool.name, tool.description));
        const QJsonObject properties = tool.parameters.value(u"properties"_s).toObject();
        QStringList required;
        for (const QJsonValue &value : tool.parameters.value(u"required"_s).toArray()) required.append(value.toString());
        for (auto property = properties.constBegin(); property != properties.constEnd(); ++property) {
            lines.append(u"    "_s + describe(property.key(), property.value().toObject(), required.contains(property.key())));
        }
    }
    return lines.join(u'\n');
}

QList<ChatMessage> TextToolParser::asText(const QList<ChatMessage> &messages) {
    QList<ChatMessage> converted;
    for (const ChatMessage &message : messages) {
        ChatMessage plain = message;
        if (message.role == Role::Assistant && !message.toolCalls.isEmpty()) {
            QStringList parts;
            if (!message.text.isEmpty()) parts.append(message.text);
            for (const ToolCall &call : message.toolCalls) {
                const QJsonObject arguments = QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object();
                const QJsonObject wire{{u"name"_s, call.name}, {u"arguments"_s, arguments}};
                parts.append(u"<tool_call>%1</tool_call>"_s.arg(QString::fromUtf8(QJsonDocument(wire).toJson(QJsonDocument::Compact))));
            }
            plain.text = parts.join(u'\n');
            plain.toolCalls.clear();
        } else if (message.role == Role::Tool) {
            plain.role = Role::User;
            plain.text = u"<tool_result name=\"%1\">\n%2\n</tool_result>"_s.arg(message.toolName, message.text);
            plain.toolCallId.clear();
            plain.toolName.clear();
        }
        // Results in a row become one message: such servers expect user and assistant to alternate.
        if (!converted.isEmpty() && plain.role == Role::User && converted.last().role == Role::User) {
            converted.last().text += u"\n\n"_s + plain.text;
            converted.last().images += plain.images;
            continue;
        }
        converted.append(plain);
    }
    return converted;
}

ChatRequest TextToolParser::asTextRequest(const ChatRequest &request) {
    ChatRequest text = request;
    text.messages = asText(request.messages);
    const QString convention = instructions(request.tools);
    if (!text.messages.isEmpty() && text.messages.first().role == Role::System) {
        text.messages.first().text += u"\n\n"_s + convention;
    } else {
        text.messages.prepend(ChatMessage{.role = Role::System, .text = convention});
    }
    text.tools.clear();
    return text;
}

QString TextToolParser::StreamFilter::feed(const QString &delta) {
    if (holding_) return {};
    pending_ += delta;
    if (shown_.isEmpty()) {
        qsizetype first = 0;
        while (first < pending_.size() && pending_.at(first).isSpace()) ++first;
        if (first == pending_.size()) return {};
        const QChar opening = pending_.at(first);
        if (opening == u'{' || opening == u'[' || opening == u'`') {
            holding_ = true;
            return {};
        }
        pending_.remove(0, first);
    }

    static constexpr std::array<QStringView, 3> kTags{
        u"<tool_call",
        u"<function_calls",
        u"<antml:function_calls",
    };
    qsizetype safe = pending_.size();
    qsizetype earliest = pending_.size() + 1;
    for (const QStringView tag : kTags) {
        if (const qsizetype idx = pending_.indexOf(tag); idx >= 0 && idx < earliest) earliest = idx;
    }
    if (earliest <= pending_.size()) {
        safe = earliest;
        holding_ = true;
    } else {
        const QStringView pendingView(pending_);
        qsizetype holdLen = 0;
        for (const QStringView tag : kTags) {
            const qsizetype maxN = std::min(tag.size() - 1, pendingView.size());
            for (qsizetype n = maxN; n > holdLen; --n) {
                if (tag.startsWith(pendingView.last(n))) {
                    holdLen = n;
                    break;
                }
            }
        }
        safe = pending_.size() - holdLen;
    }
    qsizetype end = safe;
    while (end > 0 && pending_.at(end - 1).isSpace()) --end;
    const QString out = pending_.left(end);
    pending_.remove(0, end);
    shown_ += out;
    return out;
}

} // namespace nexa
