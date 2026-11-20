#include "core/skills/SkillTools.h"

#include "core/config/Config.h"
#include "core/skills/SkillCatalog.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolArguments.h"
#include "core/tools/ToolRegistry.h"

#include <QJsonObject>
#include <QPointer>

using namespace Qt::StringLiterals;

namespace nexa {

void syncSkillTools(ToolRegistry &registry, SkillCatalog *catalog, const SkillsConfig &config) {
    if (catalog) catalog->setDisabled(config.disabled);
    if (!config.enabled || !catalog) {
        registry.remove(u"use_skill"_s);
        registry.remove(u"save_skill"_s);
        return;
    }
    const QPointer<SkillCatalog> skills(catalog);

    ToolSpec use{u"use_skill"_s,
                 u"Read the instructions of one of the skills listed in the system prompt, before doing the job it "
                 "is for; then follow them."_s,
                 schema::object({{u"name"_s, schema::string(u"The skill's name, as listed"_s)}}, {u"name"_s})};
    use.mutating = false;
    registry.add(FunctionTool::sync(use, [skills](const QJsonObject &args) -> Result<ToolResult> {
        if (!skills) return Error{ErrorCode::NotSupported, u"Skills are not available."_s};
        const Result<QString> name = toolargs::requiredString(args, u"name"_s);
        if (!name) return name.error();
        skills->refresh();
        const Skill *skill = skills->find(name.value());
        if (!skill) return Error{ErrorCode::InvalidArgument, u"There is no skill \"%1\"."_s.arg(name.value())};
        if (!skill->enabled) return Error{ErrorCode::PermissionDenied, u"The skill \"%1\" is turned off in Settings → Skills."_s.arg(name.value())};
        QString text = u"Skill \"%1\" — follow these instructions:\n\n%2"_s.arg(skill->name(), skill->document.body);
        const QStringList files = skills->extraFiles(*skill);
        if (!files.isEmpty()) {
            text += u"\n\nThe skill's folder is %1; its other files (open them with read_file when the instructions "
                    "need them):\n- %2"_s.arg(skill->directory, files.join(u"\n- "_s));
        }
        return toolargs::text(text);
    }));

    ToolSpec save{u"save_skill"_s,
                  u"Save a skill for later conversations when the user asks you to make one (\"make a skill for my "
                  "morning routine\"): a name, a description saying when to use it, and the step-by-step "
                  "instructions in Markdown. Saving under an existing name replaces that skill."_s,
                  schema::object({{u"name"_s, schema::string(u"Lower-case words joined by hyphens, e.g. morning-routine"_s)},
                                  {u"description"_s, schema::string(u"One or two sentences: what it does and when to use it"_s)},
                                  {u"instructions"_s, schema::string(u"The steps, in Markdown, naming the tools to use (browser_navigate "
                                                                     "for a web page, never open_app for a browser)"_s)}},
                                 {u"name"_s, u"description"_s, u"instructions"_s})};
    save.privileged = true;
    save.risk = u"adds a skill Nexa will follow in later conversations"_s;
    registry.add(FunctionTool::sync(save, [skills](const QJsonObject &args) -> Result<ToolResult> {
        if (!skills) return Error{ErrorCode::NotSupported, u"Skills are not available."_s};
        const Result<QString> name = toolargs::requiredString(args, u"name"_s);
        if (!name) return name.error();
        const Result<QString> description = toolargs::requiredString(args, u"description"_s);
        if (!description) return description.error();
        const Result<QString> instructions = toolargs::requiredString(args, u"instructions"_s);
        if (!instructions) return instructions.error();
        const QString text = SkillParser::compose(SkillDocument{name.value(), description.value(), instructions.value()});
        const Result<Skill> saved = skills->write(text);
        if (!saved) return saved.error();
        return toolargs::text(u"Saved the skill \"%1\" in %2."_s.arg(saved.value().name(), saved.value().directory));
    }));
}

} // namespace nexa
