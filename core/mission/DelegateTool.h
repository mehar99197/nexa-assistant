#pragma once

#include "core/tools/ITool.h"

#include <QJsonObject>

namespace nexa {

class MissionControl;

// delegate_task (Command): a lead agent hands a self-contained part of its task to an agent of another role, which
// works on it with the model that fits that role, and gets its final answer back as the result. Only inside a task,
// one level deep (a sub-task's agent is not offered it). Not mutating itself: what the sub-task does is checked and
// audited action by action. Its cancel() is a no-op: stopping a task stops its sub-tasks (MissionControl).
class DelegateTool : public ITool {
public:
    explicit DelegateTool(MissionControl *mission);

    const ToolSpec &spec() const override { return spec_; }
    void invoke(const QJsonObject &arguments, Done done) override;
    void invokeFor(const QJsonObject &arguments, const ToolContext &context, Done done) override;

    static constexpr QLatin1StringView kName{"delegate_task"};

private:
    MissionControl *mission_;
    ToolSpec spec_;
};

} // namespace nexa
