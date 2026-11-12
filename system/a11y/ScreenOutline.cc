#include "system/a11y/ScreenOutline.h"

#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

using atspi::Node;
using atspi::NodeRef;
using atspi::Role;
using atspi::State;

constexpr int kMaxDepth = 48;
constexpr QLatin1StringView kRootPath{"/org/a11y/atspi/accessible/root"};

// The desktop's own applications: never a window the user works in.
const QSet<QString> &shellApps() {
    static const QSet<QString> apps{u"gnome-shell"_s,       u"gjs"_s,          u"ibus-extension-gtk3"_s, u"ibus-x11"_s,
                                    u"xdg-desktop-portal-gtk"_s, u"xdg-desktop-portal-gnome"_s, u"at-spi2-registryd"_s,
                                    u"mutter-x11-frames"_s, u"easyeffects"_s};
    return apps;
}

bool isWindowRole(uint role) {
    return role == uint(Role::Frame) || role == uint(Role::Window) || role == uint(Role::Dialog);
}

// Containers that say nothing by themselves: their children stand in their place.
bool isStructural(uint role) {
    switch (Role(role)) {
    case Role::Filler:
    case Role::Panel:
    case Role::Viewport:
    case Role::ScrollPane:
    case Role::Section:
    case Role::RootPane:
    case Role::LayeredPane:
    case Role::GlassPane:
    case Role::SplitPane:
    case Role::Separator:
    case Role::ScrollBar:
    case Role::ToolTip:
    case Role::Unknown:
    case Role::RedundantObject:
    case Role::Form:
    case Role::ToolBar:
    case Role::MenuBar:
    case Role::List:
    case Role::PageTabList:
    case Role::Application:
        return true;
    default:
        return false;
    }
}

bool isTextRole(uint role) {
    switch (Role(role)) {
    case Role::Text:
    case Role::Entry:
    case Role::Terminal:
    case Role::Paragraph:
    case Role::Static:
    case Role::DocumentText:
    case Role::DocumentWeb:
    case Role::DocumentFrame:
    case Role::Heading:
    case Role::Label:
        return true;
    default:
        return false;
    }
}

struct Tree {
    const AtspiApp *app{nullptr};
    QHash<QString, const Node *> byPath;
    QHash<QString, QList<const Node *>> childrenOf;     // parent path → children, in their order
    const Node *root{nullptr};

    explicit Tree(const AtspiApp &application) : app(&application) {
        for (const Node &node : application.nodes) byPath.insert(node.ref.path, &node);
        for (const Node &node : application.nodes) {
            if (node.is(Role::Application) || node.ref.path == kRootPath) {
                if (!root) root = &node;
                continue;
            }
            childrenOf[node.parent.path].append(&node);
        }
        for (QList<const Node *> &children : childrenOf) {
            std::stable_sort(children.begin(), children.end(), [](const Node *a, const Node *b) {
                return a->indexInParent < b->indexInParent;
            });
        }
    }

    QList<const Node *> children(const Node &node) const {
        if (!node.children.isEmpty()) {                 // Qt's bridge lists them in order
            QList<const Node *> list;
            for (const NodeRef &ref : node.children) {
                if (const Node *child = byPath.value(ref.path)) list.append(child);
            }
            return list;
        }
        return childrenOf.value(node.ref.path);
    }

    // A window's top-level nodes: the root's children, or — for a bridge whose root is not in the cache — the nodes
    // whose parent is the application's root path.
    QList<const Node *> windows() const {
        return childrenOf.value(QString(kRootPath));
    }

    bool hasFocused(const Node &node, int depth = 0) const {
        if (depth > kMaxDepth) return false;
        if (node.has(State::Focused)) return true;
        for (const Node *child : children(node)) {
            if (hasFocused(*child, depth + 1)) return true;
        }
        return false;
    }
};

struct Line {
    QString text;
    bool header{false};
    int window{0};
};

class Builder {
public:
    Builder(const OutlineOptions &options, const TextLookup &text) : options_(options), text_(text) {}

