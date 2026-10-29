#pragma once

#include "core/ai/ChatRequest.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace nexa {

// One event of opencode's /event stream, as far as a Nexa turn cares.
struct OpenCodeEvent {
    enum class Kind { TextDelta, PartType, Busy, Idle, Retry, Error, Other };
    Kind kind{Kind::Other};
    QString sessionId;
    QString partId;         // TextDelta, PartType: which part of the reply
    QString partType;       // PartType: "text", "reasoning", "step-start", …
    QString text;           // TextDelta: a piece of a part's text — the reasoning's too: see partType
    QString message;        // Retry, Error
    QString reason;         // Retry: why, e.g. "free_tier_limit"
    bool authError{false};  // Error: the provider behind the model wants a sign-in
    int status{0};          // Error: the HTTP status the provider answered with, when opencode passes it on
};

// opencode's server API (`opencode serve`), as Nexa uses it (pure): the prompt body — Nexa's system
// prompt, opencode's own tools off — the events, the model list, and the authenticated request.
class OpenCodeWire {
public:
    // `model` is "provider/model" (split at the first '/'); empty leaves opencode's default.
    static QJsonObject promptBody(const QString &model, const QString &system, const QString &text,
                                  const QList<ImageAttachment> &images);
    static OpenCodeEvent decodeEvent(const QByteArray &data);
    // Every "provider/model" of GET /config/providers, sorted, `preferred` first when it is listed.
    static QStringList parseModels(const QByteArray &configProviders, const QString &preferred);
    // Of the same list, which "provider/model" takes images (capabilities.input.image) and which does
    // not; a model whose capabilities do not say is left out (unknown).
    static QHash<QString, bool> parseImageSupport(const QByteArray &configProviders);
    // GET /config's "model": opencode's own default model, or "".
    static QString parseDefaultModel(const QByteArray &config);
    // `opencode serve`'s "opencode server listening on http://…" line; invalid until it appears.
    static QUrl parseListening(const QByteArray &output);
    static QString parseSessionId(const QByteArray &body);
    // JSON, and basic authentication as the user "opencode".
    static QNetworkRequest request(const QUrl &base, const QString &path, const QString &password);
};

} // namespace nexa
