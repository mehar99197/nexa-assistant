#include "core/mission/DelegateTool.h"

#include "core/mission/AgentRole.h"
#include "core/mission/MissionControl.h"
#include "core/tools/ToolArguments.h"

using namespace Qt::StringLiterals;

namespace nexa {

DelegateTool::DelegateTool(MissionControl *mission)
    : mission_(mission),
      spec_{QString(kName),
            u"Hand a self-contained part of your task to another of Nexa's agents and get its final answer back. Each "
            "role works with the model that fits it: web (the browser: sites, forms, mail), code (programs, scripts, "
            "tests), screen (what is on the PC's screen), phone (the user's Android phone), general (anything else). "
            "It does not see this conversation: say in `task` everything it needs, and use it only for work another "
            "role does better or that can go on alongside yours."_s,
            schema::object({{u"role"_s, schema::choice(u"Which agent does it"_s,
                                                       {u"web"_s, u"code"_s, u"screen"_s, u"phone"_s, u"general"_s})},
                            {u"task"_s, schema::string(u"What to do, complete in itself"_s)},
                            {u"context"_s, schema::string(u"What it needs to know from your task (optional)"_s)}},
                           {u"role"_s, u"task"_s}),
            false} {}

void DelegateTool::invoke(const QJsonObject &arguments, Done done) {
    invokeFor(arguments, ToolContext{}, std::move(done));
}

void DelegateTool::invokeFor(const QJsonObject &arguments, const ToolContext &context, Done done) {
    if (context.task == 0) {
        done(Error{ErrorCode::NotSupported, u"delegate_task works only inside a task on the mission board"_s});
        return;
    }
    const Result<QString> roleKeyText = toolargs::requiredString(arguments, u"role"_s);
    const std::optional<AgentRole> role = roleKeyText ? roleFromKey(roleKeyText.value()) : std::nullopt;
    if (!role) {
        done(Error{ErrorCode::InvalidArgument, u"role must be one of web, code, screen, phone, general"_s});
        return;
    }
    const Result<QString> task = toolargs::requiredString(arguments, u"task"_s);
    if (!task) {
        done(task.error());
        return;
    }
    mission_->delegate(context.task, *role, task.value(), arguments.value(u"context"_s).toString().trimmed(),
                       std::move(done));
}

} // namespace nexa
