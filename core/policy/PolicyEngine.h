#pragma once

#include "core/tools/ToolSpec.h"

#include <QJsonObject>
#include <QString>

namespace nexa {

enum class PolicyMode {
    Autonomous,             // run everything; risky actions are only flagged in the audit log
    ConfirmRisky,           // ask before root and destructive actions
    ConfirmAll,             // ask before every action that changes something
};

struct PolicyDecision {
    bool needsConfirmation{false};
    bool destructive{false};
    QString reason;         // why it is risky (empty when it is not)
};

// Strategy for "may this action run unattended?". Pure: no I/O.
class PolicyEngine {
public:
    explicit PolicyEngine(PolicyMode mode = PolicyMode::Autonomous) : mode_(mode) {}

    PolicyMode mode() const { return mode_; }
    void setMode(PolicyMode mode) { mode_ = mode; }

    PolicyDecision evaluate(const ToolSpec &spec, const QJsonObject &arguments) const;

    // Returns why a shell command is destructive (rm -r, mkfs, dd of=, shutdown, …), or an empty string.
    static QString destructiveReason(const QString &command);

    static PolicyMode modeFromString(const QString &value);
    static QString modeToString(PolicyMode mode);

private:
    PolicyMode mode_;
};

} // namespace nexa
