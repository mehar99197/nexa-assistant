#pragma once

namespace nexa {

class SkillCatalog;
class ToolRegistry;
struct SkillsConfig;

// use_skill (read a skill's instructions) and save_skill (write one of the user's) — added while
// skills are on, removed while they are off; the catalog told which skills are turned off.
void syncSkillTools(ToolRegistry &registry, SkillCatalog *catalog, const SkillsConfig &config);

} // namespace nexa
