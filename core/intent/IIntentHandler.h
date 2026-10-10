#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace nexa {

struct IntentInput {
    QString text;               // politeness/wake words stripped, original case (for payloads)
    QString normalized;         // lower case, single spaces, no trailing punctuation
};

struct IntentMatch {
    QString toolName;           // empty for control intents
    QJsonObject arguments;
    bool panic{false};          // "stop", "cancel": stop everything instead of running a tool
    bool forwardToAgent{false}; // run the tool, then let the model answer with its result attached
    QString agentPrompt;        // what the model is asked when forwarding
    bool verbatimPayload{false};// the payload is user text (a command, text to type): skip the compound check
};

// One link of the IntentRouter chain: recognises a phrase family without any network call.
class IIntentHandler {
public:
    virtual ~IIntentHandler() = default;
    virtual std::optional<IntentMatch> match(const IntentInput &input) const = 0;
};

// What the fast path needs to know about installed applications (implemented by the app index).
class IAppCatalog {
public:
    virtual ~IAppCatalog() = default;
    // 0..1 confidence that `query` names an installed application.
    virtual double score(const QString &query) const = 0;
};

} // namespace nexa
