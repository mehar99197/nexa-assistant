#pragma once

#include "core/ai/SignIn.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QUrl>

#include <optional>

namespace nexa {

// One line of agy's Agent Client Protocol (`agy acp`: JSON-RPC 2.0, one object per line, both ways), as far
// as sign-in cares. A response has an id and a result or an error; a notification has a method and no id; a
// request from agy has both a method and an id and wants an answer.
struct AcpMessage {
    bool valid{false};          // a JSON object that is a response, a notification or a request
    qint64 id{-1};              // a numeric id (the ones Nexa sends); -1 when there is none
    QJsonValue rawId{QJsonValue::Undefined};    // the id as sent, to answer a request from agy (Undefined without one)
    bool isError{false};
    int code{0};                // error.code
    QString message;            // error.message — agy's own words, not for the screen as they are
    QJsonValue result;          // result (a response)
    QString method;             // a notification or a request from agy
    QJsonValue params;          // its params

    bool isResponse() const { return valid && id >= 0 && method.isEmpty(); }
    bool isNotification() const { return valid && !method.isEmpty() && rawId.isUndefined(); }
    bool isRequest() const { return valid && !method.isEmpty() && !rawId.isUndefined(); }
};

// What `initialize` tells: the sign-in methods agy offers, whether it can log out, and its version.
struct AcpInitialize {
    struct AuthMethod {
        QString id;
        QString name;
    };
    QList<AuthMethod> authMethods;
    bool canLogout{false};      // agentCapabilities.auth.logout is there
    QString version;
};

// What `session/new` returns when signed in: the plan (never an email) and the session to close again.
struct AcpSession {
    QString sessionId;
    QString plan;               // _meta.antigravity.tier.name, "Google AI Pro"
    QString planId;             // _meta.antigravity.tier.id
};

// Adapter between agy's ACP and what signing in needs (pure, like AgyEvents): requests as lines for its
// standard input, replies decoded, and what the replies mean for signing in. Measured against agy 1.4.12.
// What is NOT measured — `authenticate`'s reply and `logout` — is read as little as possible: a reply
// without an error is taken as "agy says so" and checked afterwards with `session/new`.
class AgyAcp {
public:
    static constexpr int kAuthRequiredCode = -32000;
    static constexpr int kInvalidParamsCode = -32602;
    static constexpr int kMethodNotFoundCode = -32601;

    // {"jsonrpc":"2.0","id":…,"method":…,"params":…} compact, and a newline.
    static QByteArray request(int id, const QString &method, const QJsonObject &params = {});
    static QByteArray initializeRequest(int id);
    static QByteArray newSessionRequest(int id, const QString &workspace);
    static QByteArray closeSessionRequest(int id, const QString &sessionId);
    static QByteArray authenticateRequest(int id, const QString &methodId);
    static QByteArray logoutRequest(int id);
    // The answer to a request from agy that Nexa does not serve: an error, so agy does not wait for it.
    static QByteArray errorReply(const QJsonValue &id, int code, const QString &message);

    static AcpMessage decode(const QByteArray &line);

    static std::optional<AcpInitialize> parseInitialize(const QJsonValue &result);
    static std::optional<AcpSession> parseSession(const QJsonValue &result);

    // `session/new` refused because nobody is signed in (code -32000, "Authentication required").
    static bool isAuthRequired(const AcpMessage &message);

    // The sign-ins that run in a browser (ids starting "oauth-"), labelled for Settings; others agy lists
    // (an API key, a cloud platform) are set up some other way and not offered.
    static QList<SignInMethod> oauthMethods(const AcpInitialize &initialize);

    // A login link agy hands over in a notification (its method says "auth", its params carry an http(s)
    // url), for the case that agy could not open a browser itself. Empty when the message has none.
    static QUrl authLink(const AcpMessage &message);

    // agy's own words made safe to show: no address, no long token-like run, cut short.
    static QString safeText(const QString &text);
};

} // namespace nexa
