#include "core/skills/SkillParser.h"

#include <QRegularExpression>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// A quoted YAML scalar, or a plain one; what YAML calls a comment is kept, as Claude's own loader does.
QString scalar(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && value.front() == u'"' && value.back() == u'"') {
        QString out;
        const QString inner = value.mid(1, value.size() - 2);
        for (qsizetype i = 0; i < inner.size(); ++i) {
            if (inner.at(i) == u'\\' && i + 1 < inner.size()) {
                const QChar next = inner.at(++i);
                out += next == u'n' ? QChar(u'\n') : next == u't' ? QChar(u'\t') : next;
            } else {
                out += inner.at(i);
            }
        }
        return out;
    }
    if (value.size() >= 2 && value.front() == u'\'' && value.back() == u'\'') {
        return value.mid(1, value.size() - 2).replace(u"''"_s, u"'"_s);
    }
    return value;
}

} // namespace

bool SkillParser::isValidName(const QString &name) {
    static const QRegularExpression valid(u"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$"_s);
    return valid.match(name).hasMatch();
}

Result<SkillDocument> SkillParser::parse(const QString &input) {
    QString text = input;
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    text.replace(u"\r\n"_s, u"\n"_s);
    QStringList lines = text.split(u'\n');
    if (lines.isEmpty() || lines.first().trimmed() != u"---"_s) {
        return Error{ErrorCode::Parse, u"SKILL.md must start with a \"---\" line and its name and description"_s};
    }
    qsizetype end = -1;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == u"---"_s) {
            end = i;
            break;
        }
    }
    if (end < 0) return Error{ErrorCode::Parse, u"The front matter of SKILL.md is not closed by a \"---\" line"_s};

    static const QRegularExpression keyLine(u"^([A-Za-z0-9_-]+)\\s*:\\s*(.*)$"_s);
    SkillDocument document;
    for (qsizetype i = 1; i < end; ++i) {
        const QString line = lines.at(i);
        if (line.isEmpty() || line.front().isSpace() || line.trimmed().startsWith(u'#')) continue;   // nested, blank, comment
        const QRegularExpressionMatch m = keyLine.match(line);
        if (!m.hasMatch()) continue;
        const QString key = m.captured(1);
        QString value = m.captured(2).trimmed();
        if (value.startsWith(u'|') || value.startsWith(u'>')) {
            // A block: the indented lines after it, joined by newlines (|) or spaces (>).
            const bool folded = value.startsWith(u'>');
            QStringList block;
            while (i + 1 < end && (lines.at(i + 1).isEmpty() || lines.at(i + 1).front().isSpace())) {
                block.append(lines.at(++i).trimmed());
            }
            while (!block.isEmpty() && block.last().isEmpty()) block.removeLast();
            value = block.join(folded ? u" "_s : u"\n"_s).trimmed();
        } else {
            value = scalar(value);
        }
        if (key == u"name"_s) document.name = value.trimmed();
        else if (key == u"description"_s) document.description = value.simplified();
    }
    if (document.name.isEmpty()) return Error{ErrorCode::Parse, u"SKILL.md has no name"_s};
    if (!isValidName(document.name)) {
        return Error{ErrorCode::Parse,
                     u"\"%1\" is not a skill name: lower-case letters, digits and hyphens, at most 64"_s.arg(document.name)};
    }
    if (document.description.isEmpty()) return Error{ErrorCode::Parse, u"SKILL.md has no description"_s};
    if (document.description.size() > kMaxDescription) {
        return Error{ErrorCode::Parse, u"The description is longer than %1 characters"_s.arg(kMaxDescription)};
    }
    document.body = lines.mid(end + 1).join(u'\n').trimmed();
    return document;
}

QString SkillParser::compose(const SkillDocument &document) {
    QString description = document.description.simplified();
    description.replace(u'\\', u"\\\\"_s);
    description.replace(u'"', u"\\\""_s);
    return u"---\nname: %1\ndescription: \"%2\"\n---\n\n%3\n"_s.arg(document.name, description, document.body.trimmed());
}

} // namespace nexa
