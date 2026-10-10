#pragma once

#include "core/intent/IIntentHandler.h"

#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace nexa {

// Chain of Responsibility over deterministic handlers that turn common phrases into tool calls in
// well under a millisecond, so "open firefox" never waits for a model. The first handler that
// matches wins; matches whose tool is not registered are skipped, and compound requests
// ("open x and do y") are left to the agent.
class IntentRouter {
public:
    using ToolAvailable = std::function<bool(const QString &toolName)>;

    explicit IntentRouter(ToolAvailable available = {});

    void add(std::unique_ptr<IIntentHandler> handler);
    std::optional<IntentMatch> route(const QString &text) const;

    static IntentInput prepare(const QString &text);
    static bool isCompound(const QString &normalized);

private:
    ToolAvailable available_;
    std::vector<std::unique_ptr<IIntentHandler>> handlers_;
};

} // namespace nexa
