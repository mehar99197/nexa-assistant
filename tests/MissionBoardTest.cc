// app/MissionBoard and app/TaskListModel over a real board with scripted models: the rows, the queue, the counts,
// and the row's button (pause, resume, clear) and the queue's ×.

#include "app/MissionBoard.h"
#include "app/TaskListModel.h"

#include "MissionRig.h"
#include "QtStringMakers.h"

#include <QLabel>
#include <QListView>
#include <QSignalSpy>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using test::Board;
using test::Reply;

namespace {

// Clicks the row's button (its right end) of the row at `row` in `view`.
void clickButton(QListView *view, int row) {
    const QRect rect = view->visualRect(view->model()->index(row, 0));
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, QPoint(rect.right() - 1 - 10 - 17, rect.top() + (rect.height() - 8) / 2));
}

} // namespace

TEST_CASE("The board lists what works and queues the rest, and its counts say so") {
    Board board(1);
    board.env.scripts = {{}, {}};
    TaskListModel model(board.mission.get());
    MissionBoard view(&model);
    view.resize(900, 600);
    view.show();
    const TaskId first = board.ask(u"write the report"_s);
    const TaskId second = board.ask(u"refactor the parser code"_s);

    auto *tasks = view.findChild<QListView *>(u"taskList"_s);
    auto *queue = view.findChild<QListView *>(u"queueList"_s);
    REQUIRE(tasks);
    REQUIRE(queue);
    CHECK(tasks->model()->rowCount() == 1);
    CHECK(queue->model()->rowCount() == 1);
    CHECK(queue->isVisible());
    CHECK(queue->model()->index(0, 0).data(TaskListModel::IdRole).toULongLong() == second);
    CHECK(queue->model()->index(0, 0).data(TaskListModel::QueuePositionRole).toInt() == 1);
    const QString counts = view.findChild<QLabel *>(u"boardCounts"_s)->text();
    CHECK(counts.contains(u"1 running"_s));
    CHECK(counts.contains(u"1 queued"_s));

    QSignalSpy pause(&view, &MissionBoard::pauseRequested);
    clickButton(tasks, 0);
    REQUIRE(pause.size() == 1);
    CHECK(pause.at(0).at(0).toULongLong() == first);

    QSignalSpy stop(&view, &MissionBoard::stopRequested);
    clickButton(queue, 0);
    REQUIRE(stop.size() == 1);
    CHECK(stop.at(0).at(0).toULongLong() == second);
    board.mission->stopAll();
}

TEST_CASE("A finished task's button clears it, a paused one's resumes it") {
    Board board;
    board.env.scripts = {{Reply{u"done"_s, {}, {}}}, {}};
    TaskListModel model(board.mission.get());
    MissionBoard view(&model);
    view.resize(900, 600);
    view.show();
    const TaskId done = board.ask(u"say done"_s);
    REQUIRE(board.waitFor(done, TaskState::Done));
    const TaskId paused = board.ask(u"think slowly"_s);
    board.mission->pause(paused);
    auto *tasks = view.findChild<QListView *>(u"taskList"_s);
    REQUIRE(tasks->model()->rowCount() == 2);
    // What works (the paused one) above the finished.
    CHECK(tasks->model()->index(0, 0).data(TaskListModel::IdRole).toULongLong() == paused);

    QSignalSpy resume(&view, &MissionBoard::resumeRequested);
    QSignalSpy remove(&view, &MissionBoard::removeRequested);
    clickButton(tasks, 0);
    clickButton(tasks, 1);
    REQUIRE(resume.size() == 1);
    CHECK(resume.at(0).at(0).toULongLong() == paused);
    REQUIRE(remove.size() == 1);
    CHECK(remove.at(0).at(0).toULongLong() == done);
    board.mission->stopAll();
}

#include "app/ActivityStream.h"
#include "app/AgentsRail.h"
#include "ui/NexaDesigns.h"
#include "ui/NexaTheme.h"
#include "ui/NexaThemes.h"

#include <QApplication>
#include <QHBoxLayout>

// NEXA_TEST_SCREENSHOTS=<dir>: the control room's panels at work — agents running, one waiting for the user, one in
// the queue — in the Mission palette, kept as a picture to look at. Without the variable it only checks they show.
TEST_CASE("Mission Control's panels at work, as a picture") {
    const QString directory = qEnvironmentVariable("NEXA_TEST_SCREENSHOTS");
    Board board(3);
    board.policy.setMode(PolicyMode::ConfirmAll);
    board.env.scripts = {{}, {Reply{{}, {test::call(u"c1"_s, u"echo"_s, u"{\"msg\":\"Submit registration\"}"_s)}, {}}}, {},
                         {}};
    TaskListModel model(board.mission.get());
    QWidget room;
    auto *row = new QHBoxLayout(&room);
    auto *rail = new AgentsRail(&model, &room);
    auto *boardView = new MissionBoard(&model, &room);
    auto *stream = new ActivityStream(board.mission.get(), &room);
    rail->setFixedWidth(290);
    stream->setFixedWidth(330);
    row->addWidget(rail);
    row->addWidget(boardView, 1);
    row->addWidget(stream);
    room.resize(1280, 640);
    if (!directory.isEmpty()) {
        const ThemePack pack = NexaThemes::resolve(u"mission-dark"_s);
        NexaTheme::apply(qApp, pack.tokens, pack.style, NexaDesigns::resolve(u"mission"_s));
    }
    room.show();
    board.ask(u"Refactor invoice module and run tests"_s);
    const TaskId web = board.ask(u"Register semester courses on mul.edu.pk"_s);
    board.ask(u"Write unit tests for the auth guard"_s);
    board.ask(u"Check my inbox and draft replies to unread mail"_s);
    REQUIRE(QTest::qWaitFor([&board, web] { return board.state(web) == TaskState::AwaitingYou; }, test::kWaitMs));
    CHECK(board.mission->queue().size() == 1);
    QTest::qWait(1200);     // a tick of the sparklines and the clocks
    if (!directory.isEmpty()) room.grab().save(directory + u"/mission-panels.png"_s);
    if (!directory.isEmpty()) qApp->setStyleSheet({});
    board.mission->stopAll();
}
