// app/AgentsRail: one card per agent at work, the slots in its header, the voice card's words.

#include "app/AgentsRail.h"
#include "app/TaskListModel.h"

#include "MissionRig.h"
#include "QtStringMakers.h"

#include <QLabel>
#include <QListView>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using test::Board;

TEST_CASE("The rail has a card for each agent at work and counts the slots") {
    Board board(2);
    board.env.scripts = {{}, {}, {}};
    TaskListModel model(board.mission.get());
    AgentsRail rail(&model);
    rail.show();
    board.ask(u"one"_s);
    board.ask(u"two"_s);
    board.ask(u"three"_s);      // waits for a slot: no card
    auto *cards = rail.findChild<QListView *>(u"agentList"_s);
    REQUIRE(cards);
    CHECK(cards->model()->rowCount() == 2);
    const QList<QLabel *> notes = rail.findChildren<QLabel *>(u"sectionNote"_s);
    CHECK(std::any_of(notes.cbegin(), notes.cend(), [](const QLabel *l) { return l->text() == u"2 active · 2 / 2 slots"_s; }));

    rail.setVoice(u"WAKE WORD ON"_s, u"Listening for “Hey Nexa”"_s, u"whisper.cpp · Piper"_s);
    CHECK(rail.findChild<QLabel *>(u"voiceChip"_s)->text() == u"WAKE WORD ON"_s);
    CHECK(rail.findChild<QLabel *>(u"voiceLine"_s)->text() == u"Listening for “Hey Nexa”"_s);
    board.mission->stopAll();
    CHECK(cards->model()->rowCount() == 0);
}
