#pragma once

#include <QByteArray>
#include <QString>

#include <catch2/catch_tostring.hpp>

#include <string>

// Readable Catch2 failure messages for Qt string types.
namespace Catch {

template <>
struct StringMaker<QString> {
    static std::string convert(const QString &value) { return '"' + value.toStdString() + '"'; }
};

template <>
struct StringMaker<QByteArray> {
    static std::string convert(const QByteArray &value) { return '"' + value.toStdString() + '"'; }
};

} // namespace Catch
