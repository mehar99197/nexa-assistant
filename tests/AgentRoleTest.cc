// core/mission/AgentRole: names, keys and agent ids of the roles, and the lanes of the activity stream.

#include "core/mission/AgentRole.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Every role has a name, a key that reads back, an agent id and a lane") {
    for (const AgentRole role : kAgentRoles) {
        INFO(roleKey(role).toStdString());
        CHECK_FALSE(roleName(role).isEmpty());
        CHECK(roleFromKey(roleKey(role)) == role);
        CHECK_FALSE(roleBrief(role).isEmpty());
        CHECK(laneName(laneFor(role)) == roleKey(role).toUpper());
    }
    CHECK_FALSE(roleFromKey(u"pilot"_s).has_value());
    CHECK(agentIdFor(AgentRole::Web, 1) == u"BROWSER-01"_s);
    CHECK(agentIdFor(AgentRole::Code, 12) == u"CODE-12"_s);
    CHECK(agentIdFor(AgentRole::General, 3) == u"AGENT-03"_s);
    CHECK(laneName(Lane::Voice) == u"VOICE"_s);
}