    QString ref(const Node &node, const QString &shownName) {
        const QString id = u"s%1"_s.arg(++counter_);
        outline_.refs.insert(id, node.ref);
        outline_.roles.insert(id, node.role);
        outline_.names.insert(id, shownName);
        return id;
    }

    void writeWindow(const Tree &tree, const Node &window, bool active, int index) {
        const QString title = window.name;
        const QString kind = window.is(Role::Dialog) ? u"Dialog"_s : u"Window"_s;
        const QString id = ref(window, title);
        lines_.append(Line{u"%1 \"%2\" — %3%4 [ref=%5]"_s.arg(kind, title, tree.app->name, active ? u" (active)"_s : QString(), id),
                           true, index});
        const qsizetype before = lines_.size();
        for (const Node *child : tree.children(window)) writeNode(tree, *child, 1, index);
        if (lines_.size() == before) {
            lines_.append(Line{u"  (exposes nothing — %1)"_s.arg(exposesNothingHint()), false, index});
        }
        ++outline_.windows;
    }

    void writeNode(const Tree &tree, const Node &node, int depth, int window) {
        if (depth > kMaxDepth || !node.has(State::Showing)) return;
        const bool actionable = node.implements("Action"_L1) || node.has(State::Focusable) || node.has(State::Editable);
        const bool textual = node.implements("Text"_L1) && (isTextRole(node.role) || node.is(Role::PasswordText));
        const bool nameless = node.name.isEmpty();
        // Lifted: a structural container, or an unnamed element that neither says nor does anything.
        if ((isStructural(node.role) && nameless) || (nameless && !actionable && !textual && !node.is(Role::Image))) {
            for (const Node *child : tree.children(node)) writeNode(tree, *child, depth, window);
            return;
        }
        if (node.is(Role::Image) && nameless) return;
        QString line = QString(depth * 2, u' ') + u"- "_s + atspi::roleName(node.role);
        if (!nameless) line += u" \"%1\""_s.arg(node.name);
        if (node.has(State::Checked)) line += u" [checked]"_s;
        if (node.has(State::Selected)) line += u" [selected]"_s;
        if (node.has(State::Expandable)) line += node.has(State::Expanded) ? u" [expanded]"_s : u" [collapsed]"_s;
        if (node.has(State::Focused)) line += u" [focused]"_s;
        if (actionable && !node.has(State::Sensitive)) line += u" [disabled]"_s;
        if (node.has(State::Editable) && isTextRole(node.role)) line += u" [editable]"_s;
        if (node.is(Role::PasswordText)) line += u" [password]"_s;
        line += u" [ref=%1]"_s.arg(ref(node, node.name));
        if (!node.description.isEmpty() && node.description != node.name) line += u" — "_s + node.description;
        if (textual && !node.is(Role::PasswordText) && (nameless || node.is(Role::Terminal)) && text_) {
            const bool terminal = node.is(Role::Terminal);
            const int max = terminal ? options_.terminalTailChars : options_.excerptChars;
            QString words = text_(node.ref, max, terminal).trimmed();
            words.replace(u'\n', u"⏎"_s);
            if (!words.isEmpty()) {
                line += u": \"%1\""_s.arg(words);
                if (words.size() >= max) line += terminal ? u" (the last %1 characters; screen_read for all of it)"_s.arg(max)
                                                          : u" (the first %1 characters; screen_read for all of it)"_s.arg(max);
            }
        }
        lines_.append(Line{line, false, window});
        for (const Node *child : tree.children(node)) writeNode(tree, *child, depth + 1, window);
    }

    void note(const QString &text) { lines_.append(Line{text, true, -1}); }

