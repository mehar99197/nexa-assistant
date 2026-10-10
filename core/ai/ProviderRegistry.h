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
};

// Factory + registry: maps a provider kind to its constructor and knows the built-in presets.
class ProviderRegistry {
public:
    using Factory = std::function<std::unique_ptr<IAiProvider>(const ProviderConfig &, HttpClient *)>;

    explicit ProviderRegistry(HttpClient *http);    // registers the built-in kinds

    void registerKind(const QString &kind, Factory factory);
    bool supports(const QString &kind) const { return factories_.contains(kind); }
    Result<std::unique_ptr<IAiProvider>> create(const ProviderConfig &config) const;

    static QList<ProviderPreset> presets();         // OmniRoute first: it is the default
    QList<ProviderPreset> availablePresets() const; // presets whose kind is registered
    static ProviderConfig fromPreset(const ProviderPreset &preset);
    static ProviderConfig defaultProvider();
    // Brings a config written by an older Nexa up to Config::kVersion (fills in what the presets
    // gained since: the speech models of v2).
    static void upgrade(Config &config);

private:
    HttpClient *http_;
    QHash<QString, Factory> factories_;
};

} // namespace nexa
