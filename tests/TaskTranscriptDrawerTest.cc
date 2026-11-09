// app/TaskTranscriptDrawer: a task's transcript replayed from the board, followed live, and asked more once done.

#include "app/ChatView.h"
#include "app/TaskTranscriptDrawer.h"

#include "MissionRig.h"
#include "QtStringMakers.h"

#include <QPlainTextEdit>
#include <QPushButton>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using test::Board;
using test::Reply;

TEST_CASE("Opening a finished task shows what was asked and answered, and asking more goes on in it") {
    Board board;
    board.env.scripts = {{Reply{u"The capital is Paris."_s, {}, {}}}};
    const TaskId id = board.ask(u"capital of France?"_s);
    REQUIRE(board.waitFor(id, TaskState::Done));

    TaskTranscriptDrawer drawer(board.mission.get());
    drawer.show();
    drawer.showTask(id);
    QTest::qWait(150);      // the chat renders a streamed reply at most ten times a second
    const QString shown = drawer.chat()->toPlainText();
    CHECK(shown.contains(u"capital of France?"_s));
    CHECK(shown.contains(u"The capital is Paris."_s));

    auto *send = drawer.findChild<QPushButton *>(u"send"_s);
    auto *input = drawer.findChild<QPlainTextEdit *>(u"input"_s);
    REQUIRE(send);
    CHECK(send->isEnabled());
    board.env.providers.at(0)->script = {Reply{u"About two million."_s, {}, {}}};
    input->setPlainText(u"and its population?"_s);
    send->click();
    REQUIRE(QTest::qWaitFor([&board, id] { return board.mission->transcript(id)->lastReply() == u"About two million."_s; },
                            test::kWaitMs));
    QTest::qWait(150);
    CHECK(drawer.chat()->toPlainText().contains(u"About two million."_s));
}
