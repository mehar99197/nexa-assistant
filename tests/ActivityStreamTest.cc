// app/ActivityStream: the lines of what happens, filtered by lane, and a card per question with Approve and Deny.

#include "app/ActivityStream.h"

#include "MissionRig.h"
#include "QtStringMakers.h"

#include <QAbstractButton>
#include <QListView>
#include <QPushButton>
#include <QSignalSpy>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using test::Board;
using test::Reply;
using test::call;

TEST_CASE("A question becomes a card; Approve answers it and the card goes") {
    Board board;
    board.policy.setMode(PolicyMode::ConfirmAll);
    board.env.scripts = {{Reply{{}, {call(u"c1"_s, u"echo"_s, u"{\"msg\":\"a\"}"_s)}, {}}, Reply{u"done"_s, {}, {}}}};
    ActivityStream stream(board.mission.get());
    QObject::connect(&stream, &ActivityStream::approved, board.mission.get(), &MissionControl::approve);
    stream.show();
    const TaskId id = board.ask(u"search github.com for an echo"_s);
    REQUIRE(QTest::qWaitFor([&stream] { return stream.findChild<QFrame *>(u"approvalCard"_s) != nullptr; }, test::kWaitMs));
    auto *approve = stream.findChild<QPushButton *>(u"approve"_s);
    REQUIRE(approve);
    approve->click();
    REQUIRE(board.waitFor(id, TaskState::Done));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(stream.findChild<QFrame *>(u"approvalCard"_s) == nullptr);
}

TEST_CASE("The lane chips show one lane's lines, ALL every line") {
    Board board;
    board.env.scripts = {{Reply{u"ok"_s, {}, {}}}, {Reply{u"ok"_s, {}, {}}}};
    ActivityStream stream(board.mission.get());
    stream.show();
    const TaskId web = board.ask(u"open github.com"_s);
    const TaskId code = board.ask(u"refactor the parser code"_s);
    REQUIRE(board.waitFor(web, TaskState::Done));
    REQUIRE(board.waitFor(code, TaskState::Done));
    auto *lines = stream.findChild<QListView *>(u"activityList"_s);
    const int all = lines->model()->rowCount();
    CHECK(all >= 4);
    QAbstractButton *webChip = nullptr;
    for (QPushButton *chip : stream.findChildren<QPushButton *>(u"laneChip"_s)) {
        if (chip->text() == u"WEB"_s) webChip = chip;
    }
    REQUIRE(webChip);
    webChip->click();
    const int webLines = lines->model()->rowCount();
    CHECK(webLines > 0);
    CHECK(webLines < all);
    for (int row = 0; row < webLines; ++row) {
        CHECK(Lane(lines->model()->index(row, 0).data(ActivityModel::LaneRole).toInt()) == Lane::Web);
    }
}
