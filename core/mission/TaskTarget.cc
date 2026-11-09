#include "core/mission/TaskTarget.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

std::optional<int> numberOf(const QString &word) {
    static const QStringList words{u"zero"_s, u"one"_s, u"two"_s, u"three"_s, u"four"_s, u"five"_s,
                                   u"six"_s,  u"seven"_s, u"eight"_s, u"nine"_s, u"ten"_s};
    bool ok = false;
    const int digits = word.toInt(&ok);
    if (ok) return digits;
    const qsizetype at = words.indexOf(word);
    return at >= 0 ? std::optional<int>(int(at)) : std::nullopt;
}

std::optional<AgentRole> roleOf(const QString &word) {
    if (word == u"browser"_s || word == u"web"_s) return AgentRole::Web;
    if (word == u"agent"_s || word == u"general"_s) return AgentRole::General;
    return roleFromKey(word);
}

// The newest top-level task not finished, of `role` when given.
std::optional<TaskId> newestAtWork(const QList<Task> &tasks, std::optional<AgentRole> role) {
    for (auto it = tasks.crbegin(); it != tasks.crend(); ++it) {
        if (it->depth == 0 && !isFinished(it->state) && (!role || it->role == *role)) return it->id;
    }
    return std::nullopt;
}

} // namespace

std::optional<TaskId> TaskTarget::resolve(const QString &target, const QList<Task> &tasks, std::optional<TaskId> inView) {
    const QString said = target.trimmed().toLower();
    const auto exists = [&tasks](TaskId id) {
        return std::any_of(tasks.cbegin(), tasks.cend(), [id](const Task &t) { return t.id == id; });
    };
    if (said.isEmpty()) {
        for (const Task &task : tasks) {
            if (inView && task.id == *inView && !isFinished(task.state)) return task.id;
        }
        return newestAtWork(tasks, std::nullopt);
    }
    if (const std::optional<int> number = numberOf(said)) {
        return exists(TaskId(*number)) ? std::optional<TaskId>(TaskId(*number)) : std::nullopt;
    }
    static const QRegularExpression agent(u"^([a-z]+)[ -]?(\\d+)$"_s);
    if (const QRegularExpressionMatch found = agent.match(said); found.hasMatch()) {
        const TaskId id = found.captured(2).toULongLong();
        return exists(id) ? std::optional<TaskId>(id) : std::nullopt;
    }
    if (const std::optional<AgentRole> role = roleOf(said)) return newestAtWork(tasks, role);
    return std::nullopt;
}

} // namespace nexa
