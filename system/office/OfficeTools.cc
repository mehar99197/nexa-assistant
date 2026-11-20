#include "system/office/OfficeTools.h"

#include "core/tools/FunctionTool.h"
#include "core/tools/ToolArguments.h"
#include "core/tools/ToolRegistry.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/files/FileAccess.h"
#include "system/office/OdfWriter.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

using namespace toolargs;
using Kind = OdfWriter::Kind;

constexpr int kConvertTimeoutMs = 120'000;

QJsonObject arrayOf(const QJsonObject &items, const QString &description) {
    return QJsonObject{{u"type"_s, u"array"_s}, {u"items"_s, items}, {u"description"_s, description}};
}

// A path a shell takes as one word.
QString quoted(const QString &path) {
#ifdef Q_OS_WIN
    QString inner = QDir::toNativeSeparators(path);
    inner.replace(u'"', u"\\\""_s);
    return u'"' + inner + u'"';
#else
    QString inner = path;
    inner.replace(u'\'', u"'\\''"_s);
    return u'\'' + inner + u'\'';
#endif
}

QString findSoffice() {
    QStringList extraDirs;
#ifdef Q_OS_WIN
    for (const QString &base : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"),
                                qEnvironmentVariable("LOCALAPPDATA") + u"/Programs"_s}) {
        if (!base.isEmpty()) extraDirs.append(base + u"/LibreOffice/program"_s);
    }
#endif
    for (const QString &name : {u"soffice"_s, u"libreoffice"_s}) {
        QString found = QStandardPaths::findExecutable(name);
        if (found.isEmpty() && !extraDirs.isEmpty()) found = QStandardPaths::findExecutable(name, extraDirs);
        if (!found.isEmpty()) return found;
    }
    return {};
}

QString defaultFolder() {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!documents.isEmpty() && QDir(documents).exists()) return documents;
    return QDir::homePath();
}

// "Plan.fodp" is taken: "Plan (2).fodp", "Plan (3).fodp", … — nothing of the user's is overwritten.
// A symbolic link counts as taken even when it dangles: writing through it would land elsewhere.
QString freeName(const QString &directory, const QString &fileName) {
    const QFileInfo info(fileName);
    const auto taken = [&directory](const QString &name) {
        const QFileInfo candidate(directory + u'/' + name);
        return candidate.exists() || candidate.isSymLink();
    };
    QString candidate = fileName;
    for (int n = 2; taken(candidate) && n < 1000; ++n) {
        candidate = u"%1 (%2).%3"_s.arg(info.completeBaseName()).arg(n).arg(info.suffix());
    }
    return candidate;
}

QStringList stringsOf(const QJsonValue &value) {
    QStringList strings;
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.isString() ? item.toString().trimmed() : QString();
        if (!text.isEmpty()) strings.append(text);
    }
    return strings;
}

struct Built {
    QString xml;
    QString summary;        // "3 slides", "12 rows", "240 words"
};

Result<Built> build(Kind kind, const QString &title, const QJsonObject &arguments) {
    switch (kind) {
    case Kind::Document: {
        const QString content = arguments.value(u"content"_s).toString().trimmed();
        if (content.isEmpty()) return Error{ErrorCode::InvalidArgument, u"A document needs content (its text, in Markdown)."_s};
        // Words, not markup: "#", "-" and "1." are not counted.
        static const QRegularExpression wordLike(u"[\\p{L}\\p{N}]"_s);
        qsizetype words = 0;
        for (const QString &token : content.split(QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts)) {
            if (wordLike.match(token).hasMatch()) ++words;
        }
        return Built{OdfWriter::document(title, content), u"%1 words"_s.arg(words)};
    }
    case Kind::Presentation: {
        QList<OdfWriter::Slide> slides;
        for (const QJsonValue &value : arguments.value(u"slides"_s).toArray()) {
            const QJsonObject slide = value.toObject();
            const QString slideTitle = slide.value(u"title"_s).toString().trimmed();
            const QStringList bullets = stringsOf(slide.value(u"bullets"_s));
            if (slideTitle.isEmpty() && bullets.isEmpty()) continue;
            slides.append({slideTitle.isEmpty() ? title : slideTitle, bullets});
        }
        if (slides.isEmpty()) {
            return Error{ErrorCode::InvalidArgument,
                         u"A presentation needs slides: a list of {\"title\": …, \"bullets\": [ … ]}."_s};
        }
        return Built{OdfWriter::presentation(title, slides), u"%1 slides"_s.arg(slides.size())};
    }
    case Kind::Spreadsheet: {
        QList<QList<QJsonValue>> rows;
        for (const QJsonValue &value : arguments.value(u"rows"_s).toArray()) {
            QList<QJsonValue> row;
            for (const QJsonValue &cell : value.toArray()) row.append(cell);
            rows.append(row);
        }
        if (rows.isEmpty()) {
            return Error{ErrorCode::InvalidArgument,
                         u"A spreadsheet needs rows: a list of rows, each a list of cells (the first row is the header)."_s};
        }
        const QString sheet = arguments.value(u"sheet"_s).toString().trimmed();
        return Built{OdfWriter::spreadsheet(title, sheet, rows), u"%1 rows"_s.arg(rows.size())};
    }
    }
    return Error{ErrorCode::InvalidArgument, u"Unknown kind."_s};
}

