#include "remote/RemoteProtocol.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa::remote {

namespace {

QByteArray message(const QString &type, QJsonObject fields = {}) {
    fields.insert(u"t"_s, type);
    return QJsonDocument(fields).toJson(QJsonDocument::Compact);
}

} // namespace

std::optional<ClientMessage> decode(QByteArrayView frame) {
    if (frame.isEmpty() || frame.size() > kMaxTextFrame) return std::nullopt;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(frame.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    const QJsonObject object = document.object();
    const QString type = object.value(u"t"_s).toString();

    ClientMessage parsed;
    if (type == u"hello") {
        parsed.type = ClientMessage::Type::Hello;
        parsed.token = object.value(u"token"_s).toString();
        if (parsed.token.isEmpty()) return std::nullopt;
    } else if (type == u"text") {
        parsed.type = ClientMessage::Type::Text;
        parsed.text = object.value(u"text"_s).toString().trimmed();
        if (parsed.text.isEmpty()) return std::nullopt;
    } else if (type == u"cancel") {
        parsed.type = ClientMessage::Type::Cancel;
    } else if (type == u"ping") {
        parsed.type = ClientMessage::Type::Ping;
    } else if (type == u"unlock") {
        parsed.type = ClientMessage::Type::Unlock;
    } else if (type == u"answer") {
        // Exactly a positive whole number and a yes or no: an answer is never guessed into a "yes".
        const QJsonValue id = object.value(u"id"_s);
        const QJsonValue allow = object.value(u"allow"_s);
        if (!id.isDouble() || !allow.isBool()) return std::nullopt;
        const double number = id.toDouble();
        if (number < 1 || number > 9.0e15 || number != std::floor(number)) return std::nullopt;
        parsed.type = ClientMessage::Type::Answer;
        parsed.id = quint64(number);
        parsed.allow = allow.toBool();
    } else {
        return std::nullopt;
    }
    return parsed;
}

QByteArray welcome(const QString &device, bool voice, bool busy, const QString &mode) {
    QJsonObject fields{{u"version"_s, kVersion}, {u"device"_s, device}, {u"voice"_s, voice}, {u"busy"_s, busy}};
    if (!mode.isEmpty()) fields.insert(u"mode"_s, mode);
    return message(u"welcome"_s, fields);
}

QByteArray state(const QString &mode, const QStringList &nodes) {
    return message(u"state"_s, {{u"mode"_s, mode}, {u"nodes"_s, QJsonArray::fromStringList(nodes)}});
}

QByteArray accepted(const QString &text, const QString &source) {
    return message(u"accepted"_s, {{u"text"_s, text}, {u"source"_s, source}});
}

QByteArray delta(const QString &text) {
    return message(u"delta"_s, {{u"text"_s, text}});
}

QByteArray step(const QString &description, bool ok) {
    return message(u"step"_s, {{u"text"_s, description}, {u"ok"_s, ok}});
}

QByteArray finished(const QString &reason) {
    return message(u"finished"_s, {{u"reason"_s, reason}});
}

QByteArray failed(const QString &text) {
    return message(u"failed"_s, {{u"text"_s, text}});
}

QByteArray transcript(const QString &text) {
    return message(u"transcript"_s, {{u"text"_s, text}});
}

QByteArray speech(const QString &url) {
    return message(u"speech"_s, {{u"url"_s, url}});
}

QByteArray busy(bool value) {
    return message(u"busy"_s, {{u"busy"_s, value}});
}

QByteArray error(const QString &code, const QString &text) {
    return message(u"error"_s, {{u"code"_s, code}, {u"text"_s, text}});
}

QByteArray pong() {
    return message(u"pong"_s);
}

QByteArray lock(bool locked, const QString &text) {
    return message(u"lock"_s, {{u"locked"_s, locked}, {u"text"_s, text}});
}

QByteArray approval(quint64 id, const QString &agent, const QString &tool, const QString &question) {
    return message(u"approval"_s,
                   {{u"id"_s, qint64(id)}, {u"agent"_s, agent}, {u"tool"_s, tool}, {u"text"_s, question}});
}

QByteArray settled(quint64 id, bool allowed) {
    return message(u"settled"_s, {{u"id"_s, qint64(id)}, {u"allow"_s, allowed}});
}

std::optional<ServerMessage> decodeServer(QByteArrayView frame) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(frame.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    const QJsonObject object = document.object();

    using Type = ServerMessage::Type;
    static const QHash<QString, Type> types{
        {u"welcome"_s, Type::Welcome},   {u"accepted"_s, Type::Accepted}, {u"delta"_s, Type::Delta},
        {u"step"_s, Type::Step},         {u"finished"_s, Type::Finished}, {u"failed"_s, Type::Failed},
        {u"transcript"_s, Type::Transcript}, {u"speech"_s, Type::Speech}, {u"busy"_s, Type::Busy},
        {u"error"_s, Type::Error},       {u"pong"_s, Type::Pong},         {u"state"_s, Type::State},
        {u"lock"_s, Type::Lock},         {u"approval"_s, Type::Approval}, {u"settled"_s, Type::Settled},
    };
    const auto type = types.constFind(object.value(u"t"_s).toString());
    if (type == types.cend()) return std::nullopt;

    ServerMessage parsed;
    parsed.type = *type;
    parsed.text = object.value(u"text"_s).toString();
    parsed.source = object.value(u"source"_s).toString();
    parsed.device = object.value(u"device"_s).toString();
    parsed.reason = object.value(u"reason"_s).toString();
    parsed.url = object.value(u"url"_s).toString();
    parsed.code = object.value(u"code"_s).toString();
    parsed.version = object.value(u"version"_s).toInt();
    parsed.ok = object.value(u"ok"_s).toBool(true);
    parsed.busy = object.value(u"busy"_s).toBool();
    parsed.voice = object.value(u"voice"_s).toBool();
    parsed.mode = object.value(u"mode"_s).toString();
    parsed.locked = object.value(u"locked"_s).toBool();
    parsed.id = quint64(std::max<qint64>(0, object.value(u"id"_s).toInteger()));
    parsed.agent = object.value(u"agent"_s).toString();
    parsed.tool = object.value(u"tool"_s).toString();
    parsed.allow = object.value(u"allow"_s).toBool();
    for (const QJsonValue &node : object.value(u"nodes"_s).toArray()) {
        if (node.isString()) parsed.nodes.append(node.toString());
    }
    return parsed;
}

QByteArray hello(const QString &token) {
    return message(u"hello"_s, {{u"token"_s, token}});
}

QByteArray text(const QString &request) {
    return message(u"text"_s, {{u"text"_s, request}});
}

QByteArray cancel() {
    return message(u"cancel"_s);
}

QByteArray ping() {
    return message(u"ping"_s);
}

QByteArray unlock() {
    return message(u"unlock"_s);
}

QByteArray answer(quint64 id, bool allow) {
    return message(u"answer"_s, {{u"id"_s, qint64(id)}, {u"allow"_s, allow}});
}

} // namespace nexa::remote
