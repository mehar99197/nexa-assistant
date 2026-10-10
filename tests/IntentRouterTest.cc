#include "core/intent/IntentRouter.h"
#include "core/intent/PcIntents.h"

#include "QtStringMakers.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class FakeCatalog : public IAppCatalog {
public:
    double score(const QString &query) const override { return scores.value(query, 0.0); }
    QHash<QString, double> scores{{u"firefox"_s, 1.0}, {u"terminal"_s, 1.0}, {u"text editor"_s, 1.0}};
};

struct Fixture {
    explicit Fixture(QSet<QString> tools = {u"open_app"_s, u"close_app"_s, u"set_volume"_s, u"mute"_s,
                                            u"media_control"_s, u"take_screenshot"_s, u"run_command"_s})
        : available(std::move(tools)), router([this](const QString &tool) { return available.contains(tool); }) {
        router.add(std::make_unique<ControlIntent>());
        router.add(std::make_unique<MediaIntent>());
        router.add(std::make_unique<AudioIntent>());
        router.add(std::make_unique<ScreenIntent>());
        router.add(std::make_unique<AppIntent>(&catalog));
        router.add(std::make_unique<CommandIntent>(&catalog, [](const QString &program) {
            return program == u"ls" || program == u"echo" || program == u"apt" || program == u"df";
        }));
        router.add(std::make_unique<InputIntent>());
    }

    FakeCatalog catalog;
    QSet<QString> available;
    IntentRouter router;
};

QString toolOf(const IntentRouter &router, const QString &text) {
    const std::optional<IntentMatch> match = router.route(text);
    return match ? match->toolName : u"<none>"_s;
}

} // namespace

TEST_CASE("IntentRouter strips wake words, politeness and punctuation") {
    const IntentInput input = IntentRouter::prepare(u"Hey Nexa, could you please open Firefox for me!"_s);
    CHECK(input.normalized == u"open firefox"_s);
    CHECK(input.text == u"open Firefox"_s);
    CHECK(IntentRouter::prepare(u"  OPEN   the   Terminal.  "_s).normalized == u"open the terminal"_s);
}

TEST_CASE("IntentRouter opens and closes known apps without the model") {
    Fixture f;
    const auto open = f.router.route(u"open firefox"_s);
    REQUIRE(open.has_value());
    CHECK(open->toolName == u"open_app"_s);
    CHECK(open->arguments.value(u"name"_s).toString() == u"firefox"_s);
    CHECK(f.router.route(u"Please launch the text editor app"_s)->arguments.value(u"name"_s).toString() == u"text editor"_s);
    CHECK(toolOf(f.router, u"close the terminal"_s) == u"close_app"_s);
    CHECK(toolOf(f.router, u"open youtube.com"_s) == u"<none>"_s);       // unknown app → the model decides
}

TEST_CASE("IntentRouter leaves compound requests to the agent") {
    Fixture f;
    CHECK(toolOf(f.router, u"open a terminal and set volume to 30%"_s) == u"<none>"_s);
    CHECK(toolOf(f.router, u"open firefox, then mute"_s) == u"<none>"_s);
}

TEST_CASE("IntentRouter recognises volume phrases") {
    Fixture f;
    const auto level = [&f](const QString &text) { return f.router.route(text)->arguments.value(u"level"_s).toInt(); };
    const auto change = [&f](const QString &text) { return f.router.route(text)->arguments.value(u"change"_s).toInt(); };
    CHECK(level(u"volume 30"_s) == 30);
    CHECK(level(u"set the volume to 75%"_s) == 75);
    CHECK(level(u"volume at 40 percent"_s) == 40);
    CHECK(level(u"max volume"_s) == 100);
    CHECK(level(u"volume 250"_s) == 100);
    CHECK(change(u"volume up"_s) == 10);
    CHECK(change(u"turn the volume down by 20"_s) == -20);
    CHECK(change(u"turn up the volume"_s) == 10);
    CHECK(change(u"louder"_s) == 10);
    CHECK(change(u"quieter"_s) == -10);
    CHECK(f.router.route(u"mute"_s)->arguments.value(u"muted"_s).toBool());
    CHECK_FALSE(f.router.route(u"unmute the sound"_s)->arguments.value(u"muted"_s).toBool());
}

