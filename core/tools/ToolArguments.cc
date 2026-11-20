#include "core/tools/ToolArguments.h"

#include <QJsonValue>

using namespace Qt::StringLiterals;

namespace nexa::toolargs {

std::optional<int> intArg(const QJsonObject &arguments, const QString &key) {
    const QJsonValue value = arguments.value(key);
    if (value.isDouble()) return int(value.toDouble());
    bool ok = false;
    const int parsed = value.toString().trimmed().toInt(&ok);
    return ok ? std::optional<int>(parsed) : std::nullopt;
}

std::optional<bool> boolArg(const QJsonObject &arguments, const QString &key) {
    const QJsonValue value = arguments.value(key);
    if (value.isBool()) return value.toBool();
    const QString text = value.toString().trimmed().toLower();
    if (text == u"true" || text == u"yes" || text == u"1") return true;
    if (text == u"false" || text == u"no" || text == u"0") return false;
    return std::nullopt;
}

Result<QString> requiredString(const QJsonObject &arguments, const QString &key) {
    const QString value = arguments.value(key).toString().trimmed();
    if (value.isEmpty()) return Error{ErrorCode::InvalidArgument, u"Missing \"%1\""_s.arg(key)};
    return value;
}

Result<QString> requiredText(const QJsonObject &arguments, const QString &key) {
    const QString value = arguments.value(key).toString();
    if (value.trimmed().isEmpty()) return Error{ErrorCode::InvalidArgument, u"Missing \"%1\""_s.arg(key)};
    return value;
}

ToolResult text(const QString &value) {
    return ToolResult{value, std::nullopt};
}

} // namespace nexa::toolargs
