// Flat OpenDocument files from plain content: well-formed XML with the right pieces in it.

#include "system/office/OdfWriter.h"

#include "QtStringMakers.h"

#include <QJsonValue>
#include <QXmlStreamReader>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

bool wellFormed(const QString &xml) {
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) reader.readNext();
    return !reader.hasError();
}

int count(const QString &haystack, const QString &needle) {
    return int(haystack.count(needle));
}

} // namespace

TEST_CASE("OdfWriter turns Markdown into a Writer document with headings, lists and spans") {
    const QString xml = OdfWriter::document(u"Solar <energy>"_s,
                                            u"# Overview\n\nSolar power is **clean** and *cheap*.\nIt keeps growing.\n\n"
                                            "- panels\n- inverters\n\n## Steps\n1. plan\n2. install\n\nDone & dusted."_s);
    REQUIRE(wellFormed(xml));
    CHECK(xml.contains(u"office:mimetype=\"application/vnd.oasis.opendocument.text\""_s));
    CHECK(xml.contains(u"<dc:title>Solar &lt;energy&gt;</dc:title>"_s));
    CHECK(xml.contains(u"<text:p text:style-name=\"Title\">Solar &lt;energy&gt;</text:p>"_s));
    CHECK(xml.contains(u"<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\">Overview</text:h>"_s));
    CHECK(xml.contains(u"<text:h text:style-name=\"Heading_20_2\" text:outline-level=\"2\">Steps</text:h>"_s));
    // Two lines of one paragraph become one paragraph; the markup becomes spans.
    CHECK(xml.contains(u"Solar power is <text:span text:style-name=\"Strong\">clean</text:span> and "
                       "<text:span text:style-name=\"Emphasis\">cheap</text:span>. It keeps growing.</text:p>"_s));
    CHECK(count(xml, u"<text:list text:style-name=\"Bullets\">"_s) == 1);
    CHECK(count(xml, u"<text:list text:style-name=\"Numbers\">"_s) == 1);
    CHECK(count(xml, u"<text:list-item>"_s) == 4);
    CHECK(xml.contains(u"Done &amp; dusted."_s));
}

TEST_CASE("OdfWriter makes one Impress page per slide, title slides centred") {
    const QString xml = OdfWriter::presentation(u"Plan"_s, {{u"Plan"_s, {}},
                                                            {u"Why"_s, {u"sun is free"_s, u"no \"fuel\""_s}},
                                                            {u"How"_s, {u"panels"_s}}});
    REQUIRE(wellFormed(xml));
    CHECK(xml.contains(u"office:mimetype=\"application/vnd.oasis.opendocument.presentation\""_s));
    CHECK(count(xml, u"<draw:page "_s) == 3);
    CHECK(xml.contains(u"draw:master-page-name=\"Default\""_s));
    CHECK(xml.contains(u"<style:master-page style:name=\"Default\""_s));
    CHECK(count(xml, u"text:style-name=\"BigTitle\""_s) == 1);
    CHECK(count(xml, u"text:style-name=\"SlideTitle\""_s) == 2);
    CHECK(count(xml, u"<text:list-item>"_s) == 3);
    CHECK(xml.contains(u"no &quot;fuel&quot;"_s));
}

TEST_CASE("OdfWriter writes a Calc sheet with typed cells and LibreOffice's formula notation") {
    const QString xml = OdfWriter::spreadsheet(u"Budget"_s, {}, {{u"Item"_s, u"Cost"_s, u"Paid"_s},
                                                                 {u"Rent"_s, 1200, true},
                                                                 {u"Food"_s, 300.5, false},
                                                                 {u"Total"_s, u"=SUM(B2:B3)"_s, QJsonValue()}});
    REQUIRE(wellFormed(xml));
    CHECK(xml.contains(u"<table:table table:name=\"Budget\">"_s));
    CHECK(xml.contains(u"table:number-columns-repeated=\"3\""_s));
    CHECK(count(xml, u"table:style-name=\"Head\""_s) == 3);
    CHECK(xml.contains(u"office:value-type=\"float\" office:value=\"1200\"><text:p>1200</text:p>"_s));
    CHECK(xml.contains(u"office:value=\"300.5\""_s));
    CHECK(xml.contains(u"office:value-type=\"boolean\" office:boolean-value=\"true\""_s));
    CHECK(xml.contains(u"table:formula=\"of:=SUM([.B2:.B3])\"/>"_s));
    CHECK(xml.contains(u"<table:table-cell/>"_s));

    CHECK(OdfWriter::formula(u"=A1*2+$B$2"_s) == u"of:=[.A1]*2+[.$B$2]"_s);
    CHECK(OdfWriter::formula(u"AVERAGE(C2:C10)"_s) == u"of:=AVERAGE([.C2:.C10])"_s);
    CHECK(OdfWriter::formula(u"=IF(A2>0;\"A1 ok\";LOG10(B2))"_s) == u"of:=IF([.A2]>0;\"A1 ok\";LOG10([.B2]))"_s);
    // Exponents are numbers, whole-column ranges are LibreOffice's to read.
    CHECK(OdfWriter::formula(u"=B2*1E3"_s) == u"of:=[.B2]*1E3"_s);
    CHECK(OdfWriter::formula(u"=2.5e3+ROUND(B2/1E3;2)"_s) == u"of:=2.5e3+ROUND([.B2]/1E3;2)"_s);
    CHECK(OdfWriter::formula(u"=SUM(B:B)"_s) == u"of:=SUM(B:B)"_s);
}

TEST_CASE("OdfWriter names files and components by kind") {
    CHECK(OdfWriter::fileName(u"Solar / energy: plan?"_s, OdfWriter::Kind::Presentation) == u"Solar energy plan.fodp"_s);
    CHECK(OdfWriter::fileName(u"   "_s, OdfWriter::Kind::Document) == u"Untitled.fodt"_s);
    CHECK(OdfWriter::fileName(u"Budget..."_s, OdfWriter::Kind::Spreadsheet) == u"Budget.fods"_s);
    CHECK(OdfWriter::kindFromString(u"Slides"_s) == OdfWriter::Kind::Presentation);
    CHECK_FALSE(OdfWriter::kindFromString(u"poster"_s).has_value());
    CHECK(OdfWriter::componentName(OdfWriter::Kind::Spreadsheet) == u"LibreOffice Calc"_s);
    CHECK(OdfWriter::componentId(OdfWriter::Kind::Document) == u"libreoffice-writer.desktop"_s);
    CHECK(OdfWriter::escape(u"a<b&c\"d'e"_s) == u"a&lt;b&amp;c&quot;d&apos;e"_s);
}
