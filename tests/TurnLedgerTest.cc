// core/ai/TurnLedger and TranscriptWriter: what a warm backend session holds, and what to send it.

#include "core/ai/TranscriptWriter.h"
#include "core/ai/TurnLedger.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

ChatMessage sys(const QString &text = u"S"_s) { return {.role = Role::System, .text = text}; }
ChatMessage u(const QString &text, const QList<ImageAttachment> &images = {}) {
    return {.role = Role::User, .text = text, .images = images};
}
ChatMessage a(const QString &text) { return {.role = Role::Assistant, .text = text}; }

ChatRequest request(const QList<ChatMessage> &messages, const QString &model = u"m"_s) {
    return ChatRequest{.model = model, .messages = messages};
}

QStringList texts(const QList<ChatMessage> &messages) {
    QStringList result;
    for (const ChatMessage &message : messages) result.append(message.text);
    return result;
}

} // namespace

TEST_CASE("An empty ledger starts fresh with the history before the new turns") {
    TurnLedger ledger;
    CHECK(ledger.isEmpty());
    const TurnPlan plan = ledger.plan(request({sys(), u(u"u1"_s)}));
    CHECK(plan.fresh);
    CHECK(plan.history.isEmpty());
    CHECK(texts(plan.turns) == QStringList{u"u1"_s});
    CHECK(plan.system == u"S"_s);
}

TEST_CASE("After a reply, the next user turn continues") {
    TurnLedger ledger;
    ledger.commit(request({sys(), u(u"u1"_s)}));
    CHECK_FALSE(ledger.isEmpty());
    const TurnPlan plan = ledger.plan(request({sys(), u(u"u1"_s), a(u"a1"_s), u(u"u2"_s)}));
    CHECK_FALSE(plan.fresh);
    CHECK(texts(plan.turns) == QStringList{u"u2"_s});
    CHECK_FALSE(plan.instructionsChanged);
}

TEST_CASE("Tool results after a call continue the same session") {
    TurnLedger ledger;
    ledger.commit(request({sys(), u(u"u1"_s)}));
    const QString result = u"<tool_result name=\"echo\">\nechoed x\n</tool_result>"_s;
    const TurnPlan plan = ledger.plan(request(
        {sys(), u(u"u1"_s), a(u"<tool_call>{\"name\":\"echo\",\"arguments\":{}}</tool_call>"_s), u(result)}));
    CHECK_FALSE(plan.fresh);
    CHECK(texts(plan.turns) == QStringList{result});
}

TEST_CASE("The assistant's own words never have to match") {
    TurnLedger ledger;
    ledger.commit(request({sys(), u(u"u1"_s)}));
    CHECK_FALSE(ledger.plan(request({sys(), u(u"u1"_s), a(u"anything at all"_s), u(u"u2"_s)})).fresh);
}

TEST_CASE("A trimmed history still continues") {
    TurnLedger ledger;
    ledger.commit(request({sys(), u(u"u1"_s), a(u"a1"_s), u(u"u2"_s)}));
    const TurnPlan plan = ledger.plan(request({sys(), u(u"u2"_s), a(u"a2"_s), u(u"u3"_s)}));
    CHECK_FALSE(plan.fresh);
    CHECK(texts(plan.turns) == QStringList{u"u3"_s});
}

TEST_CASE("An older screenshot turned into a note still matches") {
    TurnLedger ledger;
    ledger.commit(request({sys(), u(u"look"_s, {ImageAttachment{"PNG", u"image/png"_s}})}));
    const TurnPlan plan =
        ledger.plan(request({sys(), u(u"look\n[1 earlier image(s) omitted]"_s), a(u"a1"_s), u(u"u2"_s)}));
    CHECK_FALSE(plan.fresh);
    CHECK(texts(plan.turns) == QStringList{u"u2"_s});
}

TEST_CASE("A different model, a new conversation or edited history starts fresh") {
    TurnLedger ledger;
    ledger.commit(request({sys(), u(u"u1"_s)}));

    const TurnPlan otherModel = ledger.plan(request({sys(), u(u"u1"_s), a(u"a1"_s), u(u"u2"_s)}, u"m2"_s));
    CHECK(otherModel.fresh);
    CHECK(texts(otherModel.history) == QStringList{u"u1"_s, u"a1"_s});
    CHECK(texts(otherModel.turns) == QStringList{u"u2"_s});

    CHECK(ledger.plan(request({sys(), u(u"u9"_s)})).fresh);
    CHECK(ledger.plan(request({sys(), u(u"u1 edited"_s), a(u"a1"_s), u(u"u2"_s)})).fresh);
}

TEST_CASE("Changed instructions are reported") {
    TurnLedger ledger;
    ledger.commit(request({sys(u"A"_s), u(u"u1"_s)}));
    const TurnPlan plan = ledger.plan(request({sys(u"B"_s), u(u"u1"_s), a(u"a1"_s), u(u"u2"_s)}));
    CHECK_FALSE(plan.fresh);
    CHECK(plan.instructionsChanged);
    CHECK(plan.system == u"B"_s);
}

TEST_CASE("reset forgets the session") {
    TurnLedger ledger;
    ledger.commit(request({sys(), u(u"u1"_s)}));
    ledger.reset();
    CHECK(ledger.isEmpty());
    CHECK(ledger.plan(request({sys(), u(u"u1"_s), a(u"a1"_s), u(u"u2"_s)})).fresh);
}

TEST_CASE("TranscriptWriter labels each earlier turn") {
    CHECK(TranscriptWriter::history({u(u"hi"_s), a(u"hello"_s)})
          == u"[Earlier in this conversation]\n\nUser: hi\n\nNexa: hello"_s);
    CHECK(TranscriptWriter::history({}).isEmpty());
    CHECK(TranscriptWriter::turnText({u(u"one"_s), u(u"two"_s)}) == u"one\n\ntwo"_s);
    CHECK(TranscriptWriter::instructions(u"S"_s)
          == u"[Instructions from Nexa — follow them for the whole conversation]\n\nS"_s);
    CHECK(TranscriptWriter::instructionsUpdate(u"S"_s)
          == u"[Nexa's instructions have changed; these replace the earlier ones]\n\nS"_s);
}
