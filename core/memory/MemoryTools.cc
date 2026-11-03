#include "core/memory/MemoryTools.h"

#include "core/config/Config.h"
#include "core/memory/MemoryStore.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolArguments.h"
#include "core/tools/ToolRegistry.h"

#include <QPointer>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QStringList &toolNames() {
    static const QStringList names{u"remember"_s, u"forget"_s, u"search_memory"_s};
    return names;
}

QString listed(const QList<MemoryEntry> &entries) {
    QStringList lines;
    for (const MemoryEntry &entry : entries) {
        lines.append(u"- [%1] %2 (%3, %4)"_s.arg(entry.id, entry.text, entry.origin,
                                                   entry.updated.date().toString(Qt::ISODate)));
    }
    return lines.join(u'\n');
}

} // namespace

void syncMemoryTools(ToolRegistry &registry, MemoryStore *store, const MemoryConfig &config) {
    if (store) store->setLearning(config.learn);
    if (!config.enabled || !store) {
        for (const QString &name : toolNames()) registry.remove(name);
        return;
    }
    const QPointer<MemoryStore> memory(store);

    ToolSpec remember{u"remember"_s,
                      u"Save a lasting fact about the user for later conversations — one short fact per call, e.g. "
                      "\"Their office is in Multan\". origin: \"asked\" when the user told you to remember it, "
                      "\"learned\" when you noticed it yourself. Not for what the system prompt's memory already lists. Never "
                      "passwords, PINs, keys or card numbers."_s,
                      schema::object({{u"text"_s, schema::string(u"The fact, in one sentence"_s)},
                                      {u"origin"_s, schema::choice(u"Who wanted it saved"_s, {u"asked"_s, u"learned"_s})}},
                                     {u"text"_s})};
    registry.add(FunctionTool::sync(remember, [memory](const QJsonObject &args) -> Result<ToolResult> {
        if (!memory) return Error{ErrorCode::NotSupported, u"Memory is not available."_s};
        const Result<QString> text = toolargs::requiredString(args, u"text"_s);
        if (!text) return text.error();
        const qsizetype before = memory->entries().size();
        const Result<MemoryEntry> saved = memory->remember(text.value(), args.value(u"origin"_s).toString(u"asked"_s));
        if (!saved) return saved.error();
        const bool known = memory->entries().size() == before;
        return toolargs::text((known ? u"Already remembered: %1"_s : u"Remembered: %1"_s).arg(saved.value().text));
    }));

    ToolSpec forget{u"forget"_s,
                    u"Remove a saved memory about the user: its id (from search_memory) or words from it."_s,
                    schema::object({{u"query"_s, schema::string(u"The memory's id, or words that pick it out"_s)}},
                                   {u"query"_s})};
    registry.add(FunctionTool::sync(forget, [memory](const QJsonObject &args) -> Result<ToolResult> {
        if (!memory) return Error{ErrorCode::NotSupported, u"Memory is not available."_s};
        const Result<QString> query = toolargs::requiredString(args, u"query"_s);
        if (!query) return query.error();
        const Result<QList<MemoryEntry>> removed = memory->forget(query.value());
        if (!removed) return removed.error();
        return toolargs::text(u"Forgot:\n"_s + listed(removed.value()));
    }));

    ToolSpec search{u"search_memory"_s,
                    u"Look up what is saved about the user; an empty query lists the newest memories."_s,
                    schema::object({{u"query"_s, schema::string(u"Words to look for; empty for the newest"_s)}})};
    search.mutating = false;
    registry.add(FunctionTool::sync(search, [memory](const QJsonObject &args) -> Result<ToolResult> {
        if (!memory) return Error{ErrorCode::NotSupported, u"Memory is not available."_s};
        const QString query = args.value(u"query"_s).toString().trimmed();
        const QList<MemoryEntry> found = memory->search(query, 20);
        if (found.isEmpty()) {
            return toolargs::text(memory->entries().isEmpty() ? u"Nothing is saved in memory yet."_s
                                                              : u"Nothing in memory matches \"%1\"."_s.arg(query));
        }
        return toolargs::text(listed(found));
    }));
}

} // namespace nexa
