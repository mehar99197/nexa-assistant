#pragma once

#include "core/Result.h"
#include "core/ai/IAiProvider.h"
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

    static QList<ProviderPreset> presets();         // the owner's programs first: opencode, agy
    QList<ProviderPreset> availablePresets() const; // presets whose kind is registered
    static ProviderConfig fromPreset(const ProviderPreset &preset);
    // What a first start offers: opencode, then agy, each only if `installed` finds the program.
    static QList<ProviderConfig> defaultProviders(const std::function<bool(const QString &program)> &installed);
    // Brings a config written by an older Nexa up to Config::kVersion: the speech models of v2, and
    // for v3 the installed programs into a list left empty (once — an emptied list stays empty).
    static void upgrade(Config &config, const std::function<bool(const QString &program)> &installed);
    // Kinds that talk to a URL, as opposed to a program Nexa runs.
    static bool isHttpKind(const QString &kind);

    // The opencode server a provider of this config talks to: one per program and URL, kept for the
    // registry's life, so a provider rebuilt after Settings finds its server already running.
    std::shared_ptr<OpenCodeServer> openCodeServer(const ProviderConfig &config);

private:
    HttpClient *http_;
    QHash<QString, Factory> factories_;
    QHash<QString, std::shared_ptr<OpenCodeServer>> openCodeServers_;
};

} // namespace nexa
