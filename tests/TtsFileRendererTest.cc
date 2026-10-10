// voice/tts/TtsFileRenderer::speakable: what of a reply the phone reads aloud.

#include "voice/tts/TtsFileRenderer.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("A Markdown reply is read as plain sentences, code left out") {
    const QString reply = u"## Done\n\nI opened **Firefox** for you.\n\n```bash\nfirefox &\n```\n\n- It is on the left.\n"_s;
    const QString spoken = TtsFileRenderer::speakable(reply);
    CHECK(spoken.contains(u"I opened Firefox for you."_s));
    CHECK(spoken.contains(u"It is on the left."_s));
    CHECK_FALSE(spoken.contains(u"**"_s));
    CHECK_FALSE(spoken.contains(u"firefox &"_s));
    CHECK_FALSE(spoken.contains(u'\n'));
}

TEST_CASE("A long reply is read up to a sentence end that fits") {
    QString reply;
    for (int i = 0; i < 40; ++i) reply += u"This is sentence number %1 of a long answer. "_s.arg(i);
    const QString spoken = TtsFileRenderer::speakable(reply, 200);
    CHECK(spoken.size() <= 200);
    CHECK(spoken.endsWith(u'.'));
    CHECK(spoken.startsWith(u"This is sentence number 0"_s));
}

TEST_CASE("One sentence longer than the limit is cut at a word") {
    const QString spoken = TtsFileRenderer::speakable(QString(30, u'x').append(u' ').repeated(20), 100);
    CHECK(spoken.size() <= 101);
    CHECK(spoken.endsWith(u'…'));
}

TEST_CASE("A reply that is only code has nothing to read") {
    CHECK(TtsFileRenderer::speakable(u"```\nls -la\n```"_s).isEmpty());
    CHECK(TtsFileRenderer::speakable(QString()).isEmpty());
}
