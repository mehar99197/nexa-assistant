// core/skills/SkillParser: SKILL.md as Claude's skills write it, and as Nexa writes it back.

#include "core/skills/SkillParser.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("A SKILL.md gives its name, its description and the instructions after the front matter") {
    const auto plain = SkillParser::parse(u"---\nname: morning-routine\ndescription: Opens mail and the calendar.\n---\n\n# Steps\n1. Open Gmail.\n"_s);
    REQUIRE(plain.isOk());
    CHECK(plain.value().name == u"morning-routine"_s);
    CHECK(plain.value().description == u"Opens mail and the calendar."_s);
    CHECK(plain.value().body == u"# Steps\n1. Open Gmail."_s);

    // What real skills carry: other keys, quotes, folded blocks, nested maps, CRLF and a BOM.
    const QString claude = QChar(0xFEFF) + u"---\r\nname: \"pdf-tools\"\r\nargument-hint: <file>\r\nmodel: sonnet\r\n"
                                           "description: >\r\n  Fill in and merge PDFs.\r\n  Use for any PDF job.\r\n"
                                           "metadata:\r\n  author: someone\r\n---\r\nDo it.\r\n"_s;
    const auto real = SkillParser::parse(claude);
    REQUIRE(real.isOk());
    CHECK(real.value().name == u"pdf-tools"_s);
    CHECK(real.value().description == u"Fill in and merge PDFs. Use for any PDF job."_s);
    CHECK(real.value().body == u"Do it."_s);

    const auto single = SkillParser::parse(u"---\nname: x\ndescription: 'It''s quoted: fine'\n---\n"_s);
    REQUIRE(single.isOk());
    CHECK(single.value().description == u"It's quoted: fine"_s);
    CHECK(single.value().body.isEmpty());
}

TEST_CASE("A SKILL.md without what a skill needs is refused, saying why") {
    CHECK_FALSE(SkillParser::parse(u"name: x\ndescription: y\n"_s).isOk());                     // no front matter
    CHECK_FALSE(SkillParser::parse(u"---\nname: x\ndescription: y\n"_s).isOk());                // not closed
    CHECK_FALSE(SkillParser::parse(u"---\ndescription: y\n---\n"_s).isOk());                    // no name
    CHECK_FALSE(SkillParser::parse(u"---\nname: x\n---\n"_s).isOk());                           // no description
    CHECK(SkillParser::parse(u"---\nname: Morning Routine\ndescription: y\n---\n"_s).error().message.contains(u"not a skill name"_s));
    CHECK_FALSE(SkillParser::parse(u"---\nname: x\ndescription: "_s + QString(1025, u'a') + u"\n---\n"_s).isOk());

    CHECK(SkillParser::isValidName(u"web-browsing"_s));
    CHECK(SkillParser::isValidName(u"a1"_s));
    CHECK_FALSE(SkillParser::isValidName(u"-leading"_s));
    CHECK_FALSE(SkillParser::isValidName(u"trailing-"_s));
    CHECK_FALSE(SkillParser::isValidName(u"../escape"_s));
    CHECK_FALSE(SkillParser::isValidName(QString(65, u'a')));
}

TEST_CASE("What Nexa writes reads back the same, whatever the description holds") {
    const SkillDocument document{u"tricky"_s, u"Says \"hi\": then a back\\slash"_s, u"Step one.\n\nStep two."_s};
    const auto back = SkillParser::parse(SkillParser::compose(document));
    REQUIRE(back.isOk());
    CHECK(back.value() == document);
}