TEST_CASE("IntentRouter recognises media phrases but not bare chat words") {
    Fixture f;
    const auto action = [&f](const QString &text) {
        const auto match = f.router.route(text);
        return match ? match->arguments.value(u"action"_s).toString() : u"<none>"_s;
    };
    CHECK(action(u"pause"_s) == u"pause"_s);
    CHECK(action(u"play music"_s) == u"play"_s);
    CHECK(action(u"resume"_s) == u"play"_s);
    CHECK(action(u"next song"_s) == u"next"_s);
    CHECK(action(u"skip"_s) == u"next"_s);
    CHECK(action(u"previous track"_s) == u"previous"_s);
    CHECK(action(u"go back a song"_s) == u"previous"_s);
    CHECK(action(u"stop the music"_s) == u"stop"_s);
    CHECK(action(u"next"_s) == u"<none>"_s);
    CHECK(action(u"continue"_s) == u"<none>"_s);
    CHECK(action(u"back"_s) == u"<none>"_s);
}

TEST_CASE("IntentRouter treats stop words as panic") {
    Fixture f;
    for (const QString &text : {u"stop"_s, u"Cancel."_s, u"never mind"_s, u"stop everything"_s, u"Nexa, stop!"_s}) {
        INFO(text.toStdString());
        const auto match = f.router.route(text);
        REQUIRE(match.has_value());
        CHECK(match->panic);
    }
}

TEST_CASE("IntentRouter sends screen questions to the model with a screenshot") {
    Fixture f;
    const auto question = f.router.route(u"What's on my screen?"_s);
    REQUIRE(question.has_value());
    CHECK(question->toolName == u"take_screenshot"_s);
    CHECK(question->forwardToAgent);
    CHECK(question->agentPrompt == u"What's on my screen?"_s);

    const auto describe = f.router.route(u"take a screenshot"_s);
    REQUIRE(describe.has_value());
    CHECK(describe->forwardToAgent);
    CHECK(describe->agentPrompt == u"Describe what is on my screen."_s);
    CHECK(toolOf(f.router, u"see this"_s) == u"take_screenshot"_s);
    CHECK(toolOf(f.router, u"I want to see this movie"_s) == u"<none>"_s);
}

TEST_CASE("IntentRouter runs real commands verbatim and ignores chat that starts with run") {
    Fixture f;
    const auto ls = f.router.route(u"run ls -la ~/Downloads"_s);
    REQUIRE(ls.has_value());
    CHECK(ls->toolName == u"run_command"_s);
    CHECK(ls->arguments.value(u"command"_s).toString() == u"ls -la ~/Downloads"_s);

    const auto shell = f.router.route(u"$ echo Hi && ls"_s);  // && is shell syntax, not a compound request
    REQUIRE(shell.has_value());
    CHECK(shell->arguments.value(u"command"_s).toString() == u"echo Hi && ls"_s);

    CHECK(toolOf(f.router, u"run me through the steps"_s) == u"<none>"_s);
    CHECK(toolOf(f.router, u"run firefox"_s) == u"open_app"_s);
}

TEST_CASE("IntentRouter skips matches whose tool is not available") {
    Fixture f;
    CHECK(toolOf(f.router, u"run apt update as root"_s) == u"<none>"_s);   // run_root_command arrives in M3
    CHECK(toolOf(f.router, u"type Hello World"_s) == u"<none>"_s);          // type_text arrives in M3

    Fixture withInput({u"type_text"_s, u"press_keys"_s, u"run_root_command"_s});
    const auto typed = withInput.router.route(u"type \"Hello, World\""_s);
    REQUIRE(typed.has_value());
    CHECK(typed->arguments.value(u"text"_s).toString() == u"Hello, World"_s);
    const auto keys = withInput.router.route(u"press ctrl+alt+t"_s);
    REQUIRE(keys.has_value());
    CHECK(keys->arguments.value(u"keys"_s).toArray().size() == 3);
    const auto root = withInput.router.route(u"run apt update as root"_s);
    REQUIRE(root.has_value());
    CHECK(root->toolName == u"run_root_command"_s);
    CHECK(root->arguments.value(u"command"_s).toString() == u"apt update"_s);
}
