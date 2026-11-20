#include "core/mission/AgentRole.h"

using namespace Qt::StringLiterals;

namespace nexa {

QString roleName(AgentRole role) {
    switch (role) {
    case AgentRole::Web: return u"Web"_s;
    case AgentRole::Code: return u"Code"_s;
    case AgentRole::Screen: return u"Screen"_s;
    case AgentRole::Phone: return u"Phone"_s;
    case AgentRole::General: return u"General"_s;
    }
    return u"General"_s;
}

QString roleKey(AgentRole role) {
    return roleName(role).toLower();
}

std::optional<AgentRole> roleFromKey(QStringView key) {
    for (const AgentRole role : kAgentRoles) {
        if (key.compare(roleKey(role), Qt::CaseInsensitive) == 0) return role;
    }
    return std::nullopt;
}

QString agentPrefix(AgentRole role) {
    switch (role) {
    case AgentRole::Web: return u"BROWSER"_s;
    case AgentRole::Code: return u"CODE"_s;
    case AgentRole::Screen: return u"SCREEN"_s;
    case AgentRole::Phone: return u"PHONE"_s;
    case AgentRole::General: return u"AGENT"_s;
    }
    return u"AGENT"_s;
}

QString agentIdFor(AgentRole role, int ordinal) {
    return u"%1-%2"_s.arg(agentPrefix(role)).arg(ordinal, 2, 10, QLatin1Char('0'));
}

QString roleBrief(AgentRole role) {
    switch (role) {
    case AgentRole::Web:
        return u"You are Nexa's web agent for this task: do the work in the browser with the browser_* tools (read pages "
               "as text snapshots, click, fill forms). Ask before anything that cannot be undone, such as submitting "
               "a form or paying."_s;
    case AgentRole::Code:
        return u"You are Nexa's coding agent for this task: read, write and test code with the commands and files "
               "you have. Say what you changed and how you checked it."_s;
    case AgentRole::Screen:
        return u"You are Nexa's screen agent for this task: work with what is on the PC's screen (screenshots, the "
               "pointer and the keyboard), only where the other tools cannot reach."_s;
    case AgentRole::Phone:
        return u"You are Nexa's phone agent for this task: work on the user's Android phone with the android_* "
               "tools."_s;
    case AgentRole::General:
        return u"You are Nexa's general agent for this task."_s;
    }
    return {};
}

Lane laneFor(AgentRole role) {
    switch (role) {
    case AgentRole::Web: return Lane::Web;
    case AgentRole::Code: return Lane::Code;
    case AgentRole::Screen: return Lane::Screen;
    case AgentRole::Phone: return Lane::Phone;
    case AgentRole::General: return Lane::General;
    }
    return Lane::General;
}

QString laneName(Lane lane) {
    switch (lane) {
    case Lane::Web: return u"WEB"_s;
    case Lane::Code: return u"CODE"_s;
    case Lane::Screen: return u"SCREEN"_s;
    case Lane::Phone: return u"PHONE"_s;
    case Lane::General: return u"GENERAL"_s;
    case Lane::Voice: return u"VOICE"_s;
    }
    return u"GENERAL"_s;
}

} // namespace nexa
