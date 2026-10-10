#include "core/agent/Conversation.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Conversation puts the system prompt first") {
    Conversation conversation(u"Be brief."_s);
    conversation.addUser(u"hi"_s);
    const ChatRequest request = conversation.buildRequest(u"m"_s);
    CHECK(request.model == u"m"_s);
    REQUIRE(request.messages.size() == 2);
    CHECK(request.messages[0].role == Role::System);
    CHECK(request.messages[1].text == u"hi"_s);
}

TEST_CASE("Conversation omits a blank system prompt") {
    Conversation conversation(u"  "_s);
    conversation.addUser(u"hi"_s);
    CHECK(conversation.buildRequest(u"m"_s).messages.size() == 1);
}

TEST_CASE("Conversation trims the oldest turns to the budget") {
    Conversation conversation;
    conversation.setHistoryBudget(25);
    conversation.addUser(u"first question"_s);     // 14
    conversation.addAssistant(u"first answer"_s);  // 12
    conversation.addUser(u"second"_s);             // 6
    conversation.addAssistant(u"reply two"_s);     // 9
    conversation.addUser(u"third"_s);              // 5

    const QList<ChatMessage> messages = conversation.buildRequest(u"m"_s).messages;
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].text == u"second"_s);
    CHECK(messages[2].text == u"third"_s);
}

TEST_CASE("Conversation always sends the latest message, even over budget") {
    Conversation conversation;
    conversation.setHistoryBudget(3);
    conversation.addUser(u"old"_s);
    conversation.addUser(u"a long latest message"_s);
    const QList<ChatMessage> messages = conversation.buildRequest(u"m"_s).messages;
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].text == u"a long latest message"_s);
}

TEST_CASE("Conversation never starts the history in the middle of a tool exchange") {
    Conversation conversation;
    conversation.setHistoryBudget(10);
    conversation.addUser(u"open the editor"_s);
    conversation.addAssistant({}, {ToolCall{.id = u"c1"_s, .name = u"open_app"_s, .argumentsJson = u"{\"name\":\"editor\"}"_s}});
    conversation.addToolResult(u"c1"_s, u"open_app"_s, u"launched gnome-text-editor"_s);

    const QList<ChatMessage> messages = conversation.buildRequest(u"m"_s).messages;
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].role == Role::User);
    CHECK(messages[2].role == Role::Tool);
}
