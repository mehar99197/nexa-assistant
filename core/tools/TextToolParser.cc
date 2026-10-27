#include "core/tools/TextToolParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

std::optional<QJsonValue> parseJson(const QString &text) {
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
std::optional<ToolCall> callFrom(const QJsonObject &object, const QStringList &known) {
    QJsonObject source = object;
    if (object.value(u"function"_s).isObject()) source = object.value(u"function"_s).toObject();
    QString name = source.value(u"name"_s).toString();
    if (name.isEmpty()) name = source.value(u"tool"_s).toString();
    if (name.isEmpty() || !known.contains(name)) return std::nullopt;

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

QList<ToolCall> callsFrom(const QJsonValue &value, const QStringList &known) {
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
    static const QRegularExpression block(u"<tool_call>\\s*(.*?)\\s*</tool_call>"_s,
                                          QRegularExpression::DotMatchesEverythingOption);
    QString rest = reply;
    qsizetype removed = 0;
    QRegularExpressionMatchIterator it = block.globalMatch(reply);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const std::optional<QJsonValue> json = parseJson(m.captured(1));
        const QList<ToolCall> calls = json ? callsFrom(*json, known) : QList<ToolCall>{};
        if (calls.isEmpty()) continue;      // not a call Nexa can make: leave it for the user to see
        parsed.calls += calls;
        rest.remove(m.capturedStart() - removed, m.capturedLength());
        removed += m.capturedLength();
    }

    // A reply that is nothing but a call, as some models answer when told to "use a tool".
    if (parsed.calls.isEmpty()) {
        static const QRegularExpression fenced(u"^\\s*```(?:json)?\\s*(.*?)\\s*```\\s*$"_s,
                                               QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch m = fenced.match(reply);
        const QString candidate = m.hasMatch() ? m.captured(1) : reply;
        const QString trimmed = candidate.trimmed();
        if (trimmed.startsWith(u'{') || trimmed.startsWith(u'[')) {
            if (const std::optional<QJsonValue> json = parseJson(trimmed)) {
                parsed.calls = callsFrom(*json, known);
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

    static const QString tag = u"<tool_call"_s;
    qsizetype safe = pending_.size();
    if (const qsizetype start = pending_.indexOf(tag); start >= 0) {
        safe = start;
        holding_ = true;
    } else {
        for (qsizetype n = std::min(tag.size() - 1, pending_.size()); n > 0; --n) {
            if (tag.startsWith(pending_.right(n))) {
                safe = pending_.size() - n;
                break;
            }
        }
    }
    qsizetype end = safe;
    while (end > 0 && pending_.at(end - 1).isSpace()) --end;
    const QString out = pending_.left(end);
    pending_.remove(0, end);
    shown_ += out;
    return out;
}

} // namespace nexa
