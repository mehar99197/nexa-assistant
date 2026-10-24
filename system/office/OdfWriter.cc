#include "system/office/OdfWriter.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QString kNamespaces =
    u"xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
    "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
    "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
    "xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\" "
    "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
    "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
    "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
    "xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" "
    "office:version=\"1.3\""_s;

QString head(const QString &mimetype, const QString &title) {
    return u"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document %1 office:mimetype=\"%2\">\n"
           "<office:meta><dc:title>%3</dc:title><meta:generator>Nexa</meta:generator></office:meta>\n"_s
        .arg(kNamespaces, mimetype, OdfWriter::escape(title));
}

// Bullets and numbers for lists, bold and italic for spans, headings and the title.
const QString kTextStyles =
    u"<office:styles>\n"
    "<style:style style:name=\"Title\" style:family=\"paragraph\" style:class=\"chapter\">"
    "<style:paragraph-properties fo:margin-bottom=\"0.3cm\"/>"
    "<style:text-properties fo:font-size=\"26pt\" fo:font-weight=\"bold\"/></style:style>\n"
    "<style:style style:name=\"Heading_20_1\" style:display-name=\"Heading 1\" style:family=\"paragraph\" style:class=\"text\">"
    "<style:paragraph-properties fo:margin-top=\"0.5cm\" fo:margin-bottom=\"0.2cm\"/>"
    "<style:text-properties fo:font-size=\"18pt\" fo:font-weight=\"bold\"/></style:style>\n"
    "<style:style style:name=\"Heading_20_2\" style:display-name=\"Heading 2\" style:family=\"paragraph\" style:class=\"text\">"
    "<style:paragraph-properties fo:margin-top=\"0.4cm\" fo:margin-bottom=\"0.15cm\"/>"
    "<style:text-properties fo:font-size=\"15pt\" fo:font-weight=\"bold\"/></style:style>\n"
    "<style:style style:name=\"Heading_20_3\" style:display-name=\"Heading 3\" style:family=\"paragraph\" style:class=\"text\">"
    "<style:paragraph-properties fo:margin-top=\"0.3cm\" fo:margin-bottom=\"0.1cm\"/>"
    "<style:text-properties fo:font-size=\"13pt\" fo:font-weight=\"bold\"/></style:style>\n"
    "<style:style style:name=\"Body\" style:family=\"paragraph\" style:class=\"text\">"
    "<style:paragraph-properties fo:margin-bottom=\"0.25cm\" fo:line-height=\"125%\"/></style:style>\n"
    "<style:style style:name=\"Strong\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\"/></style:style>\n"
    "<style:style style:name=\"Emphasis\" style:family=\"text\"><style:text-properties fo:font-style=\"italic\"/></style:style>\n"
    "<text:list-style style:name=\"Bullets\"><text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"•\">"
    "<style:list-level-properties text:space-before=\"0.4cm\" text:min-label-width=\"0.5cm\"/></text:list-level-style-bullet>"
    "</text:list-style>\n"
    "<text:list-style style:name=\"Numbers\"><text:list-level-style-number text:level=\"1\" style:num-format=\"1\" style:num-suffix=\".\">"
    "<style:list-level-properties text:space-before=\"0.4cm\" text:min-label-width=\"0.6cm\"/></text:list-level-style-number>"
    "</text:list-style>\n"
    "</office:styles>\n"_s;

// **bold** and *italic* (or _italic_) inside a line that is already escaped.
QString inlineMarkup(const QString &escaped) {
    QString text = escaped;
    static const QRegularExpression bold(u"\\*\\*(.+?)\\*\\*"_s);
    text.replace(bold, u"<text:span text:style-name=\"Strong\">\\1</text:span>"_s);
    static const QRegularExpression italic(u"(?<![\\w*])[*_](?!\\s)([^*_]+?)(?<!\\s)[*_](?![\\w*])"_s);
    text.replace(italic, u"<text:span text:style-name=\"Emphasis\">\\1</text:span>"_s);
    return text;
}

QString paragraph(const QString &text, const QString &style) {
    return u"<text:p text:style-name=\"%1\">%2</text:p>\n"_s.arg(style, inlineMarkup(OdfWriter::escape(text)));
}

