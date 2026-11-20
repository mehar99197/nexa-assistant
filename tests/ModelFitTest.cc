// core/mission/ModelFit: the model each role gets when a task says "Auto", from what the providers can do.

#include "core/mission/ModelFit.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

ProviderFacts facts(const QString &id, const QString &kind, const QString &model = {}, bool vision = false,
                    const QStringList &imageModels = {}, bool first = false) {
    return ProviderFacts{id, kind, id, model, vision, imageModels, first};
}

} // namespace

TEST_CASE("Code goes to a coding program: Codex, then opencode, then agy") {
    const QList<ProviderFacts> all{facts(u"groq"_s, u"openai-compatible"_s, u"llama"_s, false, {}, true),
                                   facts(u"agy"_s, u"agy"_s), facts(u"opencode"_s, u"opencode"_s, u"oc/fast"_s),
                                   facts(u"codex"_s, u"codex"_s)};
    CHECK(ModelFit::defaultFor(AgentRole::Code, all) == ModelChoice{u"codex"_s, {}});
    CHECK(ModelFit::defaultFor(AgentRole::Code, all.mid(0, 3)) == ModelChoice{u"opencode"_s, u"oc/fast"_s});
    CHECK(ModelFit::defaultFor(AgentRole::Code, all.mid(0, 2)) == ModelChoice{u"agy"_s, {}});
    CHECK(ModelFit::defaultFor(AgentRole::Code, all.mid(0, 1)).isAuto());
}

TEST_CASE("The screen and the phone go to a provider that sees") {
    // The first choice sees: it stays (Auto keeps the chain's fallbacks).
    const QList<ProviderFacts> seeingFirst{facts(u"gemini"_s, u"gemini"_s, u"g-flash"_s, true, {}, true),
                                           facts(u"agy"_s, u"agy"_s)};
    CHECK(ModelFit::defaultFor(AgentRole::Screen, seeingFirst).isAuto());
    // A blind first choice: the first provider that sees, on a model that sees.
    const QList<ProviderFacts> blindFirst{facts(u"groq"_s, u"openai-compatible"_s, u"llama"_s, false, {}, true),
                                          facts(u"opencode"_s, u"opencode"_s, u"oc/text-only"_s, false,
                                                {u"oc/vision"_s, u"oc/vision-2"_s}),
                                          facts(u"gemini"_s, u"gemini"_s, u"g-flash"_s, true)};
    CHECK(ModelFit::defaultFor(AgentRole::Phone, blindFirst) == ModelChoice{u"opencode"_s, u"oc/vision"_s});
    // Nobody sees: Auto, and the agent leaves pictures out as ever.
    CHECK(ModelFit::defaultFor(AgentRole::Screen, {facts(u"agy"_s, u"agy"_s, {}, false, {}, true)}).isAuto());
}

TEST_CASE("The web and general work stay on the first choice") {
    const QList<ProviderFacts> all{facts(u"opencode"_s, u"opencode"_s, {}, false, {}, true), facts(u"codex"_s, u"codex"_s)};
    CHECK(ModelFit::defaultFor(AgentRole::Web, all).isAuto());
    CHECK(ModelFit::defaultFor(AgentRole::General, all).isAuto());
}

TEST_CASE("Settings → Mission wins while its provider is still there") {
    const QList<ProviderFacts> all{facts(u"opencode"_s, u"opencode"_s, {}, false, {}, true), facts(u"codex"_s, u"codex"_s),
                                   facts(u"agy"_s, u"agy"_s)};
    MissionConfig mission;
    mission.roleModels.insert(u"code"_s, ModelChoice{u"agy"_s, u"gemini-3.8-pro"_s});
    mission.roleModels.insert(u"web"_s, ModelChoice{u"groq"_s, u"gone"_s});      // removed since
    CHECK(ModelFit::resolve(AgentRole::Code, mission, all) == ModelChoice{u"agy"_s, u"gemini-3.8-pro"_s});
    CHECK(ModelFit::resolve(AgentRole::Web, mission, all).isAuto());
    CHECK(ModelFit::resolve(AgentRole::General, mission, all).isAuto());
}

TEST_CASE("A program provider answers one conversation at a time, so a task needs its own") {
    CHECK(ModelFit::needsOwnInstance(u"agy"_s));
    CHECK(ModelFit::needsOwnInstance(u"opencode"_s));
    CHECK(ModelFit::needsOwnInstance(u"codex"_s));
    CHECK_FALSE(ModelFit::needsOwnInstance(u"openai-compatible"_s));
    CHECK_FALSE(ModelFit::needsOwnInstance(u"anthropic"_s));
}
