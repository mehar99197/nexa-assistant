// core/intent/MemoryIntents: "remember that …", "forget that …", "what do you know about me" on the
// fast path, in the order app/main.cc builds the chain — and the ordinary chat it must leave alone.

#include "core/config/Config.h"
#include "core/intent/IntentRouter.h"
#include "core/intent/MemoryIntents.h"
#include "core/intent/PcIntents.h"
#include "core/memory/MemoryStore.h"
#include "core/memory/MemoryTools.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"

#include <QJsonArray>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class NoApps : public IAppCatalog {
public:
    double score(const QString &) const override { return 0; }
};

struct Fixture {
    Fixture() : store(dir.filePath(u"memory.json"_s)), router([this](const QString &tool) { return registry.contains(tool); }) {
        syncMemoryTools(registry, &store, MemoryConfig{});
        router.add(std::make_unique<ControlIntent>());
        router.add(std::make_unique<MemoryIntent>());
        router.add(std::make_unique<MediaIntent>());
        router.add(std::make_unique<AudioIntent>());
        router.add(std::make_unique<ScreenIntent>());
        router.add(std::make_unique<AppIntent>(&catalog));
        router.add(std::make_unique<CommandIntent>(&catalog, [](const QString &) { return false; }));
        router.add(std::make_unique<InputIntent>());
    }

    // The match's arguments fit the tool's own schema: no unknown keys, every required one there.
    void checkShape(const IntentMatch &match) const {
        const QJsonObject parameters = registry.find(match.toolName)->spec().parameters;
        const QJsonObject properties = parameters.value(u"properties"_s).toObject();
        for (const QString &key : match.arguments.keys()) CHECK(properties.contains(key));
        for (const QJsonValue &required : parameters.value(u"required"_s).toArray()) {
            CHECK(match.arguments.contains(required.toString()));
        }
    }

    QTemporaryDir dir;
    MemoryStore store;
    ToolRegistry registry;
    NoApps catalog;
    IntentRouter router;
};

} // namespace

TEST_CASE("Remember, forget and what-do-you-know go straight to the memory tools") {
    Fixture f;
    const auto remember = f.router.route(u"Nexa, remember that I like tea and coffee"_s);
    REQUIRE(remember.has_value());
    CHECK(remember->toolName == u"remember"_s);
    CHECK(remember->arguments.value(u"text"_s).toString() == u"I like tea and coffee"_s);   // one fact, not compound
    CHECK(remember->arguments.value(u"origin"_s).toString() == u"asked"_s);
    f.checkShape(*remember);

    CHECK(f.router.route(u"remember my sister's name is Sara."_s)->arguments.value(u"text"_s).toString()
          == u"my sister's name is Sara"_s);
    CHECK(f.router.route(u"please keep in mind that I'm vegetarian"_s)->arguments.value(u"text"_s).toString()
          == u"I'm vegetarian"_s);
    CHECK(f.router.route(u"remember: the car is a white Corolla"_s)->arguments.value(u"text"_s).toString()
          == u"the car is a white Corolla"_s);
    // A dictated fact keeps its last word ("now" is not politeness here).
    CHECK(f.router.route(u"remember that I work from home now"_s)->arguments.value(u"text"_s).toString()
          == u"I work from home now"_s);

    const auto forget = f.router.route(u"forget that my office is in Multan"_s);
    REQUIRE(forget.has_value());
    CHECK(forget->toolName == u"forget"_s);
    CHECK(forget->arguments.value(u"query"_s).toString() == u"my office is in Multan"_s);
    f.checkShape(*forget);
    CHECK(f.router.route(u"forget what I told you about Sara"_s)->arguments.value(u"query"_s).toString() == u"Sara"_s);

    const auto known = f.router.route(u"What do you know about me?"_s);
    REQUIRE(known.has_value());
    CHECK(known->toolName == u"search_memory"_s);
    f.checkShape(*known);
}

TEST_CASE("Chat about remembering and forgetting is left to the model") {
    Fixture f;
    const auto none = [&f](const QString &text) {
        INFO(text.toStdString());
        const auto match = f.router.route(text);
        CHECK((!match || match->toolName.isEmpty()));
        return match;
    };
    none(u"do you remember when we talked about Lahore"_s);
    none(u"I forget my keys all the time"_s);
    none(u"remember that time we went to Murree?"_s);
    none(u"remember that time we went to Murree"_s);
    none(u"remember to buy milk"_s);
    none(u"forget about it"_s);
    none(u"forget everything"_s);
    none(u"what do you know about quantum physics"_s);
    none(u"can you remember things?"_s);
    // "forget it" is still a stop.
    const auto stop = f.router.route(u"forget it"_s);
    REQUIRE(stop.has_value());
    CHECK(stop->panic);
}

TEST_CASE("With memory off, its phrases go to the model") {
    Fixture f;
    MemoryConfig off;
    off.enabled = false;
    syncMemoryTools(f.registry, &f.store, off);
    CHECK_FALSE(f.router.route(u"remember that I like tea"_s).has_value());
}