// A1 references and ranges become [.A1] and [.A1:.B2]; text inside double quotes is left alone.
QString translateReferences(const QString &expression) {
    // Not after a digit or a point: the E3 of 1E3 is an exponent, not a cell.
    static const QRegularExpression reference(
        u"(?<![0-9.])(\\$?[A-Za-z]{1,3}\\$?[0-9]{1,7})(?::(\\$?[A-Za-z]{1,3}\\$?[0-9]{1,7}))?(?![A-Za-z0-9_(])"_s);
    QString result;
    const QStringList pieces = expression.split(u'"');
    for (qsizetype i = 0; i < pieces.size(); ++i) {
        if (i > 0) result += u'"';
        if (i % 2 == 1) {   // inside quotes
            result += pieces[i];
            continue;
        }
        QString piece = pieces[i];
        QString rewritten;
        qsizetype last = 0;
        QRegularExpressionMatchIterator it = reference.globalMatch(piece);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            // A function name just before a parenthesis is not a cell: SUM( LOG10( — skipped above
            // by the look-ahead; a plain word like "A1B" is caught by the look-ahead too.
            rewritten += piece.mid(last, m.capturedStart() - last);
            rewritten += m.captured(2).isEmpty() ? u"[.%1]"_s.arg(m.captured(1))
                                                 : u"[.%1:.%2]"_s.arg(m.captured(1), m.captured(2));
            last = m.capturedEnd();
        }
        rewritten += piece.mid(last);
        result += rewritten;
    }
    return result;
}

QString cellXml(const QJsonValue &value, bool header) {
    const QString style = header ? u" table:style-name=\"Head\""_s : QString();
    if (value.isDouble()) {
        const QString number = QString::number(value.toDouble(), 'g', 15);
        return u"<table:table-cell%1 office:value-type=\"float\" office:value=\"%2\"><text:p>%2</text:p></table:table-cell>"_s
            .arg(style, number);
    }
    if (value.isBool()) {
        const QString word = value.toBool() ? u"true"_s : u"false"_s;
        return u"<table:table-cell%1 office:value-type=\"boolean\" office:boolean-value=\"%2\"><text:p>%3</text:p></table:table-cell>"_s
            .arg(style, word, value.toBool() ? u"TRUE"_s : u"FALSE"_s);
    }
    if (value.isString()) {
        const QString text = value.toString();
        if (text.startsWith(u'=') && text.size() > 1) {
            return u"<table:table-cell%1 table:formula=\"%2\"/>"_s.arg(style, OdfWriter::escape(OdfWriter::formula(text)));
        }
        if (text.isEmpty()) return u"<table:table-cell%1/>"_s.arg(style);
        return u"<table:table-cell%1 office:value-type=\"string\"><text:p>%2</text:p></table:table-cell>"_s
            .arg(style, OdfWriter::escape(text));
    }
    return u"<table:table-cell%1/>"_s.arg(style);
}

} // namespace

QString OdfWriter::document(const QString &title, const QString &markdown) {
    QString body;
    QStringList paragraphLines;
    QString openList;   // "Bullets" | "Numbers" | empty
    const auto flushParagraph = [&] {
        if (paragraphLines.isEmpty()) return;
        body += paragraph(paragraphLines.join(u' '), u"Body"_s);
        paragraphLines.clear();
    };
    const auto closeList = [&] {
        if (openList.isEmpty()) return;
        body += u"</text:list>\n"_s;
        openList.clear();
    };
    const auto listItem = [&](const QString &style, const QString &text) {
        flushParagraph();
        if (openList != style) {
            closeList();
            body += u"<text:list text:style-name=\"%1\">\n"_s.arg(style);
            openList = style;
        }
        body += u"<text:list-item>"_s + paragraph(text, u"Body"_s).trimmed() + u"</text:list-item>\n"_s;
    };

    static const QRegularExpression heading(u"^(#{1,3})\\s+(.*)$"_s);
    static const QRegularExpression bullet(u"^[-*•]\\s+(.*)$"_s);
    static const QRegularExpression numbered(u"^\\d{1,3}[.)]\\s+(.*)$"_s);
    for (const QString &rawLine : markdown.split(u'\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            flushParagraph();
            closeList();
            continue;
        }
        if (const QRegularExpressionMatch m = heading.match(line); m.hasMatch()) {
            flushParagraph();
            closeList();
            const int level = int(m.captured(1).size());
            body += u"<text:h text:style-name=\"Heading_20_%1\" text:outline-level=\"%1\">%2</text:h>\n"_s
                        .arg(level).arg(inlineMarkup(escape(m.captured(2).trimmed())));
            continue;
        }
        if (const QRegularExpressionMatch m = bullet.match(line); m.hasMatch()) {
            listItem(u"Bullets"_s, m.captured(1).trimmed());
            continue;
        }
        if (const QRegularExpressionMatch m = numbered.match(line); m.hasMatch()) {
            listItem(u"Numbers"_s, m.captured(1).trimmed());
            continue;
        }
        if (!openList.isEmpty()) closeList();
        paragraphLines.append(line);
    }
    flushParagraph();
    closeList();

    return head(u"application/vnd.oasis.opendocument.text"_s, title) + kTextStyles
           + u"<office:body><office:text>\n"_s + paragraph(title, u"Title"_s) + body
           + u"</office:text></office:body>\n</office:document>\n"_s;
}

