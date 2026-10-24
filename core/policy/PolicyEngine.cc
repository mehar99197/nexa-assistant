#include "core/policy/PolicyEngine.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

struct DestructivePattern {
    const char16_t *pattern;
    const char16_t *reason;
};

constexpr std::array kDestructivePatterns{
    DestructivePattern{u"\\brm\\s+(?:-\\S*\\s+)*-\\S*[rRf]", u"recursive or forced delete (rm -r/-f)"},
    DestructivePattern{u"\\bmkfs(?:\\.\\w+)?\\b", u"formats a filesystem (mkfs)"},
    DestructivePattern{u"\\bdd\\b.*\\bof=", u"raw disk write (dd of=)"},
    DestructivePattern{u"\\b(?:wipefs|shred|fdisk|sfdisk|parted|sgdisk|blkdiscard)\\b", u"disk or partition tool"},
    DestructivePattern{u">\\s*/dev/(?:sd|nvme|hd|vd|mmcblk)", u"writes to a block device"},
    DestructivePattern{u"\\b(?:shutdown|reboot|poweroff|halt)\\b", u"shuts down or reboots"},
    DestructivePattern{u"\\bsystemctl\\s+(?:poweroff|reboot|halt|suspend|hibernate)\\b", u"shuts down or suspends"},
    DestructivePattern{u":\\(\\)\\s*\\{.*\\};\\s*:", u"fork bomb"},
    DestructivePattern{u"\\bchmod\\s+(?:-\\S+\\s+)*-R\\b.*\\s/(?:\\s|$)", u"recursive permission change on /"},
    DestructivePattern{u"\\bchown\\s+(?:-\\S+\\s+)*-R\\b.*\\s/(?:\\s|$)", u"recursive ownership change on /"},
    DestructivePattern{u"\\bkill(?:all)?\\s+(?:-\\S+\\s+)*-?1\\b", u"signals every process"},
    DestructivePattern{u"\\b(?:apt|apt-get|dpkg)\\s+(?:-\\S+\\s+)*(?:remove|purge|autoremove|-r|-P)\\b", u"removes packages"},
    DestructivePattern{u"\\bgit\\s+(?:reset\\s+--hard|clean\\s+-\\S*f|push\\s+(?:-\\S+\\s+)*--force)", u"discards git work"},
    DestructivePattern{u"\\bfind\\b.*\\s-delete\\b", u"bulk delete (find -delete)"},
    DestructivePattern{u"\\btruncate\\s+(?:-\\S+\\s+)*-s\\s*0\\b", u"truncates files"},
};

// The command a tool call will run, for tools that take one.
QString commandOf(const QJsonObject &arguments) {
    const QJsonValue command = arguments.value(u"command"_s);
    if (command.isString()) return command.toString();
    QStringList parts;
    for (const QJsonValue &part : arguments.value(u"argv"_s).toArray()) parts.append(part.toString());
    return parts.join(u' ');
}

} // namespace

PolicyDecision PolicyEngine::evaluate(const ToolSpec &spec, const QJsonObject &arguments) const {
    PolicyDecision decision;
    // Text typed into a terminal is a command once Enter follows it: judged like one.
    decision.reason = destructiveReason(spec.name == u"type_text"_s ? arguments.value(u"text"_s).toString()
                                                                     : commandOf(arguments));
    decision.destructive = !decision.reason.isEmpty();
    if (spec.privileged && decision.reason.isEmpty()) decision.reason = spec.risk.isEmpty() ? u"runs as root"_s : spec.risk;

    switch (mode_) {
    case PolicyMode::Autonomous:
        decision.needsConfirmation = false;
        break;
    case PolicyMode::ConfirmRisky:
        decision.needsConfirmation = spec.privileged || decision.destructive;
        break;
    case PolicyMode::ConfirmAll:
        decision.needsConfirmation = spec.mutating;
        break;
    }
    return decision;
}

QString PolicyEngine::destructiveReason(const QString &command) {
    if (command.trimmed().isEmpty()) return {};
    static const auto compiled = [] {
        std::vector<std::pair<QRegularExpression, QString>> list;
        for (const DestructivePattern &pattern : kDestructivePatterns) {
            list.emplace_back(QRegularExpression(QString::fromUtf16(pattern.pattern),
                                                 QRegularExpression::CaseInsensitiveOption),
                              QString::fromUtf16(pattern.reason));
        }
        return list;
    }();
    for (const auto &[expression, reason] : compiled) {
        if (expression.match(command).hasMatch()) return reason;
    }
    return {};
}

PolicyMode PolicyEngine::modeFromString(const QString &value) {
    if (value == u"confirm-risky") return PolicyMode::ConfirmRisky;
    if (value == u"confirm-all") return PolicyMode::ConfirmAll;
    return PolicyMode::Autonomous;
}

QString PolicyEngine::modeToString(PolicyMode mode) {
    switch (mode) {
    case PolicyMode::ConfirmRisky: return u"confirm-risky"_s;
    case PolicyMode::ConfirmAll: return u"confirm-all"_s;
    case PolicyMode::Autonomous: break;
    }
    return u"autonomous"_s;
}

} // namespace nexa
