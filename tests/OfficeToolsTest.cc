// create_document end to end: the file lands where it should, as OpenDocument XML, and — when
// LibreOffice is installed — LibreOffice reads it back and computes the formulas in it.

#include "core/agent/AppContext.h"
#include "core/tools/ITool.h"
#include "core/tools/ToolRegistry.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/files/FileAccess.h"
#include "system/office/OfficeTools.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kConvertWaitMs = 150'000;

struct Fixture {
    Fixture() : files({dir.path()}) {
        REQUIRE(dir.isValid());
        registerOfficeTools(registry, OfficeServices{
                                          .files = &files,
                                          .commands = &commands,
                                          .appOpened = [this](const OpenedApp &app) { opened.append(app.name); },
                                          .sofficeArguments = {u"-env:UserInstallation=file://%1/profile"_s.arg(dir.path())},
                                          .openFiles = false,
                                      });
    }

    // Runs the tool and waits for its answer.
    Result<ToolResult> create(const QJsonObject &arguments) {
        std::optional<Result<ToolResult>> answer;
        registry.find(u"create_document"_s)->invoke(arguments, [&answer](Result<ToolResult> result) { answer = result; });
        REQUIRE(QTest::qWaitFor([&answer] { return answer.has_value(); }, kConvertWaitMs));
        return *answer;
    }

    QString pathIn(const QString &reply) const {
        // "Created <path> (…)…"
        const QString rest = reply.section(u"Created "_s, 1);
        return rest.left(rest.lastIndexOf(u" ("_s));
    }

    QTemporaryDir dir;
    FileAccess files;
    LocalCommandRunner commands;
    ToolRegistry registry;
    QStringList opened;
};

bool wellFormed(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QXmlStreamReader reader(&file);
    while (!reader.atEnd()) reader.readNext();
    return !reader.hasError();
}

QString soffice() {
    return QStandardPaths::findExecutable(u"soffice"_s);
}

} // namespace

TEST_CASE("create_document writes the three kinds as OpenDocument files, never over an existing one") {
    Fixture f;
    const Result<ToolResult> doc = f.create({{u"kind"_s, u"document"_s},
                                             {u"title"_s, u"Notes"_s},
                                             {u"content"_s, u"# Hello\n\nSome **text**.\n- one\n- two"_s},
                                             {u"folder"_s, f.dir.path()}});
    REQUIRE(doc.isOk());
    CHECK(doc.value().text.contains(u"Created "_s));
    CHECK(doc.value().text.contains(u"(5 words)"_s));
    CHECK(wellFormed(f.dir.filePath(u"Notes.fodt"_s)));
    CHECK(f.opened.isEmpty());  // openFiles is off: nothing was opened

    // The same title again goes to a new name; so does one taken by a link, even a dangling one
    // (writing through it would land outside the folder).
    REQUIRE(f.create({{u"kind"_s, u"document"_s}, {u"title"_s, u"Notes"_s}, {u"content"_s, u"again"_s}, {u"folder"_s, f.dir.path()}})
                .isOk());
    CHECK(QFile::exists(f.dir.filePath(u"Notes (2).fodt"_s)));
    REQUIRE(QFile::link(u"/nonexistent/elsewhere.fodt"_s, f.dir.filePath(u"Linked.fodt"_s)));
    const Result<ToolResult> linked = f.create({{u"kind"_s, u"document"_s}, {u"title"_s, u"Linked"_s}, {u"content"_s, u"x"_s},
                                                {u"folder"_s, f.dir.path()}});
    REQUIRE(linked.isOk());
    CHECK(linked.value().text.contains(u"Linked (2).fodt"_s));
    CHECK(QFileInfo(f.dir.filePath(u"Linked.fodt"_s)).isSymLink());
    CHECK_FALSE(QFile::exists(u"/nonexistent/elsewhere.fodt"_s));

    const QJsonArray slides{QJsonObject{{u"title"_s, u"Solar"_s}},
                            QJsonObject{{u"title"_s, u"Why"_s}, {u"bullets"_s, QJsonArray{u"free"_s, u"clean"_s}}}};
    const Result<ToolResult> deck = f.create({{u"kind"_s, u"presentation"_s},
                                              {u"title"_s, u"Solar energy"_s},
                                              {u"slides"_s, slides},
                                              {u"folder"_s, f.dir.path()}});
    REQUIRE(deck.isOk());
    CHECK(deck.value().text.contains(u"(2 slides)"_s));
    CHECK(wellFormed(f.dir.filePath(u"Solar energy.fodp"_s)));

    const QJsonArray rows{QJsonArray{u"Item"_s, u"Cost"_s}, QJsonArray{u"Rent"_s, 1200}, QJsonArray{u"Food"_s, 300.5},
                          QJsonArray{u"Total"_s, u"=SUM(B2:B3)"_s}};
    const Result<ToolResult> sheet = f.create({{u"kind"_s, u"spreadsheet"_s},
                                               {u"title"_s, u"Budget"_s},
                                               {u"rows"_s, rows},
                                               {u"folder"_s, f.dir.path()}});
    REQUIRE(sheet.isOk());
    CHECK(sheet.value().text.contains(u"(4 rows)"_s));
    CHECK(wellFormed(f.dir.filePath(u"Budget.fods"_s)));

    // What it refuses: a kind it does not know, content missing, a folder outside the roots, a
    // format that does not fit the kind.
    CHECK_FALSE(f.create({{u"kind"_s, u"poster"_s}, {u"title"_s, u"x"_s}}).isOk());
    CHECK_FALSE(f.create({{u"kind"_s, u"presentation"_s}, {u"title"_s, u"x"_s}}).isOk());
    CHECK_FALSE(f.create({{u"kind"_s, u"document"_s}, {u"title"_s, u"x"_s}, {u"content"_s, u"y"_s}, {u"folder"_s, u"/etc"_s}}).isOk());
    const Result<ToolResult> wrong = f.create({{u"kind"_s, u"document"_s}, {u"title"_s, u"x"_s}, {u"content"_s, u"y"_s},
                                               {u"folder"_s, f.dir.path()}, {u"format"_s, u"pptx"_s}});
    REQUIRE_FALSE(wrong.isOk());
    CHECK(wrong.error().message.contains(u"docx or odt"_s));
}