    ScreenOutline finish() {
        QList<Line> kept;
        const QStringList words = options_.filter.simplified().toLower().split(u' ', Qt::SkipEmptyParts);
        for (const Line &line : lines_) {
            if (words.isEmpty() || line.header) {
                kept.append(line);
                continue;
            }
            const QString lower = line.text.toLower();
            if (std::all_of(words.cbegin(), words.cend(), [&lower](const QString &word) { return lower.contains(word); })) kept.append(line);
        }
        QStringList out;
        qsizetype chars = 0;
        int written = 0;
        for (const Line &line : kept) {
            if (written >= options_.maxLines || chars + line.text.size() > options_.maxChars) {
                outline_.truncated = true;
                break;
            }
            out.append(line.text);
            chars += line.text.size() + 1;
            ++written;
        }
        if (outline_.truncated) {
            out.append(u"… %1 more elements; ask again with filter, or scope to one window."_s.arg(kept.size() - written));
        }
        // Refs of lines that were filtered or cut stay resolvable: the model may still name them from an earlier look.
        outline_.text = out.join(u'\n');
        return outline_;
    }

private:
    const OutlineOptions &options_;
    const TextLookup &text_;
    QList<Line> lines_;
    ScreenOutline outline_;
    int counter_{0};
};

struct WindowEntry {
    const Tree *tree;
    const Node *window;
    bool active{false};
};

} // namespace

QString exposesNothingHint() {
    return u"this app is not exposing its contents yet: switch on Settings → General → \"Let apps expose their contents\", "
           "then close and open the app again"_s;
}

ScreenOutline buildScreenOutline(const QList<AtspiApp> &apps, const OutlineOptions &options, const TextLookup &text) {
    QList<Tree> trees;
    trees.reserve(apps.size());
    for (const AtspiApp &app : apps) {
        if (app.name == options.ownApp || shellApps().contains(app.name)) continue;
        trees.append(Tree(app));
    }
    QList<WindowEntry> windows;
    for (const Tree &tree : trees) {
        for (const Node *window : tree.windows()) {
            if (!isWindowRole(window->role)) continue;
            if (!window->has(State::Showing) || !window->has(State::Visible) || window->has(State::Iconified)) continue;
            // A nameless frame with nothing in it (a tooltip's host, a popup's shell) is noise, not a window.
            if (window->name.isEmpty() && tree.children(*window).isEmpty()) continue;
            windows.append(WindowEntry{&tree, window, window->has(State::Active)});
        }
    }
    if (std::none_of(windows.cbegin(), windows.cend(), [](const WindowEntry &w) { return w.active; })) {
        for (WindowEntry &entry : windows) {
            if (entry.tree->hasFocused(*entry.window)) {
                entry.active = true;
                break;
            }
        }
    }
    std::stable_sort(windows.begin(), windows.end(), [](const WindowEntry &a, const WindowEntry &b) { return a.active && !b.active; });

    Builder builder(options, text);
    const QString scope = options.scope.trimmed().toLower();
    if (windows.isEmpty()) {
        builder.note(u"No window exposes itself on the accessibility bus — %1."_s.arg(exposesNothingHint()));
        return builder.finish();
    }
    const bool haveActive = !windows.isEmpty() && windows.first().active;
    if (scope.isEmpty() || scope == u"active"_s) {
        if (!haveActive) {
            builder.note(u"No window is active (the desktop or Nexa itself is in front); every window follows."_s);
        }
        int index = 0;
        for (const WindowEntry &entry : windows) {
            if (haveActive && !entry.active) {
                builder.note(u"Also open: \"%1\" — %2"_s.arg(entry.window->name, entry.tree->app->name));
                continue;
            }
            builder.writeWindow(*entry.tree, *entry.window, entry.active, index++);
        }
        return builder.finish();
    }
    int index = 0;
    bool any = false;
    for (const WindowEntry &entry : windows) {
        const bool wanted = scope == u"all"_s || entry.window->name.toLower().contains(scope)
                            || entry.tree->app->name.toLower().contains(scope);
        if (!wanted) continue;
        any = true;
        builder.writeWindow(*entry.tree, *entry.window, entry.active, index++);
    }
    if (!any) {
        QStringList titles;
        for (const WindowEntry &entry : windows) titles.append(u"\"%1\" (%2)"_s.arg(entry.window->name, entry.tree->app->name));
        builder.note(u"No window matches \"%1\". Open: %2."_s.arg(options.scope.trimmed(), titles.join(u", "_s)));
    }
    return builder.finish();
}

} // namespace nexa
