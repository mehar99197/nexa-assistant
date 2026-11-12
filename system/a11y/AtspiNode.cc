#include "system/a11y/AtspiNode.h"

#include <QDBusMetaType>

#include <mutex>

using namespace Qt::StringLiterals;

namespace nexa::atspi {

namespace {

// AtspiRole, in order, as a person says them (atspi-constants.h of at-spi2-core 2.61; 131 roles).
constexpr const char *const kRoles[] = {
    "invalid", "accelerator label", "alert", "animation", "arrow", "calendar", "canvas", "check box",
    "check menu item", "color chooser", "column header", "combo box", "date editor", "desktop icon",
    "desktop frame", "dial", "dialog", "directory pane", "drawing area", "file chooser", "filler",
    "focus traversable", "font chooser", "frame", "glass pane", "html container", "icon", "image",
    "internal frame", "label", "layered pane", "list", "list item", "menu", "menu bar", "menu item",
    "option pane", "tab", "tab list", "panel", "password", "popup menu", "progress bar", "button",
    "radio button", "radio menu item", "root pane", "row header", "scroll bar", "scroll pane", "separator",
    "slider", "spin button", "split pane", "status bar", "table", "table cell", "table column header",
    "table row header", "tearoff menu item", "terminal", "text", "toggle button", "tool bar", "tool tip",
    "tree", "tree table", "unknown", "viewport", "window", "extended", "header", "footer", "paragraph",
    "ruler", "application", "autocomplete", "editbar", "embedded", "entry", "chart", "caption", "document",
    "heading", "page", "section", "redundant object", "form", "link", "input method window", "table row",
    "tree item", "document", "document", "document", "document", "document", "comment", "list box", "grouping",
    "image map", "notification", "info bar", "level bar", "title bar", "block quote", "audio", "video",
    "definition", "article", "landmark", "log", "marquee", "math", "rating", "timer", "static",
    "math fraction", "math root", "subscript", "superscript", "description list", "description term",
    "description value", "footnote", "content deletion", "content insertion", "mark", "suggestion",
    "menu button", "switch",
};
constexpr uint kRoleCount = sizeof(kRoles) / sizeof(kRoles[0]);
static_assert(kRoleCount == 131, "the AT-SPI role table has 131 entries");

NodeRef toRef(const WireRef &wire) {
    return NodeRef{wire.bus, wire.path.path()};
}

quint64 toStates(const QList<uint> &words) {
    quint64 states = 0;
    for (qsizetype i = 0; i < std::min<qsizetype>(words.size(), 2); ++i) states |= quint64(words.at(i)) << (32 * i);
    return states;
}

Node toNode(const CountedItem &item) {
    Node node;
    node.ref = toRef(item.ref);
    node.parent = toRef(item.parent);
    node.childCount = std::max(0, item.count);
    node.interfaces = item.interfaces;
    node.name = item.name;
    node.description = item.description;
    node.role = item.role;
    node.states = toStates(item.states);
    return node;
}

Node toNode(const ListedItem &item) {
    Node node;
    node.ref = toRef(item.ref);
    node.parent = toRef(item.parent);
    for (const WireRef &child : item.children) node.children.append(toRef(child));
    node.childCount = int(item.children.size());
    node.interfaces = item.interfaces;
    node.name = item.name;
    node.description = item.description;
    node.role = item.role;
    node.states = toStates(item.states);
    return node;
}

} // namespace

bool Node::implements(QLatin1StringView interface) const {
    const QString full = u"org.a11y.atspi."_s + interface;
    return interfaces.contains(full);
}

QString roleName(uint role) {
    return QString::fromLatin1(role < kRoleCount ? kRoles[role] : "unknown");
}

QDBusArgument &operator<<(QDBusArgument &argument, const WireRef &ref) {
    argument.beginStructure();
    argument << ref.bus << ref.path;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, WireRef &ref) {
    argument.beginStructure();
    argument >> ref.bus >> ref.path;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const CountedItem &item) {
    argument.beginStructure();
    argument << item.ref << item.app << item.parent << item.index << item.count << item.interfaces << item.name
             << item.role << item.description << item.states;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, CountedItem &item) {
    argument.beginStructure();
    argument >> item.ref >> item.app >> item.parent >> item.index >> item.count >> item.interfaces >> item.name
             >> item.role >> item.description >> item.states;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const ListedItem &item) {
    argument.beginStructure();
    argument << item.ref << item.app << item.parent << item.children << item.interfaces << item.name << item.role
             << item.description << item.states;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, ListedItem &item) {
    argument.beginStructure();
    argument >> item.ref >> item.app >> item.parent >> item.children >> item.interfaces >> item.name >> item.role
             >> item.description >> item.states;
    argument.endStructure();
    return argument;
}

void registerCacheTypes() {
    static std::once_flag once;
    std::call_once(once, [] {
        qDBusRegisterMetaType<WireRef>();
        qDBusRegisterMetaType<QList<WireRef>>();
        qDBusRegisterMetaType<CountedItem>();
        qDBusRegisterMetaType<QList<CountedItem>>();
        qDBusRegisterMetaType<ListedItem>();
        qDBusRegisterMetaType<QList<ListedItem>>();
    });
}

CacheForm cacheForm(QStringView signature) {
    if (signature == kCountedSignature) return CacheForm::Counted;
    if (signature == kListedSignature) return CacheForm::Listed;
    return CacheForm::Unknown;
}

QList<Node> toNodes(const QList<CountedItem> &items) {
    QList<Node> nodes;
    nodes.reserve(items.size());
    for (const CountedItem &item : items) nodes.append(toNode(item));
    return nodes;
}

QList<Node> toNodes(const QList<ListedItem> &items) {
    QList<Node> nodes;
    nodes.reserve(items.size());
    for (const ListedItem &item : items) nodes.append(toNode(item));
    return nodes;
}

Result<QList<Node>> decodeCacheItems(const QDBusArgument &items, const QString &signature) {
    registerCacheTypes();
    switch (cacheForm(signature)) {
    case CacheForm::Counted: {
        QList<CountedItem> list;
        items >> list;
        return toNodes(list);
    }
    case CacheForm::Listed: {
        QList<ListedItem> list;
        items >> list;
        return toNodes(list);
    }
    case CacheForm::Unknown:
        break;
    }
    return Error{ErrorCode::Parse, u"The application's accessibility cache is in a form Nexa does not read (%1)."_s.arg(signature)};
}

} // namespace nexa::atspi
