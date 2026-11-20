#pragma once

#include "core/mission/AgentRole.h"

#include <QString>
#include <QStringView>

namespace nexa {

// Which kind of agent leads a task, from its words (the fast path's way: patterns, no model). The phone wins over
// the screen, the screen over the web, the web over code; anything else is General. Pure.
class RoleClassifier {
public:
    static AgentRole classify(const QString &text);
    // Whose work a tool is (a fast-path task's role comes from the tool it runs).
    static AgentRole forTool(QStringView toolName);
};

} // namespace nexa
