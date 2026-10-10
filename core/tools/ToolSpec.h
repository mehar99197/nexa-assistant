#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace nexa {

// Name, description and JSON-Schema parameters of a tool the model (or the fast path) can call.
struct ToolSpec {
    QString name;
    QString description;
    QJsonObject parameters;     // JSON Schema of the arguments object
    bool mutating{true};        // changes system state (policy + audit)
    bool privileged{false};     // runs as root

    // {"type":"function","function":{name, description, parameters}}
    QJsonObject toOpenAi() const;
};

// Helpers for compact JSON Schemas.
namespace schema {

QJsonObject string(const QString &description);
QJsonObject integer(const QString &description, int minimum, int maximum);
QJsonObject boolean(const QString &description);
QJsonObject choice(const QString &description, const QStringList &values);
QJsonObject object(const QJsonObject &properties = {}, const QStringList &required = {});

} // namespace schema

} // namespace nexa
