#include "system/apps/DesktopEntry.h"

#include <QHash>
#include <QLocale>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// String escapes of the spec: \s \n \t \r \\ (and \; inside lists, handled by splitList).
QString unescape(const QString &value) {
    QString result;
    result.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c != u'\\' || i + 1 == value.size()) {
            result.append(c);
            continue;
        }
        const QChar next = value.at(++i);
        switch (next.unicode()) {
        case u's': result.append(u' '); break;
        case u'n': result.append(u'\n'); break;
        case u't': result.append(u'\t'); break;
        case u'r': result.append(u'\r'); break;
        case u'\\': result.append(u'\\'); break;
        default: result.append(u'\\').append(next); break;
        }
    }
    return result;
}

QStringList splitList(const QString &value) {
    QStringList items;
    QString current;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == u'\\' && i + 1 < value.size() && value.at(i + 1) == u';') {
            current.append(u';');
            ++i;
        } else if (c == u';') {
            items.append(current);
            current.clear();
        } else {
            current.append(c);
        }
    }
    items.append(current);
    QStringList cleaned;
    for (const QString &item : std::as_const(items)) {
        const QString text = unescape(item).trimmed();
        if (!text.isEmpty()) cleaned.append(text);
    }
    return cleaned;
}

bool isTrue(const QString &value) {
    return value.trimmed().compare(u"true"_s, Qt::CaseInsensitive) == 0;
}

void assign(DesktopEntry &entry, QString &type, const QString &key, const QString &value) {
    if (key == u"Type") type = value;
    else if (key == u"Name") entry.name = unescape(value);
    else if (key == u"GenericName") entry.genericName = unescape(value);
    else if (key == u"Comment") entry.comment = unescape(value);
    else if (key == u"Keywords") entry.keywords = splitList(value);
    else if (key == u"Categories") entry.categories = splitList(value);
    else if (key == u"Exec") entry.exec = unescape(value);
    else if (key == u"TryExec") entry.tryExec = unescape(value);
    else if (key == u"Icon") entry.icon = unescape(value);
    else if (key == u"OnlyShowIn") entry.onlyShowIn = splitList(value);
    else if (key == u"NotShowIn") entry.notShowIn = splitList(value);
    else if (key == u"Terminal") entry.terminal = isTrue(value);
    else if (key == u"DBusActivatable") entry.dbusActivatable = isTrue(value);
    else if (key == u"NoDisplay") entry.noDisplay = isTrue(value);
    else if (key == u"Hidden") entry.hidden = isTrue(value);
}

bool isFieldCode(const QString &argument) {
    static const QRegularExpression code(u"^%[fFuUdDnNickvm]$"_s);
    return code.match(argument).hasMatch();
}

QString basename(const QString &path) {
    return path.section(u'/', -1).section(u'\\', -1);
}

} // namespace

std::optional<DesktopEntry> DesktopEntry::parse(const QByteArray &content, const QString &id, const QString &path,
                                                const QStringList &locales) {
    DesktopEntry entry;
    entry.id = id;
    entry.path = path;
    QString type;
    bool inMainGroup = false;
    bool sawMainGroup = false;
    QHash<QString, qsizetype> rankOfKey;  // locale rank of the value kept for each key (lower is better)

    const QStringList lines = QString::fromUtf8(content).split(u'\n');
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) continue;
        if (line.startsWith(u'[')) {
            inMainGroup = line == u"[Desktop Entry]";
            sawMainGroup = sawMainGroup || inMainGroup;
            continue;
        }
        if (!inMainGroup) continue;

        const qsizetype equals = line.indexOf(u'=');
        if (equals <= 0) continue;
        QString key = line.left(equals).trimmed();
        const QString value = line.mid(equals + 1).trimmed();

        qsizetype rank = locales.size();  // unlocalized value
        if (key.endsWith(u']')) {
            const qsizetype bracket = key.indexOf(u'[');
            if (bracket <= 0) continue;
            rank = locales.indexOf(key.mid(bracket + 1, key.size() - bracket - 2));
            if (rank < 0) continue;  // a locale we do not want
            key = key.left(bracket);
        }
        if (const auto kept = rankOfKey.constFind(key); kept != rankOfKey.constEnd() && *kept <= rank) continue;
        rankOfKey.insert(key, rank);
        assign(entry, type, key, value);
    }

    if (!sawMainGroup || type != u"Application") return std::nullopt;
    return entry;
}

QStringList DesktopEntry::execArguments(const QString &exec) {
    QStringList arguments;
    QString current;
    bool inQuotes = false;
    bool hasArgument = false;
    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar c = exec.at(i);
        if (inQuotes) {
            if (c == u'\\' && i + 1 < exec.size() && QStringView(u"\"`$\\").contains(exec.at(i + 1))) {
                current.append(exec.at(++i));
            } else if (c == u'"') {
                inQuotes = false;
            } else {
                current.append(c);
            }
            continue;
        }
        if (c == u'"') {
            inQuotes = true;
            hasArgument = true;
        } else if (c.isSpace()) {
            if (hasArgument) arguments.append(current);
            current.clear();
            hasArgument = false;
        } else {
            current.append(c);
            hasArgument = true;
        }
    }
    if (inQuotes) return {};
    if (hasArgument) arguments.append(current);

    QStringList result;
    for (QString argument : std::as_const(arguments)) {
        if (isFieldCode(argument)) continue;
        // Field codes embedded in an argument are dropped too; %% is a literal percent sign.
        QString expanded;
        for (qsizetype i = 0; i < argument.size(); ++i) {
            if (argument.at(i) == u'%' && i + 1 < argument.size()) {
                if (argument.at(i + 1) == u'%') expanded.append(u'%');
                ++i;
            } else {
                expanded.append(argument.at(i));
            }
        }
        if (!expanded.isEmpty()) result.append(expanded);
    }
    return result;
}

QStringList DesktopEntry::preferredLocales() {
    const QString name = QLocale::system().name();  // e.g. "de_DE"
    QStringList locales{name};
    const QString language = name.section(u'_', 0, 0);
    if (!language.isEmpty() && language != name) locales.append(language);
    return locales;
}

QString DesktopEntry::executableName() const {
    const QStringList argv = execArguments(exec);
    qsizetype i = 0;
    if (i < argv.size() && basename(argv.at(i)) == u"env") {
        ++i;
        while (i < argv.size() && (argv.at(i).contains(u'=') || argv.at(i).startsWith(u'-'))) ++i;
    }
    return i < argv.size() ? basename(argv.at(i)) : QString();
}

QString DesktopEntry::flatpakAppId() const {
    const QStringList argv = execArguments(exec);
    if (argv.isEmpty() || basename(argv.first()) != u"flatpak") return {};
    const qsizetype run = argv.indexOf(u"run"_s);
    if (run < 0) return {};
    for (qsizetype i = run + 1; i < argv.size(); ++i) {
        if (!argv.at(i).startsWith(u'-')) return argv.at(i);
    }
    return {};
}

bool DesktopEntry::isShownIn(const QStringList &desktops) const {
    if (hidden) return false;
    const auto shownOn = [&desktops](const QStringList &list) {
        for (const QString &desktop : desktops) {
            if (list.contains(desktop, Qt::CaseInsensitive)) return true;
        }
        return false;
    };
    if (!onlyShowIn.isEmpty() && !shownOn(onlyShowIn)) return false;
    return !shownOn(notShowIn);
}

} // namespace nexa
