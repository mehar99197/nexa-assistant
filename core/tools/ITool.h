#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"
#include "core/tools/ToolSpec.h"

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace nexa {

struct ToolResult {
    QString text;                               // what the model and the action log see
    std::optional<ImageAttachment> image;       // e.g. a screenshot, sent to vision models
};

// Command pattern: every action Nexa performs — from the fast path or the model — is an ITool
// invocation, so policy, audit and cancellation apply uniformly.
class ITool {
public:
    using Done = std::function<void(Result<ToolResult>)>;

    virtual ~ITool() = default;

    virtual const ToolSpec &spec() const = 0;
    // `done` runs exactly once — after cancel() too, with ErrorCode::Cancelled.
    virtual void invoke(const QJsonObject &arguments, Done done) = 0;
    virtual void cancel() {}
};

} // namespace nexa
