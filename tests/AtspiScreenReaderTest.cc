// system/a11y/AtspiScreenReader against the desktop's real accessibility bus — skipped where there is none. Read-only:
// it lists the applications, decodes their caches and reads one terminal's words, nothing is clicked or typed.

#include "system/a11y/AtspiBus.h"
#include "system/a11y/AtspiScreenReader.h"
#include "system/a11y/ScreenOutline.h"

#include "QtStringMakers.h"

#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("The real accessibility bus lists its applications, and their caches become an outline", "[live]") {
    AtspiBus bus;
    if (!bus.isAvailable()) SKIP("no accessibility bus here: " + bus.unavailableReason().toStdString());
    AtspiScreenReader reader(&bus);
    std::optional<Result<QList<AtspiApp>>> got;
    reader.apps([&got](Result<QList<AtspiApp>> apps) { got = std::move(apps); });
    REQUIRE(QTest::qWaitFor([&got] { return got.has_value(); }, 15'000));
    REQUIRE(*got);
    const QList<AtspiApp> apps = got->value();
    INFO("applications: " << apps.size());
    REQUIRE_FALSE(apps.isEmpty());
    for (const AtspiApp &app : apps) CHECK_FALSE(app.bus.isEmpty());
    OutlineOptions options;
    options.scope = u"all"_s;
    options.ownApp = u"none-of-them"_s;     // the test runner is not Nexa; keep every window
    const ScreenOutline outline = buildScreenOutline(apps, options, [](const atspi::NodeRef &, int, bool) { return QString(); });
    INFO(outline.text.toStdString());
    CHECK_FALSE(outline.text.isEmpty());

    // The words of one text element, when some application exposes one (a terminal running this test, say).
    std::optional<atspi::NodeRef> textual;
    for (const AtspiApp &app : apps) {
        for (const atspi::Node &node : app.nodes) {
            if (node.implements("Text"_L1) && node.has(atspi::State::Showing) && node.is(atspi::Role::Terminal)) textual = node.ref;
        }
    }
    if (!textual) return;
    std::optional<Result<IScreenReader::TextSlice>> slice;
    reader.text(*textual, 0, 200, [&slice](Result<IScreenReader::TextSlice> result) { slice = std::move(result); });
    REQUIRE(QTest::qWaitFor([&slice] { return slice.has_value(); }, 15'000));
    REQUIRE(*slice);
    CHECK(slice->value().total >= 0);
    CHECK(slice->value().text.size() <= 200);
    std::optional<Result<QStringList>> actions;
    reader.actions(*textual, [&actions](Result<QStringList> result) { actions = std::move(result); });
    REQUIRE(QTest::qWaitFor([&actions] { return actions.has_value(); }, 15'000));
    REQUIRE(*actions);
}
