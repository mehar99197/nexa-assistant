#include "system/android/UiHierarchy.h"

#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa::uihierarchy {

namespace {

bool flag(const QXmlStreamAttributes &attributes, QLatin1StringView name) {
    return attributes.value(name) == u"true"_s;
}

QRect parseBounds(QStringView bounds) {
    static const QRegularExpression shape(u"^\\[(-?\\d+),(-?\\d+)\\]\\[(-?\\d+),(-?\\d+)\\]$"_s);
    const QRegularExpressionMatch match = shape.matchView(bounds);
    if (!match.hasMatch()) return {};
    const int left = match.captured(1).toInt();
    const int top = match.captured(2).toInt();
    return QRect(left, top, match.captured(3).toInt() - left, match.captured(4).toInt() - top);
}

QString lastSegment(const QString &className) {
    QString name = className.section(u'.', -1);
    if (const qsizetype dollar = name.indexOf(u'$'); dollar >= 0) name.truncate(dollar);
    return name;
}

// A view that says or does something; the rest are layouts the outline lifts out.
bool speaks(const UiNode &node) {
    return !node.text.isEmpty() || !node.description.isEmpty() || node.clickable || node.scrollable || node.checkable
           || node.focused || node.password || role(node.className) == u"entry"_s;
}

} // namespace

QString role(const QString &className) {
    const QString name = lastSegment(className);
    if (name == u"Button"_s || name == u"ImageButton"_s || name == u"MaterialButton"_s || name == u"AppCompatButton"_s
        || name == u"AppCompatImageButton"_s || name == u"FloatingActionButton"_s) {
        return u"button"_s;
    }
    if (name == u"EditText"_s || name == u"AutoCompleteTextView"_s || name == u"TextInputEditText"_s
        || name == u"AppCompatEditText"_s || name == u"SearchView"_s) {
        return u"entry"_s;
    }
    if (name == u"TextView"_s || name == u"AppCompatTextView"_s || name == u"MaterialTextView"_s) return u"text"_s;
    if (name == u"CheckBox"_s || name == u"CheckedTextView"_s || name == u"AppCompatCheckBox"_s) return u"check box"_s;
    if (name == u"Switch"_s || name == u"SwitchCompat"_s || name == u"ToggleButton"_s || name == u"SwitchMaterial"_s) return u"switch"_s;
    if (name == u"RadioButton"_s) return u"radio button"_s;
    if (name == u"Spinner"_s) return u"combo box"_s;
    if (name == u"RecyclerView"_s || name == u"ListView"_s || name == u"ScrollView"_s || name == u"HorizontalScrollView"_s
        || name == u"NestedScrollView"_s || name == u"ViewPager"_s || name == u"ViewPager2"_s || name == u"GridView"_s) {
        return u"list"_s;
    }
    if (name == u"WebView"_s) return u"web view"_s;
    if (name == u"ImageView"_s || name == u"AppCompatImageView"_s) return u"image"_s;
    if (name == u"SeekBar"_s) return u"slider"_s;
    if (name == u"ProgressBar"_s) return u"progress bar"_s;
    if (name == u"TabWidget"_s || name == u"TabLayout"_s) return u"tab list"_s;
    return u"item"_s;
}

Result<QList<UiNode>> parse(const QString &xml) {
    const qsizetype start = xml.indexOf(u"<hierarchy"_s);
    if (start < 0) {
        const QString said = xml.trimmed().section(u'\n', 0, 0).trimmed();
        return Error{ErrorCode::Parse,
                     said.isEmpty() ? u"The phone did not describe its screen (uiautomator printed nothing)."_s
                                    : u"The phone did not describe its screen: %1 Try again in a moment."_s.arg(said)};
    }
    QXmlStreamReader reader(QStringView(xml).mid(start).toString());
    QList<UiNode> nodes;
    int depth = 0;
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement && reader.name() == u"node"_s) {
            const QXmlStreamAttributes a = reader.attributes();
            UiNode node;
            node.className = a.value("class"_L1).toString();
            node.text = a.value("text"_L1).toString();
            node.description = a.value("content-desc"_L1).toString();
            node.resourceId = a.value("resource-id"_L1).toString();
            node.package = a.value("package"_L1).toString();
            node.bounds = parseBounds(a.value("bounds"_L1));
            node.depth = depth;
            node.clickable = flag(a, "clickable"_L1);
            node.longClickable = flag(a, "long-clickable"_L1);
            node.checkable = flag(a, "checkable"_L1);
            node.checked = flag(a, "checked"_L1);
            node.enabled = !a.hasAttribute("enabled"_L1) || flag(a, "enabled"_L1);
            node.focused = flag(a, "focused"_L1);
            node.scrollable = flag(a, "scrollable"_L1);
            node.selected = flag(a, "selected"_L1);
            node.password = flag(a, "password"_L1);
            nodes.append(node);
            ++depth;
        } else if (token == QXmlStreamReader::EndElement && reader.name() == u"node"_s) {
            --depth;
        }
    }
    if (reader.hasError() && nodes.isEmpty()) {
        return Error{ErrorCode::Parse, u"The phone's description of its screen could not be read: %1"_s.arg(reader.errorString())};
    }
    return nodes;
}

