// system/a11y/AtspiNode: the accessibility bus's cache items, in both forms the bridges send, as nodes.

#include "system/a11y/AtspiNode.h"

#include "QtStringMakers.h"

#include <QDBusArgument>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::atspi;
using namespace Qt::StringLiterals;

namespace {

WireRef ref(const QString &path) {
    return WireRef{u":1.13"_s, QDBusObjectPath(path)};
}

// The terminal's frame as the live sample gave it: showing, visible, sensitive, five children.
CountedItem frame() {
    CountedItem item;
    item.ref = ref(u"/org/a11y/atspi/accessible/75"_s);
    item.app = ref(u"/org/a11y/atspi/accessible/root"_s);
    item.parent = ref(u"/org/a11y/atspi/accessible/root"_s);
    item.index = 0;
    item.count = 5;
    item.interfaces = {u"org.a11y.atspi.Accessible"_s, u"org.a11y.atspi.Component"_s};
    item.name = u"◑ nexa-mission-control-dashboard"_s;
    item.role = 23;
    item.states = {1124073728u, 0u};    // 0x43000000: SENSITIVE (24), SHOWING (25), VISIBLE (30)
    return item;
}

// A button as Qt's bridge lists it: two children, focusable, checkable (the second state word).
ListedItem listedButton() {
    ListedItem item;
    item.ref = ref(u"/org/a11y/atspi/accessible/1"_s);
    item.app = ref(u"/org/a11y/atspi/accessible/root"_s);
    item.parent = ref(u"/org/a11y/atspi/accessible/root"_s);
    item.children = {ref(u"/org/a11y/atspi/accessible/2"_s), ref(u"/org/a11y/atspi/accessible/3"_s)};
    item.interfaces = {u"org.a11y.atspi.Accessible"_s, u"org.a11y.atspi.Action"_s};
    item.name = u"OK"_s;
    item.role = 43;
    item.states = {(1u << 11) | (1u << 24) | (1u << 25) | (1u << 30), 1u << (41 - 32)};
    return item;
}

} // namespace

TEST_CASE("The GTK cache form decodes: parent, count, interfaces, name, role and the state words") {
    CHECK(cacheForm(QString(kCountedSignature)) == CacheForm::Counted);
    const QList<Node> nodes = toNodes(QList<CountedItem>{frame()});
    REQUIRE(nodes.size() == 1);
    const Node &node = nodes.first();
    CHECK(node.ref == NodeRef{u":1.13"_s, u"/org/a11y/atspi/accessible/75"_s});
    CHECK(node.parent.path == u"/org/a11y/atspi/accessible/root"_s);
    CHECK(node.childCount == 5);
    CHECK(node.children.isEmpty());
    CHECK(node.name == u"◑ nexa-mission-control-dashboard"_s);
    CHECK(node.is(Role::Frame));
    CHECK(node.implements("Component"_L1));
    CHECK_FALSE(node.implements("Text"_L1));
    CHECK(node.has(State::Showing));
    CHECK(node.has(State::Visible));
    CHECK(node.has(State::Sensitive));
    CHECK_FALSE(node.has(State::Focused));
    CHECK_FALSE(node.has(State::Active));
}

TEST_CASE("Qt's cache form lists the children instead of counting them") {
    registerCacheTypes();
    CHECK(cacheForm(QString(kListedSignature)) == CacheForm::Listed);
    const QList<Node> nodes = toNodes(QList<ListedItem>{listedButton()});
    REQUIRE(nodes.size() == 1);
    const Node &node = nodes.first();
    CHECK(node.childCount == 2);
    CHECK(node.children.size() == 2);
    CHECK(node.children.last().path == u"/org/a11y/atspi/accessible/3"_s);
    CHECK(node.is(Role::PushButton));
    CHECK(node.implements("Action"_L1));
    CHECK(node.has(State::Focusable));
    CHECK(node.has(State::Checkable));    // the second state word
    CHECK_FALSE(node.has(State::Checked));
}

TEST_CASE("A cache form neither bridge sends is a Parse error, not a guess") {
    CHECK(cacheForm(u"a((so)(so)(so)iassusau)"_s) == CacheForm::Unknown);
    const QDBusArgument argument;
    const Result<QList<Node>> nodes = decodeCacheItems(argument, u"a((so)(so)(so)iassusau)"_s);
    REQUIRE_FALSE(nodes);
    CHECK(nodes.error().code == ErrorCode::Parse);
    CHECK(nodes.error().message.contains(u"a((so)(so)(so)iassusau)"_s));
}

TEST_CASE("The wire types marshal in the order of the bridges' signatures") {
    // A written argument cannot be read back outside a message, but its signature can be checked.
    registerCacheTypes();
    QDBusArgument counted;
    counted << QList<CountedItem>{frame()};
    CHECK(counted.currentSignature() == QString(kCountedSignature));
    QDBusArgument listed;
    listed << QList<ListedItem>{listedButton()};
    CHECK(listed.currentSignature() == QString(kListedSignature));
}

TEST_CASE("Role names are the words a person uses") {
    CHECK(roleName(43) == u"button"_s);
    CHECK(roleName(62) == u"toggle button"_s);
    CHECK(roleName(37) == u"tab"_s);
    CHECK(roleName(38) == u"tab list"_s);
    CHECK(roleName(40) == u"password"_s);
    CHECK(roleName(60) == u"terminal"_s);
    CHECK(roleName(95) == u"document"_s);
    CHECK(roleName(130) == u"switch"_s);
    CHECK(roleName(uint(Role::Static)) == u"static"_s);
    CHECK(roleName(131) == u"unknown"_s);
    CHECK(roleName(999) == u"unknown"_s);
}
