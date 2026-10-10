#pragma once

#include "core/Result.h"
#include "core/tools/ITool.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace nexa::toolargs {

// Reading a tool call's arguments the forgiving way. Models send numbers and booleans as strings
// often enough ("x": "120", "muted": "yes") that rejecting them would only make tools look broken.
std::optional<int> intArg(const QJsonObject &arguments, const QString &key);
std::optional<bool> boolArg(const QJsonObject &arguments, const QString &key);
// Trimmed; an empty or missing value is an InvalidArgument naming the key.
Result<QString> requiredString(const QJsonObject &arguments, const QString &key);

// A text-only tool result.
ToolResult text(const QString &value);

} // namespace nexa::toolargs