TEST_CASE("LibreOffice reads what create_document writes and computes its formulas") {
    if (soffice().isEmpty()) SKIP("LibreOffice is not installed");
    Fixture f;
    const QJsonArray rows{QJsonArray{u"Item"_s, u"Cost"_s}, QJsonArray{u"Rent"_s, 1200}, QJsonArray{u"Food"_s, 300.5},
                          QJsonArray{u"Total"_s, u"=SUM(B2:B3)"_s}};
    REQUIRE(f.create({{u"kind"_s, u"spreadsheet"_s}, {u"title"_s, u"Budget"_s}, {u"rows"_s, rows}, {u"folder"_s, f.dir.path()}})
                .isOk());

    // LibreOffice itself, headless with a profile of its own, exports the sheet as CSV: the sum
    // shows that the formula was understood and computed on load.
    QProcess convert;
    convert.start(soffice(), {u"-env:UserInstallation=file://%1/profile"_s.arg(f.dir.path()), u"--headless"_s,
                              u"--convert-to"_s, u"csv"_s, u"--outdir"_s, f.dir.path(), f.dir.filePath(u"Budget.fods"_s)});
    REQUIRE(convert.waitForFinished(kConvertWaitMs));
    QFile csv(f.dir.filePath(u"Budget.csv"_s));
    REQUIRE(csv.open(QIODevice::ReadOnly));
    const QString table = QString::fromUtf8(csv.readAll());
    CHECK(table.contains(u"Rent,1200"_s));
    CHECK(table.contains(u"Total,1500.5"_s));

    // And the tool's own conversion: a pdf of the presentation through the same LibreOffice.
    const QJsonArray slides{QJsonObject{{u"title"_s, u"Solar"_s}, {u"bullets"_s, QJsonArray{u"free"_s}}}};
    const Result<ToolResult> pdf = f.create({{u"kind"_s, u"presentation"_s},
                                             {u"title"_s, u"Deck"_s},
                                             {u"slides"_s, slides},
                                             {u"folder"_s, f.dir.path()},
                                             {u"format"_s, u"pdf"_s}});
    REQUIRE(pdf.isOk());
    CHECK(pdf.value().text.contains(u"Deck.pdf"_s));
    CHECK(QFile::exists(f.dir.filePath(u"Deck.pdf"_s)));
    CHECK_FALSE(QFile::exists(f.dir.filePath(u"Deck.fodp"_s)));   // the flat file went with the conversion
    CHECK(QDir(f.dir.path()).entryList({u".nexa-convert-*"_s}, QDir::Dirs | QDir::Hidden).isEmpty());   // and the scratch folder

    // A file of the user's with the converted name is never written over: the new one goes beside it.
    QFile existing(f.dir.filePath(u"Report.pdf"_s));
    REQUIRE(existing.open(QIODevice::WriteOnly));
    existing.write("the user's own report");
    existing.close();
    const Result<ToolResult> report = f.create({{u"kind"_s, u"document"_s},
                                                {u"title"_s, u"Report"_s},
                                                {u"content"_s, u"# New\n\nSomething new."_s},
                                                {u"folder"_s, f.dir.path()},
                                                {u"format"_s, u"pdf"_s}});
    REQUIRE(report.isOk());
    CHECK(report.value().text.contains(u"Report (2).pdf"_s));
    CHECK(QFile::exists(f.dir.filePath(u"Report (2).pdf"_s)));
    REQUIRE(existing.open(QIODevice::ReadOnly));
    CHECK(existing.readAll() == QByteArray("the user's own report"));
}
