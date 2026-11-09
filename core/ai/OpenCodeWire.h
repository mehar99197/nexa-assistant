#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
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
    bool aborted{false};    // Error: only the report of an abort (MessageAbortedError)
    int status{0};          // Error: the HTTP status the provider answered with, when opencode passes it on
    QString toolName;       // PartType "tool": the tool the model called
    QString toolStatus;     // PartType "tool": pending, running, error, completed
    QString toolInput;      // PartType "tool": its arguments, a JSON object as compact text
};

// One sign-in entry of GET /provider/auth: a way to connect one provider.
struct OpenCodeAuthMethod {
    QString providerId;
    int index{0};           // its place in the provider's list — what authorize and callback call "method"
    bool oauth{false};      // type "oauth" (a browser sign-in); the other kind is "api" (a pasted key)
    QString label;          // opencode's own wording: "ChatGPT Pro/Plus (browser)"
    bool hasPrompts{false}; // opencode wants answers first (an enterprise URL, an account name …)
};

// The reply of POST /provider/<id>/oauth/authorize.
struct OpenCodeAuthorization {
    QUrl url;               // where to sign in: http or https, and a secret (state, PKCE): never logged
    bool needsCode{false};  // method "code": the browser ends at a code to paste back; "auto": opencode waits itself
    QString instructions;   // opencode's words, often the device code of a headless flow
};

// Where a sign-in step failed, for the words of the failure.
enum class OpenCodeAuthStep { Authorize, WaitForBrowser, SubmitCode };

// opencode's server API (`opencode serve`), as Nexa uses it (pure): the prompt body — Nexa's system
// prompt, opencode's own tools off — the events, the model list, and the authenticated request.
class OpenCodeWire {
public:
    // `model` is "provider/model" (split at the first '/'); empty leaves opencode's default. opencode's
    // own tools are all off — but `read`, for opencode's free models (see the .cc), which the server denies.
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
    // opencode's own tools (and the MCP resource tools it adds); a call to any other name is meant for Nexa.
    static bool isOwnTool(const QString &name);

    // --- Signing in (opencode 1.19.3's /provider/auth, /provider/<id>/oauth/*; the Facade is OpenCodeSignIn) ---

    // GET /provider/auth: {providerId: [{type, label, prompts?}]}, as a list by provider id, then index. Unknown
    // fields and entries of an unknown shape are skipped; a body that is no JSON object is a Parse error.
    static Result<QList<OpenCodeAuthMethod>> parseAuthMethods(const QByteArray &body);
    // {"method": index}: the index into the provider's list, as authorize takes it (no `inputs`: Nexa offers
    // no method that asks questions).
    static QByteArray authorizeBody(int method);
    // {"method": index} and, when given, "code": what the browser page showed.
    static QByteArray callbackBody(int method, const QString &code = {});
    // The authorize reply. A `null` body (HTTP 200: the index is not an oauth method) is NotSupported;
    // anything else that is not {url, method: "auto"|"code", instructions} with an http(s) url is Parse.
    static Result<OpenCodeAuthorization> parseAuthorization(const QByteArray &body);
    // The callback reply: true when the body is `true`.
    static bool isTrue(const QByteArray &body);
    // The providers opencode calls connected: "connected" of GET /provider, or the ids of GET /config/providers'
    // "providers" (the same list — Provider.list() — in 26 ms against 2 s for /provider's 7 MB). A body that
    // has neither is a Parse error: an unknown reply must never read as "nothing connected".
    static Result<QStringList> parseConnected(const QByteArray &body);
    // The failure of a sign-in step as words for the person: opencode's error name (a 400 body {name, data},
    // or Effect's {_tag, message}) turned into a sentence. Never carries a url, a code or a long server text.
    static Error authFailure(int httpStatus, const QByteArray &body, OpenCodeAuthStep step);
};

} // namespace nexa
