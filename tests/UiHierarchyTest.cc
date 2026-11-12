// system/android/UiHierarchy: the phone's screen from `uiautomator dump` as the outline a model works on.

#include "system/android/UiHierarchy.h"

#include "QtStringMakers.h"

#include <QFile>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::uihierarchy;
using namespace Qt::StringLiterals;

namespace {

QString videoApp() {
    QFile file(QStringLiteral(NEXA_TESTS_DIR "/fake-ui-dump.xml"));
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
}

const QString kSettings = uR"(<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation="0">
<node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="com.android.settings" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="false" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[0,0][1080,2400]">
  <node index="0" text="Wi-Fi" resource-id="android:id/title" class="android.widget.TextView" package="com.android.settings" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="false" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[48,300][600,360]" />
  <node index="1" text="" resource-id="android:id/switch_widget" class="android.widget.Switch" package="com.android.settings" content-desc="Wi-Fi" checkable="true" checked="true" clickable="true" enabled="true" focusable="true" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[900,300][1032,360]" />
  <node index="2" text="" resource-id="com.android.settings:id/password" class="android.widget.EditText" package="com.android.settings" content-desc="" checkable="false" checked="false" clickable="true" enabled="true" focusable="true" focused="true" scrollable="false" long-clickable="true" password="true" selected="false" bounds="[48,500][1032,600]" />
  <node index="3" text="" resource-id="com.android.settings:id/name" class="android.widget.EditText" package="com.android.settings" content-desc="" checkable="false" checked="false" clickable="true" enabled="true" focusable="true" focused="false" scrollable="false" long-clickable="true" password="false" selected="false" bounds="[48,700][1032,800]" />
</node></hierarchy>)"_s;

} // namespace

TEST_CASE("The dump becomes an outline with a ref per element that says or does something, and the centre of each") {
    const Result<QList<UiNode>> nodes = parse(videoApp());
    REQUIRE(nodes);
    CHECK(nodes.value().size() == 17);
    const PhoneOutline out = outline(nodes.value());
    const QStringList lines = out.text.split(u'\n');
    REQUIRE(!lines.isEmpty());
    CHECK(lines.first() == u"Phone screen — com.google.android.youtube"_s);
    CHECK(out.text.contains(u"  - image \"YouTube\" [ref=p1]"_s));
    CHECK(out.text.contains(u"  - button \"Search\" [tap] [ref=p2]"_s));
    CHECK(out.refs.value(u"p2"_s) == QRect(924, 84, 108, 108));
    CHECK(centre(out.refs.value(u"p2"_s)) == QPoint(978, 138));
    CHECK(out.names.value(u"p2"_s) == u"Search"_s);
    CHECK(out.text.contains(u"  - list (results) [scrollable] [ref=p3]"_s));
    CHECK(out.text.contains(u"    - item [tap] [ref=p4]"_s));                       // a video card: clickable, wordless itself
    CHECK(out.text.contains(u"      - text \"Learn Qt in 20 minutes\" [ref=p5]"_s));
    CHECK(out.text.contains(u"  - item \"Home\" [tap] [selected] [ref="_s));
    CHECK(out.text.contains(u"  - item \"Library\" [tap] [disabled] [ref="_s));
    CHECK_FALSE(out.text.contains(u"toolbar"_s));                                    // a bare layout, lifted out
    CHECK_FALSE(out.text.contains(u"bottom_bar"_s));
    CHECK(out.package == u"com.google.android.youtube"_s);
    CHECK_FALSE(out.truncated);
    for (const QString &ref : out.refs.keys()) CHECK(out.text.contains(u"[ref=%1]"_s.arg(ref)));
}

TEST_CASE("A switch; a focused field; a password without its text") {
    const Result<QList<UiNode>> nodes = parse(kSettings);
    REQUIRE(nodes);
    const PhoneOutline out = outline(nodes.value());
    CHECK(out.text.contains(u"  - text \"Wi-Fi\" [ref=p1]"_s));
    CHECK(out.text.contains(u"  - switch \"Wi-Fi\" [tap] [checked] [ref=p2]"_s));
    CHECK(out.text.contains(u"  - entry [password] [tap] [focused] [ref=p3]"_s));
    CHECK(out.text.contains(u"  - entry (name) [tap] [ref=p4]"_s));      // an empty field: its id says what it is
    CHECK_FALSE(out.text.contains(u"password)"_s));
}

TEST_CASE("Roles come from the view's class") {
    CHECK(role(u"android.widget.Button"_s) == u"button"_s);
    CHECK(role(u"android.widget.ImageButton"_s) == u"button"_s);
    CHECK(role(u"com.google.android.material.button.MaterialButton"_s) == u"button"_s);
    CHECK(role(u"android.widget.EditText"_s) == u"entry"_s);
    CHECK(role(u"androidx.appcompat.widget.AppCompatEditText"_s) == u"entry"_s);
    CHECK(role(u"android.widget.TextView"_s) == u"text"_s);
    CHECK(role(u"androidx.recyclerview.widget.RecyclerView"_s) == u"list"_s);
    CHECK(role(u"android.webkit.WebView"_s) == u"web view"_s);
    CHECK(role(u"android.widget.Switch"_s) == u"switch"_s);
    CHECK(role(u"android.view.View"_s) == u"item"_s);
    CHECK(role(u"android.widget.LinearLayout$Inner"_s) == u"item"_s);
}

TEST_CASE("uiautomator's own error is reported as such, and an empty hierarchy is said in words") {
    const Result<QList<UiNode>> busy = parse(u"ERROR: could not get idle state.\n"_s);
    REQUIRE_FALSE(busy);
    CHECK(busy.error().code == ErrorCode::Parse);
    CHECK(busy.error().message == u"The phone did not describe its screen: ERROR: could not get idle state. Try again in a moment."_s);
    const Result<QList<UiNode>> nothing = parse(QString());
    REQUIRE_FALSE(nothing);
    CHECK(nothing.error().message.contains(u"printed nothing"_s));
    const Result<QList<UiNode>> empty = parse(u"UI hierchary dumped to: /dev/tty\n<?xml version='1.0'?><hierarchy rotation=\"0\"></hierarchy>"_s);
    REQUIRE(empty);
    CHECK(empty.value().isEmpty());
    CHECK(outline(empty.value()).text == u"The phone showed an empty screen (no elements)."_s);
}

TEST_CASE("filter keeps the header and the matching lines, and the cap says what was left out") {
    const QList<UiNode> nodes = parse(videoApp()).value();
    const PhoneOutline filtered = outline(nodes, u"QT"_s);
    const QStringList lines = filtered.text.split(u'\n');
    REQUIRE(lines.size() == 3);
    CHECK(lines.first().startsWith(u"Phone screen"_s));
    CHECK(lines.at(1).contains(u"Learn Qt in 20 minutes"_s));
    CHECK(lines.at(2).contains(u"Qt Group"_s));
    const PhoneOutline capped = outline(nodes, {}, 4);
    CHECK(capped.truncated);
    CHECK(capped.text.split(u'\n').size() == 5);
    CHECK(capped.text.endsWith(u"more elements; ask again with filter."_s));
    CHECK(capped.refs.size() == outline(nodes).refs.size());     // every element keeps its ref
}