QString OdfWriter::presentation(const QString &title, const QList<Slide> &slides) {
    QString xml = head(u"application/vnd.oasis.opendocument.presentation"_s, title);
    xml += u"<office:styles>\n"
           "<text:list-style style:name=\"Bullets\"><text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"•\">"
           "<style:list-level-properties text:space-before=\"0.6cm\" text:min-label-width=\"0.8cm\"/>"
           "</text:list-level-style-bullet></text:list-style>\n"
           "</office:styles>\n"
           "<office:automatic-styles>\n"
           "<style:page-layout style:name=\"Wide\"><style:page-layout-properties fo:page-width=\"28cm\" "
           "fo:page-height=\"15.75cm\" fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\" fo:margin-left=\"0cm\" "
           "fo:margin-right=\"0cm\" style:print-orientation=\"landscape\"/></style:page-layout>\n"
           "<style:style style:name=\"Page\" style:family=\"drawing-page\"><style:drawing-page-properties "
           "presentation:background-visible=\"true\" presentation:background-objects-visible=\"true\"/></style:style>\n"
           "<style:style style:name=\"SlideTitle\" style:family=\"paragraph\"><style:text-properties "
           "fo:font-size=\"36pt\" fo:font-weight=\"bold\"/></style:style>\n"
           "<style:style style:name=\"BigTitle\" style:family=\"paragraph\"><style:paragraph-properties "
           "fo:text-align=\"center\"/><style:text-properties fo:font-size=\"44pt\" fo:font-weight=\"bold\"/></style:style>\n"
           "<style:style style:name=\"Bullet\" style:family=\"paragraph\"><style:paragraph-properties "
           "fo:margin-bottom=\"0.3cm\"/><style:text-properties fo:font-size=\"22pt\"/></style:style>\n"
           "</office:automatic-styles>\n"
           "<office:master-styles><style:master-page style:name=\"Default\" style:page-layout-name=\"Wide\" "
           "draw:style-name=\"Page\"/></office:master-styles>\n"
           "<office:body><office:presentation>\n"_s;
    int number = 0;
    for (const Slide &slide : slides) {
        ++number;
        xml += u"<draw:page draw:name=\"page%1\" draw:master-page-name=\"Default\" draw:style-name=\"Page\">\n"_s.arg(number);
        if (slide.bullets.isEmpty()) {
            xml += u"<draw:frame svg:width=\"26cm\" svg:height=\"5cm\" svg:x=\"1cm\" svg:y=\"5.4cm\"><draw:text-box>"
                   "<text:p text:style-name=\"BigTitle\">%1</text:p></draw:text-box></draw:frame>\n"_s
                       .arg(escape(slide.title));
        } else {
            xml += u"<draw:frame svg:width=\"26cm\" svg:height=\"3cm\" svg:x=\"1cm\" svg:y=\"0.9cm\"><draw:text-box>"
                   "<text:p text:style-name=\"SlideTitle\">%1</text:p></draw:text-box></draw:frame>\n"_s
                       .arg(escape(slide.title));
            xml += u"<draw:frame svg:width=\"26cm\" svg:height=\"10.5cm\" svg:x=\"1cm\" svg:y=\"4.4cm\"><draw:text-box>"
                   "<text:list text:style-name=\"Bullets\">\n"_s;
            for (const QString &bullet : slide.bullets) {
                xml += u"<text:list-item><text:p text:style-name=\"Bullet\">%1</text:p></text:list-item>\n"_s
                           .arg(escape(bullet));
            }
            xml += u"</text:list></draw:text-box></draw:frame>\n"_s;
        }
        xml += u"</draw:page>\n"_s;
    }
    xml += u"</office:presentation></office:body>\n</office:document>\n"_s;
    return xml;
}

