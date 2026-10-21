#include "core/tools/ToolRegistry.h"

#include <algorithm>

namespace nexa {

void ToolRegistry::add(std::unique_ptr<ITool> tool) {
    const QString name = tool->spec().name;
    if (ITool *existing = find(name)) {
        std::erase_if(tools_, [existing](const std::unique_ptr<ITool> &candidate) {
            return candidate.get() == existing;
        });
    }
    byName_.insert(name, tool.get());
    tools_.push_back(std::move(tool));
}

void ToolRegistry::remove(const QString &name) {
    ITool *existing = find(name);
    if (!existing) return;
    byName_.remove(name);
    std::erase_if(tools_, [existing](const std::unique_ptr<ITool> &candidate) { return candidate.get() == existing; });
}

ITool *ToolRegistry::find(const QString &name) const {
    return byName_.value(name, nullptr);
}

QList<ToolSpec> ToolRegistry::specs() const {
    QList<ToolSpec> result;
    result.reserve(qsizetype(tools_.size()));
    for (const auto &tool : tools_) {
        if (isOffered(tool->spec().name)) result.append(tool->spec());
    }
    return result;
}

void ToolRegistry::withhold(const QString &name, const QString &reason) {
    withheld_.insert(name, reason.isEmpty() ? QStringLiteral("it is not available now") : reason);
}

void ToolRegistry::offer(const QString &name) {
    withheld_.remove(name);
}

void ToolRegistry::cancelAll() {
    for (const auto &tool : tools_) tool->cancel();
}

} // namespace nexa
