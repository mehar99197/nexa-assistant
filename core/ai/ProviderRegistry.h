#pragma once

#include "core/Result.h"
#include "core/ai/IAiProvider.h"
#include "core/ai/SignIn.h"
#include "core/config/Config.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>
#include <memory>

namespace nexa {

class HttpClient;
class OpenCodeServer;

struct ProviderPreset {
    QString id;
    QString kind;
    QString name;
    QString baseUrl;
    bool requiresKey{true};
    QString defaultModel;   // recommended starting model; empty = first non-auto model from /models
    QString sttModel;       // speech recognition model known to work (empty: none)
    QString ttsModel;       // speech output model known to work (empty: none)
    QString ttsVoice;
    QString modelsUrl;      // where the model list is, when not {baseUrl}/models
    // For the Add menu and the gallery. The catalogue's entries carry them; a hand-tuned preset takes them from
    // the catalogue entry of the same id (never its URLs).
    QString category;       // one of ProviderRegistry::presetCategories()
    QString website;
    QString hint;
    bool freeTier{false};
};

// One submenu of the Add menu: the presets (indices into the list given) of one category, in their order.
struct PresetGroup {
    QString title;
    QList<int> indices;
};

// Factory + registry: maps a provider kind to its constructor and knows the built-in presets.
class ProviderRegistry {
public:
    using Factory = std::function<std::unique_ptr<IAiProvider>(const ProviderConfig &, HttpClient *)>;

    explicit ProviderRegistry(HttpClient *http);    // registers the built-in kinds
    ~ProviderRegistry();

    void registerKind(const QString &kind, Factory factory);
    bool supports(const QString &kind) const { return factories_.contains(kind); }
    Result<std::unique_ptr<IAiProvider>> create(const ProviderConfig &config) const;
    // The ProviderChain Nexa asks: the first choice (config.active()), then every other provider with
    // `fallback`, in list order. One that cannot be created is left out; none at all is an error.
    Result<std::unique_ptr<IAiProvider>> createChain(const Config &config) const;

    // The programs first (opencode, agy, Codex), then the hand-tuned entries, then every catalogue entry whose id is
    // not already there; the hand-tuned one wins on a clash and takes the catalogue's category, website, hint and
    // free-tier mark. Built once.
    static const QList<ProviderPreset> &presets();
    // "Programs", then the catalogue's categories, in display order.
    static QStringList presetCategories();
    QList<ProviderPreset> availablePresets() const; // presets whose kind is registered
    static ProviderConfig fromPreset(const ProviderPreset &preset);
    // What a first start offers: opencode, then agy, each only if `installed` finds the program.
    static QList<ProviderConfig> defaultProviders(const std::function<bool(const QString &program)> &installed);
    // Brings a config written by an older Nexa up to Config::kVersion: the speech models of v2, and
    // for v3 the installed programs into a list left empty (once — an emptied list stays empty).
    static void upgrade(Config &config, const std::function<bool(const QString &program)> &installed);
    // Kinds that talk to a URL, as opposed to a program Nexa runs.
    static bool isHttpKind(const QString &kind);
    // The programs Nexa runs on the user's own sign-in: agy, opencode and Codex.
    static bool isProgramKind(const QString &kind);
    // Whether a program said, on a reply or a model list, that nobody is signed in — what Settings → Providers →
    // Sign in mends. Not any Unauthorized: a model opencode refuses (no plan, no licence) and a server that refused
    // its password say Unauthorized too, and a sign-in mends neither, so their own words must stay in the chat.
    static bool meansNotSignedIn(const Error &error);
    // The presets by category for the Add menu: one group per category of `categories` that has a preset, in that
    // order, then "Other" for a preset whose category is none of them (or empty). Empty groups are not made.
    static QList<PresetGroup> groupPresets(const QList<ProviderPreset> &presets, const QStringList &categories);
    // Whether a request needs a model named: an API does; the programs answer with their own default.
    static bool needsModel(const ProviderConfig &config);

    // The sign-in of a program provider (agy, opencode, Codex); nullptr for an API. The program is found as its
    // provider finds it, and opencode's shares the server the provider uses.
    std::unique_ptr<ISignIn> createSignIn(const ProviderConfig &config) const;

    // The opencode server a provider of this config talks to: one per program and URL, kept for the
    // registry's life, so a provider rebuilt after Settings finds its server already running.
    std::shared_ptr<OpenCodeServer> openCodeServer(const ProviderConfig &config) const;

private:
    HttpClient *http_;
    QHash<QString, Factory> factories_;
    mutable QHash<QString, std::shared_ptr<OpenCodeServer>> openCodeServers_;
};

} // namespace nexa
