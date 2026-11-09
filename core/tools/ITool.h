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

// Whom an invocation is for: where the request came from (gui | voice | phone | phone-voice), and the task it
// belongs to on the mission board (0: none — the fast path of a test, say).
struct ToolContext {
    QString source;
    quint64 task{0};
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
    // The same, knowing whom it is for: what the executor calls. Only a tool that needs to know (delegate_task, whose
    // sub-task belongs to the task that asked) overrides it.
    virtual void invokeFor(const QJsonObject &arguments, const ToolContext &context, Done done) {
        Q_UNUSED(context);
        invoke(arguments, std::move(done));
    }
    virtual void cancel() {}
};

} // namespace nexa
