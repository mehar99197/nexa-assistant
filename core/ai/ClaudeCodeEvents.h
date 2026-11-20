#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"
#include "core/ai/SignIn.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace nexa {

// One line of Claude Code's `--output-format stream-json`, as far as Nexa cares.
struct ClaudeCodeEvent {
    enum class Kind { Init, Text, ApiError, Result, Ignored };
    Kind kind{Kind::Ignored};
    QString model;              // Init: the model id Claude Code resolved its alias to
    QString sessionId;          // Init, Result
    QString text;               // Text: a piece of the reply (a text_delta; thinking is not reply text)
    bool ok{true};              // Result: is_error false
    QString response;           // Result: the whole reply
    QString subtype;            // Result: "success", "error_during_execution", "error_max_turns", …
    QString error;              // Result: the error's words when !ok
    // ApiError: the name on an `assistant` event of a turn the API refused ("authentication_failed"); Result: the
    // same, filled in by the session that saw the ApiError event before the result.
    QString apiError;
    int apiErrorStatus{0};      // Result: api_error_status when the API answered with an HTTP status
};

// Claude Code's print mode, both ways (pure): events read from its standard output, turns written to its standard
// input (`--input-format stream-json`), the answer of `claude auth status --json`, and what its failures mean.
// Measured against Claude Code 2.1.291 on 2026-11-12.
class ClaudeCodeEvents {
public:
    static ClaudeCodeEvent decode(const QByteArray &line);
    // {"type":"user","message":{"role":"user","content":[…image blocks…, {"type":"text","text":…}]}} and a newline:
    // the Anthropic Messages shape, images as base64 blocks before the words.
    static QByteArray userTurn(const QString &text, const QList<ImageAttachment> &images = {});
    // The models the toolbar offers: Claude Code's aliases for its latest models (there is no list command).
    static QStringList knownModels();
    // `claude auth status --json`: loggedIn, authMethod and subscriptionType are read — the plan, never the account
    // (the JSON also carries the email and the organisation, which nothing of Nexa's touches). A reply that is not
    // that JSON is Parse, never "signed out".
    static Result<SignInStatus> parseStatus(const QByteArray &json);
    // Claude Code ended before it answered: not signed in, or what it said last.
    static Error exitError(int exitCode, const QString &stderrTail);
    // A turn that ended with is_error: the sign-in gone reads as Unauthorized ("not signed in", which the registry
    // recognises), a usage or rate limit as HTTP 429, a model Claude Code does not have as NotFound (this provider
    // cannot answer: the chain may ask another).
    static Error resultError(const ClaudeCodeEvent &result);
    // The sentence every "not signed in" error ends with.
    static QString notSignedInMessage();
};

} // namespace nexa
