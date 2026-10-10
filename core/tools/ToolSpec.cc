#include "core/tools/ToolSpec.h"

#include <QJsonArray>

using namespace Qt::StringLiterals;

namespace nexa {

QJsonObject ToolSpec::toOpenAi() const {
    return QJsonObject{
        {u"type"_s, u"function"_s},
        {u"function"_s, QJsonObject{
            {u"name"_s, name},
            {u"description"_s, description},
            {u"parameters"_s, parameters.isEmpty() ? schema::object() : parameters},
        }},
    };
}

namespace schema {

QJsonObject string(const QString &description) {
    return {{u"type"_s, u"string"_s}, {u"description"_s, description}};
}

QJsonObject integer(const QString &description, int minimum, int maximum) {
    return {{u"type"_s, u"integer"_s}, {u"description"_s, description},
            {u"minimum"_s, minimum}, {u"maximum"_s, maximum}};
}

QJsonObject boolean(const QString &description) {
    return {{u"type"_s, u"boolean"_s}, {u"description"_s, description}};
}

QJsonObject choice(const QString &description, const QStringList &values) {
    return {{u"type"_s, u"string"_s}, {u"description"_s, description}, {u"enum"_s, QJsonArray::fromStringList(values)}};
}

QJsonObject object(const QJsonObject &properties, const QStringList &required) {
    QJsonObject result{{u"type"_s, u"object"_s}, {u"properties"_s, properties}};
    if (!required.isEmpty()) result.insert(u"required"_s, QJsonArray::fromStringList(required));
    return result;
}

} // namespace schema

} // namespace nexa