QPoint centre(const QRect &bounds) {
    return QPoint(bounds.x() + bounds.width() / 2, bounds.y() + bounds.height() / 2);
}

PhoneOutline outline(const QList<UiNode> &nodes, const QString &filter, int maxLines, int maxChars) {
    PhoneOutline out;
    if (nodes.isEmpty()) {
        out.text = u"The phone showed an empty screen (no elements)."_s;
        return out;
    }
    out.package = nodes.first().package;
    struct Line {
        QString text;
        bool header{false};
    };
    QList<Line> lines;
    lines.append(Line{u"Phone screen — %1"_s.arg(out.package.isEmpty() ? u"unknown app"_s : out.package), true});
    // The shown depth of each ancestor: a lifted layout adds none.
    QList<int> shownDepthAt;     // by node depth
    int counter = 0;
    for (const UiNode &node : nodes) {
        shownDepthAt.resize(node.depth + 1);
        const int parentShown = node.depth == 0 ? 0 : shownDepthAt.value(node.depth - 1, 0);
        if (!speaks(node)) {
            shownDepthAt[node.depth] = parentShown;
            continue;
        }
        shownDepthAt[node.depth] = parentShown + 1;
        const QString id = u"p%1"_s.arg(++counter);
        const QString words = !node.text.isEmpty() ? node.text : node.description;
        out.refs.insert(id, node.bounds);
        out.names.insert(id, words);
        QString line = QString(parentShown * 2 + 2, u' ') + u"- "_s + role(node.className);
        if (node.password) {
            line += u" [password]"_s;
        } else if (!node.text.isEmpty()) {
            line += u" \"%1\""_s.arg(node.text);
            if (!node.description.isEmpty() && node.description != node.text) line += u" — "_s + node.description;
        } else if (!node.description.isEmpty()) {
            line += u" \"%1\""_s.arg(node.description);
        } else if (!node.resourceId.isEmpty()) {
            line += u" (%1)"_s.arg(node.resourceId.section(u'/', -1));
        }
        if (node.clickable) line += u" [tap]"_s;
        if (node.checked) line += u" [checked]"_s;
        if (node.selected) line += u" [selected]"_s;
        if (node.focused) line += u" [focused]"_s;
        if (!node.enabled) line += u" [disabled]"_s;
        if (node.scrollable) line += u" [scrollable]"_s;
        line += u" [ref=%1]"_s.arg(id);
        lines.append(Line{line, false});
    }
    if (lines.size() == 1) lines.append(Line{u"  (nothing on it says or does anything)"_s, false});
    const QStringList wanted = filter.simplified().toLower().split(u' ', Qt::SkipEmptyParts);
    QStringList kept;
    for (const Line &line : lines) {
        if (!line.header && !wanted.isEmpty()) {
            const QString lower = line.text.toLower();
            if (!std::all_of(wanted.cbegin(), wanted.cend(), [&lower](const QString &word) { return lower.contains(word); })) continue;
        }
        kept.append(line.text);
    }
    QStringList shown;
    qsizetype chars = 0;
    for (const QString &line : kept) {
        if (shown.size() >= maxLines || chars + line.size() > maxChars) {
            out.truncated = true;
            break;
        }
        shown.append(line);
        chars += line.size() + 1;
    }
    if (out.truncated) shown.append(u"… %1 more elements; ask again with filter."_s.arg(kept.size() - shown.size()));
    out.text = shown.join(u'\n');
    return out;
}

} // namespace nexa::uihierarchy