QString OdfWriter::spreadsheet(const QString &title, const QString &sheetName, const QList<QList<QJsonValue>> &rows) {
    qsizetype columns = 1;
    for (const QList<QJsonValue> &row : rows) columns = std::max(columns, row.size());
    QString xml = head(u"application/vnd.oasis.opendocument.spreadsheet"_s, title);
    xml += u"<office:automatic-styles>\n"
           "<style:style style:name=\"Head\" style:family=\"table-cell\"><style:text-properties fo:font-weight=\"bold\"/></style:style>\n"
           "<style:style style:name=\"Col\" style:family=\"table-column\"><style:table-column-properties "
           "style:column-width=\"4cm\"/></style:style>\n"
           "</office:automatic-styles>\n"
           "<office:body><office:spreadsheet>\n"_s;
    xml += u"<table:table table:name=\"%1\">\n<table:table-column table:style-name=\"Col\" table:number-columns-repeated=\"%2\"/>\n"_s
               .arg(escape(sheetName.isEmpty() ? title : sheetName))
               .arg(columns);
    bool header = true;
    for (const QList<QJsonValue> &row : rows) {
        xml += u"<table:table-row>"_s;
        for (const QJsonValue &cell : row) xml += cellXml(cell, header);
        xml += u"</table:table-row>\n"_s;
        header = false;
    }
    xml += u"</table:table>\n</office:spreadsheet></office:body>\n</office:document>\n"_s;
    return xml;
}

std::optional<OdfWriter::Kind> OdfWriter::kindFromString(const QString &kind) {
    const QString word = kind.trimmed().toLower();
    if (word == u"document"_s || word == u"text"_s || word == u"doc"_s) return Kind::Document;
    if (word == u"presentation"_s || word == u"slides"_s || word == u"ppt"_s) return Kind::Presentation;
    if (word == u"spreadsheet"_s || word == u"sheet"_s || word == u"table"_s) return Kind::Spreadsheet;
    return std::nullopt;
}

QString OdfWriter::kindString(Kind kind) {
    switch (kind) {
    case Kind::Document: return u"document"_s;
    case Kind::Presentation: return u"presentation"_s;
    case Kind::Spreadsheet: return u"spreadsheet"_s;
    }
    return {};
}

QString OdfWriter::extension(Kind kind) {
    switch (kind) {
    case Kind::Document: return u"fodt"_s;
    case Kind::Presentation: return u"fodp"_s;
    case Kind::Spreadsheet: return u"fods"_s;
    }
    return {};
}

QString OdfWriter::componentName(Kind kind) {
    switch (kind) {
    case Kind::Document: return u"LibreOffice Writer"_s;
    case Kind::Presentation: return u"LibreOffice Impress"_s;
    case Kind::Spreadsheet: return u"LibreOffice Calc"_s;
    }
    return {};
}

QString OdfWriter::componentId(Kind kind) {
    switch (kind) {
    case Kind::Document: return u"libreoffice-writer.desktop"_s;
    case Kind::Presentation: return u"libreoffice-impress.desktop"_s;
    case Kind::Spreadsheet: return u"libreoffice-calc.desktop"_s;
    }
    return {};
}

QString OdfWriter::category(Kind kind) {
    switch (kind) {
    case Kind::Document: return u"WordProcessor"_s;
    case Kind::Presentation: return u"Presentation"_s;
    case Kind::Spreadsheet: return u"Spreadsheet"_s;
    }
    return {};
}

QString OdfWriter::fileName(const QString &title, Kind kind) {
    static const QRegularExpression unsafe(u"[\\\\/:*?\"<>|\\p{Cc}]+"_s);
    static const QRegularExpression spaces(u"\\s+"_s);
    QString name = title;
    name.replace(unsafe, u" "_s);
    name = name.replace(spaces, u" "_s).trimmed();
    if (name.size() > 80) name = name.left(80).trimmed();
    while (name.endsWith(u'.')) name.chop(1);
    if (name.isEmpty()) name = u"Untitled"_s;
    return name + u'.' + extension(kind);
}

QString OdfWriter::formula(const QString &a1Formula) {
    QString expression = a1Formula.trimmed();
    if (expression.startsWith(u'=')) expression.remove(0, 1);
    return u"of:="_s + translateReferences(expression);
}

QString OdfWriter::escape(const QString &text) {
    QString out = text;
    out.replace(u'&', u"&amp;"_s);
    out.replace(u'<', u"&lt;"_s);
    out.replace(u'>', u"&gt;"_s);
    out.replace(u'"', u"&quot;"_s);
    out.replace(u'\'', u"&apos;"_s);
    return out;
}

} // namespace nexa
