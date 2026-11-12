// core/tools/ToolArguments: how tool arguments are read — forgiving about numbers as strings, and keeping text to
// type exactly as written (a trailing newline is the Enter the model asked for).

#include "core/tools/ToolArguments.h"

#include "QtStringMakers.h"

#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::toolargs;
using namespace Qt::StringLiterals;

TEST_CASE("requiredText keeps the text as written, and only a blank one is missing") {
    const Result<QString> typed = requiredText({{u"text"_s, u"ls -la\n"_s}}, u"text"_s);
    REQUIRE(typed);
    CHECK(typed.value() == u"ls -la\n"_s);
    const Result<QString> spaced = requiredText({{u"text"_s, u"  two  spaces  "_s}}, u"text"_s);
    REQUIRE(spaced);
    CHECK(spaced.value() == u"  two  spaces  "_s);
    const Result<QString> blank = requiredText({{u"text"_s, u" \n "_s}}, u"text"_s);
    REQUIRE_FALSE(blank);
    CHECK(blank.error().code == ErrorCode::InvalidArgument);
    CHECK(blank.error().message == u"Missing \"text\""_s);
    CHECK_FALSE(requiredText({}, u"text"_s));
}

TEST_CASE("requiredString trims, and a number sent as a string is still a number") {
    const Result<QString> name = requiredString({{u"name"_s, u"  calculator \n"_s}}, u"name"_s);
    REQUIRE(name);
    CHECK(name.value() == u"calculator"_s);
    CHECK_FALSE(requiredString({{u"name"_s, u"   "_s}}, u"name"_s));
    CHECK(intArg({{u"x"_s, u" 42 "_s}}, u"x"_s) == 42);
    CHECK(intArg({{u"x"_s, 7}}, u"x"_s) == 7);
    CHECK_FALSE(intArg({{u"x"_s, u"many"_s}}, u"x"_s).has_value());
    CHECK(boolArg({{u"on"_s, u"yes"_s}}, u"on"_s) == true);
    CHECK(boolArg({{u"on"_s, false}}, u"on"_s) == false);
    CHECK_FALSE(boolArg({{u"on"_s, u"maybe"_s}}, u"on"_s).has_value());
}