// The export LibreOffice is asked for, checked against the kind: a document cannot become a pptx.
Result<QString> exportFormat(Kind kind, const QString &format) {
    const QString word = format.trimmed().toLower();
    if (word.isEmpty() || word == u"odf"_s) return QString();
    if (word == u"pdf"_s) return word;
    const bool fits = (kind == Kind::Document && (word == u"docx"_s || word == u"odt"_s))
                      || (kind == Kind::Presentation && (word == u"pptx"_s || word == u"odp"_s))
                      || (kind == Kind::Spreadsheet && (word == u"xlsx"_s || word == u"ods"_s));
    if (!fits) {
        return Error{ErrorCode::InvalidArgument,
                     u"A %1 cannot be saved as %2: use odf (the default), pdf, or %3."_s.arg(
                         OdfWriter::kindString(kind), word,
                         kind == Kind::Document       ? u"docx or odt"_s
                         : kind == Kind::Presentation ? u"pptx or odp"_s
                                                      : u"xlsx or ods"_s)};
    }
    return word;
}

} // namespace

void registerOfficeTools(ToolRegistry &registry, const OfficeServices &given) {
    OfficeServices services = given;
    if (services.soffice.isEmpty()) services.soffice = findSoffice();

    const QJsonObject slideSchema{
        {u"type"_s, u"object"_s},
        {u"properties"_s, QJsonObject{{u"title"_s, schema::string(u"The slide's title"_s)},
                                      {u"bullets"_s, arrayOf(QJsonObject{{u"type"_s, u"string"_s}},
                                                             u"The slide's points, one string each (none: a title slide)"_s)}}},
    };
    const QJsonObject cellSchema{{u"description"_s, u"A number, a string, true/false, or a formula such as \"=SUM(B2:B9)\""_s}};
    ToolSpec spec{
        u"create_document"_s,
        u"Create a text document, a presentation (slides) or a spreadsheet as a file and open it in LibreOffice "
        "Writer, Impress or Calc. Give the content itself — never type it through the keyboard. A document takes "
        "`content` in Markdown (# headings, paragraphs, - bullets, 1. numbered lists, **bold**, *italic*); a "
        "presentation takes `slides`; a spreadsheet takes `rows` (the first row is the header; formulas in A1 "
        "notation are computed by LibreOffice). The file goes to the user's Documents folder unless `folder` says "
        "otherwise, as OpenDocument unless `format` asks for docx, pptx, xlsx or pdf."_s,
        schema::object(
            {{u"kind"_s, schema::choice(u"What to make"_s, {u"document"_s, u"presentation"_s, u"spreadsheet"_s})},
             {u"title"_s, schema::string(u"The title; also the file's name"_s)},
             {u"content"_s, schema::string(u"document: the text, in Markdown"_s)},
             {u"slides"_s, arrayOf(slideSchema, u"presentation: the slides in order"_s)},
             {u"rows"_s, arrayOf(arrayOf(cellSchema, u"one row: its cells left to right"_s),
                                 u"spreadsheet: the rows top to bottom, the first one the header"_s)},
             {u"sheet"_s, schema::string(u"spreadsheet: the sheet's name (default: the title)"_s)},
             {u"folder"_s, schema::string(u"Where to save it (default: ~/Documents)"_s)},
             {u"format"_s, schema::choice(u"The file type (default odf: fodt/fodp/fods, which LibreOffice opens directly)"_s,
                                          {u"odf"_s, u"docx"_s, u"pptx"_s, u"xlsx"_s, u"pdf"_s, u"odt"_s, u"odp"_s, u"ods"_s})},
             {u"open"_s, schema::boolean(u"Open it in LibreOffice once written (default true)"_s)}},
            {u"kind"_s, u"title"_s}),
    };

    registry.add(std::make_unique<FunctionTool>(spec, [services](const QJsonObject &arguments, ITool::Done done) {
        const std::optional<Kind> kind = OdfWriter::kindFromString(arguments.value(u"kind"_s).toString());
        if (!kind) {
            done(Error{ErrorCode::InvalidArgument, u"kind must be document, presentation or spreadsheet."_s});
            return;
        }
        const Result<QString> title = requiredString(arguments, u"title"_s);
        if (!title) {
            done(title.error());
            return;
        }
        const Result<Built> built = build(*kind, title.value(), arguments);
        if (!built) {
            done(built.error());
            return;
        }
        const Result<QString> format = exportFormat(*kind, arguments.value(u"format"_s).toString());
        if (!format) {
            done(format.error());
            return;
        }

        // Where it goes: inside the allowed roots, under a name that is still free.
        const QString folderArgument = arguments.value(u"folder"_s).toString().trimmed();
        const Result<QString> folder = services.files->resolve(folderArgument.isEmpty() ? defaultFolder() : folderArgument);
        if (!folder) {
            done(folder.error());
            return;
        }
        QDir().mkpath(folder.value());
        const QString wantedName = OdfWriter::fileName(title.value(), *kind);   // "Budget.fods"

        const bool open = boolArg(arguments, u"open"_s).value_or(true) && services.openFiles;
        const QString component = OdfWriter::componentName(*kind);
        const auto finish = [services, kind, component, open, summary = built.value().summary, done](const QString &path) {
            QString report = u"Created %1 (%2)"_s.arg(path, summary);
            if (open && path.endsWith(u".pdf"_s, Qt::CaseInsensitive)) {
                // A pdf is for reading: the user's own viewer, not LibreOffice (which would open it
                // for editing in Draw) — and no office app is in front afterwards.
                report += QDesktopServices::openUrl(QUrl::fromLocalFile(path)) ? u" and opened it in your PDF viewer."_s
                                                                               : u". No PDF viewer could be started."_s;
            } else if (open) {
                if (services.soffice.isEmpty()) {
                    report += u". LibreOffice is not installed, so it was not opened."_s;
                } else if (QProcess::startDetached(services.soffice, services.sofficeArguments + QStringList{path})) {
                    report += u" and opened it in %1."_s.arg(component);
                    if (services.appOpened) {
                        services.appOpened(OpenedApp{component, OdfWriter::componentId(*kind), u"soffice"_s,
                                                     {u"Office"_s, OdfWriter::category(*kind)}});
                    }
                } else {
                    report += u". LibreOffice could not be started to open it."_s;
                }
            } else {
                report += u'.';
            }
            done(text(report));
        };

        if (format.value().isEmpty()) {
            const QString flatPath = folder.value() + u'/' + freeName(folder.value(), wantedName);
            if (const Result<qint64> written = services.files->write(flatPath, built.value().xml, false); !written) {
                done(written.error());
                return;
            }
            finish(flatPath);
            return;
        }

        // Another file type: LibreOffice converts the flat file in a scratch folder of its own, and
        // the result moves into place under a name that is still free — nothing of the user's is
        // ever written over, and a conversion that produced nothing cannot be mistaken for a file
        // that was there before.
        if (services.soffice.isEmpty() || !services.commands) {
            done(Error{ErrorCode::NotSupported, u"LibreOffice is needed to make a %1; without it, ask for the default "
                                                "format."_s.arg(format.value())});
            return;
        }
        auto scratch = std::make_shared<QTemporaryDir>(folder.value() + u"/.nexa-convert-XXXXXX"_s);
        if (!scratch->isValid()) {
            done(Error{ErrorCode::Io, u"Could not make a scratch folder in %1."_s.arg(folder.value())});
            return;
        }
        const QString flatPath = scratch->path() + u'/' + wantedName;
        if (const Result<qint64> written = services.files->write(flatPath, built.value().xml, false); !written) {
            done(written.error());
            return;
        }
        QStringList words{quoted(services.soffice)};
        for (const QString &argument : services.sofficeArguments) words.append(quoted(argument));
        words += {u"--headless"_s, u"--convert-to"_s, format.value(), u"--outdir"_s, quoted(scratch->path()), quoted(flatPath)};
        const QString base = QFileInfo(wantedName).completeBaseName();
        const QString produced = scratch->path() + u'/' + base + u'.' + format.value();
        services.commands->run(words.join(u' '), scratch->path(), kConvertTimeoutMs,
                               [scratch, finish, done, produced, base, folder = folder.value(), format = format.value()](
                                   Result<CommandOutput> result) {
            if (!result) {
                done(result.error());
                return;
            }
            if (!QFileInfo::exists(produced)) {
                const CommandOutput &run = result.value();
                done(Error{ErrorCode::Io,
                           run.timedOut ? u"LibreOffice did not finish converting to %1 within %2 s."_s.arg(format).arg(kConvertTimeoutMs / 1000)
                           : run.crashed ? u"LibreOffice crashed while converting to %1."_s.arg(format)
                                         : u"LibreOffice did not produce the %1: %2"_s.arg(format, run.output.trimmed())});
                return;
            }
            // QFile::rename refuses a destination that exists, so the free name found a moment ago
            // is the one taken.
            const QString finalPath = folder + u'/' + freeName(folder, base + u'.' + format);
            if (!QFile::rename(produced, finalPath)) {
                done(Error{ErrorCode::Io, u"Could not move the %1 into %2."_s.arg(format, folder)});
                return;
            }
            finish(finalPath);
        });
    }));
}

} // namespace nexa
