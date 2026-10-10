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

ITool *ToolRegistry::find(const QString &name) const {
    return byName_.value(name, nullptr);
}

QList<ToolSpec> ToolRegistry::specs() const {
    QList<ToolSpec> result;
    result.reserve(qsizetype(tools_.size()));
    for (const auto &tool : tools_) result.append(tool->spec());
    return result;
}

void ToolRegistry::cancelAll() {
    for (const auto &tool : tools_) tool->cancel();
}

} // namespace nexa
