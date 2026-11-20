#pragma once

// A mission board for tests: real tools, policy, executor and audit, and scripted models handed out by a fake
// environment, one instance per task as the app does.

#include "core/audit/AuditLog.h"
#include "core/mission/MissionControl.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"

#include "ScriptedProvider.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace nexa::test {

using namespace Qt::StringLiterals;
using Reply = ScriptedProvider::Reply;

inline constexpr int kWaitMs = 5'000;

inline ToolCall call(const QString &id, const QString &name, const QString &arguments) {
    return ToolCall{id, name, arguments};
}

struct FakeEnvironment : ITaskEnvironment {
    Result<std::shared_ptr<IAiProvider>> createProvider(const ModelChoice &choice) override {
        created.append(choice);
        if (failWith) return *failWith;
        // Kept alive here too, so a test can read what a released provider was asked.
        auto provider = std::make_shared<ScriptedProvider>();
        if (!scripts.isEmpty()) provider->script = scripts.takeFirst();
        providers.append(provider);
        return std::shared_ptr<IAiProvider>(provider);
    }
    QString modelFor(const ModelChoice &choice) override { return choice.model.isEmpty() ? u"auto-model"_s : choice.model; }
    QString describeChoice(const ModelChoice &choice) override { return choice.isAuto() ? u"Auto"_s : choice.provider; }
    QList<ProviderFacts> providerFacts() override { return facts; }
    QString systemPrompt(const QString &source, AgentRole role) override {
        return u"prompt for %1 from %2"_s.arg(roleKey(role), source);
    }
    int maxSteps() override { return 8; }

    QList<QList<Reply>> scripts;                    // one per provider made, in order
    QList<ModelChoice> created;
    QList<std::shared_ptr<ScriptedProvider>> providers;
    QList<ProviderFacts> facts;
    std::optional<Error> failWith;
};

struct Board {
    explicit Board(int slotCount = 3) : audit(dir.filePath(u"audit.jsonl"_s)), executor(&registry, &policy, &audit) {
        registry.add(FunctionTool::sync(ToolSpec{u"echo"_s, u"Echo"_s, {}}, [](const QJsonObject &arguments) -> Result<ToolResult> {
            return ToolResult{u"echoed "_s + arguments.value(u"msg"_s).toString(), std::nullopt};
        }));
        MissionConfig config;
        config.maxConcurrent = slotCount;
        mission = std::make_unique<MissionControl>(&executor, &registry, &env, config);
        executor.setConfirmer([this](const ConfirmRequest &request, std::function<void(bool)> answer) {
            mission->confirm(request, std::move(answer));
        });
    }

    TaskState state(TaskId id) const { return mission->task(id) ? mission->task(id)->state : TaskState::Stopped; }
    bool waitFor(TaskId id, TaskState wanted) {
        return QTest::qWaitFor([this, id, wanted] { return mission->task(id) && mission->task(id)->state == wanted; }, kWaitMs);
    }
    TaskId ask(const QString &text, std::optional<TaskId> continues = std::nullopt) {
        TaskRequest request;
        request.text = text;
        request.continues = continues;
        return mission->add(request);
    }
    int auditLines() const {
        QFile file(dir.filePath(u"audit.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return 0;
        return int(file.readAll().count('\n'));
    }

    QTemporaryDir dir;
    FakeEnvironment env;
    ToolRegistry registry;
    PolicyEngine policy;
    AuditLog audit;
    ToolExecutor executor;
    std::unique_ptr<MissionControl> mission;
};

} // namespace nexa::test
