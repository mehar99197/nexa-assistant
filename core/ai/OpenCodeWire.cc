#include "core/ai/OpenCodeWire.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

QJsonObject OpenCodeWire::promptBody(const QString &model, const QString &system, const QString &text,
                                     const QList<ImageAttachment> &images) {
    QJsonArray parts{QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, text}}};
    for (const ImageAttachment &image : images) {
        parts.append(QJsonObject{
            {u"type"_s, u"file"_s},
            {u"mime"_s, image.mimeType},
            {u"url"_s, u"data:%1;base64,%2"_s.arg(image.mimeType, QString::fromLatin1(image.data.toBase64()))},
        });
    }
    // Every tool of opencode's own off: Nexa's tools travel in the prompt, and only Nexa acts.
    QJsonObject body{{u"tools"_s, QJsonObject{{u"*"_s, false}}}, {u"parts"_s, parts}};
    if (const qsizetype slash = model.indexOf(u'/'); slash > 0) {
        body.insert(u"model"_s, QJsonObject{{u"providerID"_s, model.left(slash)}, {u"modelID"_s, model.mid(slash + 1)}});
    }
    if (!system.isEmpty()) body.insert(u"system"_s, system);
    return body;
}

OpenCodeEvent OpenCodeWire::decodeEvent(const QByteArray &data) {
    OpenCodeEvent event;
    const QJsonObject object = QJsonDocument::fromJson(data).object();
    const QString type = object.value(u"type"_s).toString();
    const QJsonObject properties = object.value(u"properties"_s).toObject();
    event.sessionId = properties.value(u"sessionID"_s).toString();
    if (type == u"message.part.delta") {
        if (properties.value(u"field"_s).toString() == u"text") {
            event.kind = OpenCodeEvent::Kind::TextDelta;
            event.partId = properties.value(u"partID"_s).toString();
            event.text = properties.value(u"delta"_s).toString();
        }
    } else if (type == u"message.part.updated") {
        // Each part is announced before its deltas; only this says which are reasoning.
        const QJsonObject part = properties.value(u"part"_s).toObject();
        event.kind = OpenCodeEvent::Kind::PartType;
        event.partId = part.value(u"id"_s).toString();
        event.partType = part.value(u"type"_s).toString();
        if (event.sessionId.isEmpty()) event.sessionId = part.value(u"sessionID"_s).toString();
    } else if (type == u"session.status") {
        const QJsonObject status = properties.value(u"status"_s).toObject();
        const QString state = status.value(u"type"_s).toString();
        if (state == u"busy") event.kind = OpenCodeEvent::Kind::Busy;
        if (state == u"idle") event.kind = OpenCodeEvent::Kind::Idle;
        if (state == u"retry") {
            event.kind = OpenCodeEvent::Kind::Retry;
            event.message = status.value(u"message"_s).toString();
            event.reason = status.value(u"action"_s).toObject().value(u"reason"_s).toString();
        }
    } else if (type == u"session.idle") {
        event.kind = OpenCodeEvent::Kind::Idle;
    } else if (type == u"session.error") {
        const QJsonObject error = properties.value(u"error"_s).toObject();
        event.kind = OpenCodeEvent::Kind::Error;
        event.authError = error.value(u"name"_s).toString() == u"ProviderAuthError";
        event.message = error.value(u"data"_s).toObject().value(u"message"_s).toString();
        if (event.message.isEmpty()) event.message = error.value(u"name"_s).toString();
    }
    return event;
}

QStringList OpenCodeWire::parseModels(const QByteArray &configProviders, const QString &preferred) {
    QStringList models;
    const QJsonArray providers = QJsonDocument::fromJson(configProviders).object().value(u"providers"_s).toArray();
    for (const QJsonValue &value : providers) {
        const QJsonObject provider = value.toObject();
        const QString id = provider.value(u"id"_s).toString();
        const QJsonObject list = provider.value(u"models"_s).toObject();
        for (auto it = list.begin(); it != list.end(); ++it) models.append(id + u'/' + it.key());
    }
    std::sort(models.begin(), models.end());
    if (const qsizetype at = models.indexOf(preferred); at > 0) models.move(at, 0);
    return models;
}

QString OpenCodeWire::parseDefaultModel(const QByteArray &config) {
    return QJsonDocument::fromJson(config).object().value(u"model"_s).toString();
}

QUrl OpenCodeWire::parseListening(const QByteArray &output) {
    static const QRegularExpression line(u"opencode server listening on (https?://\\S+)"_s);
    const QRegularExpressionMatch match = line.match(QString::fromUtf8(output));
    return match.hasMatch() ? QUrl(match.captured(1)) : QUrl();
}

QString OpenCodeWire::parseSessionId(const QByteArray &body) {
    return QJsonDocument::fromJson(body).object().value(u"id"_s).toString();
}

QNetworkRequest OpenCodeWire::request(const QUrl &base, const QString &path, const QString &password) {
    QString root = base.toString();
    while (root.endsWith(u'/')) root.chop(1);
    QNetworkRequest request(QUrl(root + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setRawHeader("Authorization", "Basic " + (u"opencode:"_s + password).toUtf8().toBase64());
    return request;
}

} // namespace nexa
