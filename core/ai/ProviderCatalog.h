#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <cstddef>

namespace nexa {

// One provider Nexa can be pointed at with a key (or none), as the Add menu and the gallery list it. `kind` is one
// of the kKind* strings of core/config/Config.h (openai-compatible, anthropic or gemini); `baseUrl` is what that
// provider appends its own path to; an empty `modelsUrl` means the kind's default ({baseUrl}/models).
struct CatalogEntry {
    QString id;                 // lower-case [a-z0-9-], unique
    QString name;               // shown in the UI
    QString kind;
    QString baseUrl;
    QString modelsUrl;          // where the model list is, when not the default
    bool requiresKey{true};     // false for a local server and for a service that works without a key
    QString category;           // one of ProviderCatalog::categories()
    QString website;            // https
    QString hint;               // one short sentence, possibly empty
    bool freeTier{false};       // the service has a free tier or needs no key
};

namespace catalogdata {

// One row of the generated table in ProviderCatalogData.cc: plain C strings in a constant array, so nothing is
// built at load. Only ProviderCatalog reads it.
struct Row {
    const char *id;
    const char *name;
    const char *kind;
    const char *baseUrl;
    const char *modelsUrl;
    bool requiresKey;
    const char *category;
    const char *website;
    const char *hint;
    bool freeTier;
};

extern const Row kRows[];
extern const std::size_t kRowCount;

} // namespace catalogdata

// Registry (read-only, over generated constant data): the catalogue of key-based and local providers taken from
// OmniRoute's registry by scripts/generate-provider-catalog.py. The data is converted to QStrings once, on first
// use. It carries no model lists and no speech models: the live /models is the source of the first, and the
// hand-tuned presets of ProviderRegistry are the only source of the second.
class ProviderCatalog {
public:
    // Every entry, sorted by category (in categories() order), then by name.
    static const QList<CatalogEntry> &entries();
    // The categories in display order. "Programs" (agy, opencode, Codex) is not one of them: it is not data.
    static QStringList categories();
    // The entry with this id, or nullptr.
    static const CatalogEntry *find(const QString &id);
};

} // namespace nexa
