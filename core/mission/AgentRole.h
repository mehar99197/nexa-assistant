#pragma once

#include <QString>
#include <QStringView>

#include <array>
#include <optional>

namespace nexa {

// The kinds of agent a task can be led by. Each has its own model (Settings → Mission, ModelFit), its own words in
// the system prompt (roleBrief) and its own lane in the live activity.
enum class AgentRole { Web, Code, Screen, Phone, General };
inline constexpr int kAgentRoleCount = 5;
inline constexpr std::array<AgentRole, kAgentRoleCount> kAgentRoles{AgentRole::Web, AgentRole::Code, AgentRole::Screen,
                                                                    AgentRole::Phone, AgentRole::General};

QString roleName(AgentRole role);                       // "Web"
QString roleKey(AgentRole role);                        // "web": config keys and the delegate_task tool
std::optional<AgentRole> roleFromKey(QStringView key);
QString agentPrefix(AgentRole role);                    // "BROWSER", "CODE", "SCREEN", "PHONE", "AGENT"
QString agentIdFor(AgentRole role, int ordinal);        // "BROWSER-01"
// What the role is for, one paragraph added to its agent's system prompt.
QString roleBrief(AgentRole role);

// The lanes of the live activity (its filter chips): the five roles, and the voice.
enum class Lane { Web, Code, Screen, Phone, General, Voice };
inline constexpr int kLaneCount = 6;
Lane laneFor(AgentRole role);
QString laneName(Lane lane);                            // "WEB", …, "VOICE"

} // namespace nexa
