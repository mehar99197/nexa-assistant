// system/android/AndroidTools against tests/fake-adb/adb: the phone's screen as text and the taps by ref.

#include "system/android/AndroidTools.h"

#include "core/tools/ToolRegistry.h"
#include "system/android/AdbClient.h"
#include "system/android/AndroidDevice.h"

#include "QtStringMakers.h"

#include <QJsonObject>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 15'000;

struct Rig {
    Rig() : phone(&adb) {
#ifdef Q_OS_WIN
        adb.setProgram(QStringLiteral(NEXA_TESTS_DIR "/fake-adb/adb.cmd"));
#else
        adb.setProgram(QStringLiteral(NEXA_TESTS_DIR "/fake-adb/adb"));
#endif
        registerAndroidTools(registry, &phone, nullptr);
    }

    Result<ToolResult> run(const QString &tool, const QJsonObject &arguments = {}) {
        Result<ToolResult> out = Error{ErrorCode::Unknown, u"never answered"_s};
        bool done = false;
        REQUIRE(registry.find(tool) != nullptr);
        registry.find(tool)->invoke(arguments, [&out, &done](Result<ToolResult> result) {
            out = std::move(result);
            done = true;
        });
        REQUIRE(QTest::qWaitFor([&done] { return done; }, kWaitMs));
        return out;
    }

    AdbClient adb;
    AndroidDevice phone;
    ToolRegistry registry;
};

} // namespace

TEST_CASE("android_snapshot reads the phone's screen as text with a ref per element") {
    Rig rig;
    const Result<ToolResult> snapshot = rig.run(u"android_snapshot"_s);
    REQUIRE(snapshot);
    CHECK(snapshot.value().text.startsWith(u"Phone screen — com.google.android.youtube"_s));
    CHECK(snapshot.value().text.contains(u"- button \"Search\" [tap] [ref=p2]"_s));
    CHECK_FALSE(snapshot.value().image.has_value());
    CHECK_FALSE(rig.registry.find(u"android_snapshot"_s)->spec().mutating);
    const Result<ToolResult> filtered = rig.run(u"android_snapshot"_s, {{u"filter"_s, u"shorts"_s}});
    REQUIRE(filtered);
    CHECK(filtered.value().text.split(u'\n').size() == 2);
}

TEST_CASE("android_tap and android_type take a ref from the last snapshot, and ask for one when it is unknown") {
    Rig rig;
    REQUIRE(rig.run(u"android_snapshot"_s));
    const Result<ToolResult> tapped = rig.run(u"android_tap"_s, {{u"ref"_s, u"p2"_s}});
    REQUIRE(tapped);
    CHECK(tapped.value().text == u"Tapped \"Search\" (p2) at 978, 138 on the phone."_s);
    const Result<ToolResult> typed = rig.run(u"android_type"_s, {{u"ref"_s, u"p2"_s}, {u"text"_s, u"qt widgets"_s}});
    REQUIRE(typed);
    CHECK(typed.value().text == u"Tapped \"Search\" (p2), then typed 10 characters on the phone."_s);
    const Result<ToolResult> unknown = rig.run(u"android_tap"_s, {{u"ref"_s, u"p99"_s}});
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == ErrorCode::InvalidArgument);
    CHECK(unknown.error().message.contains(u"no element p99 in the last android_snapshot"_s));
    const Result<ToolResult> neither = rig.run(u"android_tap"_s, {});
    REQUIRE_FALSE(neither);
    CHECK(neither.error().message.contains(u"ref from android_snapshot, or both"_s));
}

TEST_CASE("android_wait_for answers as soon as the words are on the screen") {
    Rig rig;
    const Result<ToolResult> waited = rig.run(u"android_wait_for"_s, {{u"text"_s, u"Subscriptions"_s}, {u"timeout_ms"_s, 3000}});
    REQUIRE(waited);
    CHECK(waited.value().text.startsWith(u"\"Subscriptions\" appeared after "_s));
    CHECK(waited.value().text.contains(u"Phone screen — com.google.android.youtube"_s));
    const Result<ToolResult> never = rig.run(u"android_wait_for"_s, {{u"text"_s, u"Nothing like this"_s}, {u"timeout_ms"_s, 600}});
    REQUIRE_FALSE(never);
    CHECK(never.error().code == ErrorCode::Timeout);
    CHECK(never.error().message.startsWith(u"\"Nothing like this\" did not appear within 0.6 s."_s));
}
