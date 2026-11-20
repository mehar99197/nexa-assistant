#pragma once

#include "core/Result.h"

#ifdef Q_OS_UNIX
#include <QDBusArgument>
#include <QDBusObjectPath>
#endif
#include <QHashFunctions>
#include <QList>
#include <QString>
#include <QStringList>

namespace nexa::atspi {

// One accessible object on the desktop's accessibility bus (AT-SPI2): the connection that owns it and its path.
struct NodeRef {
    QString bus;
    QString path;

    bool isNull() const { return path.isEmpty(); }
    bool operator==(const NodeRef &) const = default;
};

inline size_t qHash(const NodeRef &ref, size_t seed = 0) noexcept {
    return qHashMulti(seed, ref.bus, ref.path);
}

// AT-SPI's state bits (atspi-constants.h, AtspiStateType) — the ones the outline reads.
enum class State : int {
    Active = 1,
    Checked = 4,
    Collapsed = 5,
    Editable = 7,
    Enabled = 8,
    Expandable = 9,
    Expanded = 10,
    Focusable = 11,
    Focused = 12,
    Iconified = 15,
    Pressed = 20,
    Selectable = 22,
    Selected = 23,
    Sensitive = 24,
    Showing = 25,
    Visible = 30,
    Checkable = 41,
    ReadOnly = 43,
};

// AT-SPI's roles (AtspiRole) the outline tells apart; every other role is shown by its name alone.
enum class Role : uint {
    Invalid = 0,
    CheckBox = 7,
    CheckMenuItem = 8,
    ComboBox = 11,
    DesktopFrame = 14,
    Dialog = 16,
    Filler = 20,
    Frame = 23,
    GlassPane = 24,
    Image = 27,
    Label = 29,
    LayeredPane = 30,
    List = 31,
    ListItem = 32,
    Menu = 33,
    MenuBar = 34,
    MenuItem = 35,
    PageTab = 37,
    PageTabList = 38,
    Panel = 39,
    PasswordText = 40,
    PushButton = 43,
    RadioButton = 44,
    RadioMenuItem = 45,
    RootPane = 46,
    ScrollBar = 48,
    ScrollPane = 49,
    Separator = 50,
    Slider = 51,
    SpinButton = 52,
    SplitPane = 53,
    StatusBar = 54,
    TableCell = 56,
    Terminal = 60,
    Text = 61,
    ToggleButton = 62,
    ToolBar = 63,
    ToolTip = 64,
    Unknown = 67,
    Viewport = 68,
    Window = 69,
    Paragraph = 73,
    Application = 75,
    Entry = 79,
    DocumentFrame = 82,
    Heading = 83,
    Section = 85,
    RedundantObject = 86,
    Form = 87,
    Link = 88,
    TreeItem = 91,
    DocumentText = 94,
    DocumentWeb = 95,
    Static = 116,
    Switch = 130,
};

// A node as an application's cache describes it.
struct Node {
    NodeRef ref;
    NodeRef parent;
    QList<NodeRef> children;    // listed by Qt's bridge; empty when the bridge only counts them
    int childCount{0};
    int indexInParent{-1};      // the counting bridges say; -1 when unknown
    QStringList interfaces;     // "org.a11y.atspi.Text", "org.a11y.atspi.Action", …
    QString name;
    QString description;
    uint role{0};
    quint64 states{0};          // the two state words, low word first

    bool has(State state) const { return (states >> int(state)) & 1; }
    bool is(Role expected) const { return role == uint(expected); }
    // "Text", "Action", "EditableText", "Component", "Value" — the short name of an org.a11y.atspi interface.
    bool implements(QLatin1StringView interface) const;
};

// The role as a person says it: 43 → "button", 62 → "toggle button", 37 → "tab"; "unknown" past the table.
QString roleName(uint role);

// The two forms a bridge's org.a11y.atspi.Cache.GetItems reply comes in.
inline constexpr QLatin1StringView kCountedSignature{"a((so)(so)(so)iiassusau)"};      // GTK, Chromium, Electron (atk-bridge)
inline constexpr QLatin1StringView kListedSignature{"a((so)(so)(so)a(so)assusau)"};    // Qt

enum class CacheForm { Counted, Listed, Unknown };
CacheForm cacheForm(QStringView signature);

#ifdef Q_OS_UNIX
// The wire structures of those two forms, for QtDBus marshalling and demarshalling.
struct WireRef {
    QString bus;
    QDBusObjectPath path;
};

struct CountedItem {
    WireRef ref, app, parent;
    int index{0};
    int count{0};
    QStringList interfaces;
    QString name;
    uint role{0};
    QString description;
    QList<uint> states;
};

struct ListedItem {
    WireRef ref, app, parent;
    QList<WireRef> children;
    QStringList interfaces;
    QString name;
    uint role{0};
    QString description;
    QList<uint> states;
};

QDBusArgument &operator<<(QDBusArgument &argument, const WireRef &ref);
const QDBusArgument &operator>>(const QDBusArgument &argument, WireRef &ref);
QDBusArgument &operator<<(QDBusArgument &argument, const CountedItem &item);
const QDBusArgument &operator>>(const QDBusArgument &argument, CountedItem &item);
QDBusArgument &operator<<(QDBusArgument &argument, const ListedItem &item);
const QDBusArgument &operator>>(const QDBusArgument &argument, ListedItem &item);

// The nodes those items describe (pure).
QList<Node> toNodes(const QList<CountedItem> &items);
QList<Node> toNodes(const QList<ListedItem> &items);

// The nodes of a live GetItems reply, in whichever form `signature` (the reply's) says; Parse for a form neither
// bridge sends. A QDBusArgument can only be read once it came out of a message, so this is for the reader alone.
Result<QList<Node>> decodeCacheItems(const QDBusArgument &items, const QString &signature);

// Registers the wire types with QtDBus; called by decodeCacheItems, harmless to call again.
void registerCacheTypes();
#endif

} // namespace nexa::atspi
