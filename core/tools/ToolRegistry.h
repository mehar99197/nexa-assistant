#pragma once

#include "core/tools/ITool.h"

#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <vector>

namespace nexa {

// Registry of every tool, in registration order (the order the model sees them).
class ToolRegistry {
public:
    // Replaces an existing tool of the same name.
    void add(std::unique_ptr<ITool> tool);
    ITool *find(const QString &name) const;
    bool contains(const QString &name) const { return byName_.contains(name); }
    QList<ToolSpec> specs() const;
    void cancelAll();

private:
    std::vector<std::unique_ptr<ITool>> tools_;
    QHash<QString, ITool *> byName_;
};

} // namespace nexa
