#pragma once

#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa {

// Builder of flat OpenDocument files — the XML LibreOffice opens directly as .fodt, .fodp and
// .fods — from plain content: a text document from a small Markdown (headings, paragraphs, bullet
// and numbered lists, **bold** and *italic*), a presentation from slides of a title and bullets, a
// spreadsheet from rows of values and formulas. Pure: strings in, XML out; LibreOffice does the
// rest when the file is opened (it even computes the formulas).
class OdfWriter {
public:
    enum class Kind { Document, Presentation, Spreadsheet };

    struct Slide {
        QString title;
        QStringList bullets;        // none: a title slide
    };

    static QString document(const QString &title, const QString &markdown);
    static QString presentation(const QString &title, const QList<Slide> &slides);
    // Each row a list of cells: a number, a string, a boolean, or a formula ("=SUM(B2:B5)", in the
    // usual A1 notation). The first row is the header.
    static QString spreadsheet(const QString &title, const QString &sheetName, const QList<QList<QJsonValue>> &rows);

    static std::optional<Kind> kindFromString(const QString &kind);   // document | presentation | spreadsheet
    static QString kindString(Kind kind);
    static QString extension(Kind kind);               // fodt | fodp | fods
    static QString componentName(Kind kind);           // LibreOffice Writer | Impress | Calc
    static QString componentId(Kind kind);             // libreoffice-writer.desktop | …
    static QString category(Kind kind);                // WordProcessor | Presentation | Spreadsheet
    // "Solar energy / plan?" → "Solar energy plan.fodp": a name a file system takes.
    static QString fileName(const QString &title, Kind kind);
    // "=SUM(B2:B5)*2" → "of:=SUM([.B2:.B5])*2", the notation ODF keeps a formula in.
    static QString formula(const QString &a1Formula);
    static QString escape(const QString &text);        // XML text and attribute values
};

} // namespace nexa
