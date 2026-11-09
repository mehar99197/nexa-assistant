#pragma once

#include "core/config/Config.h"
#include "core/mission/AgentRole.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace nexa {

// What ModelFit needs to know of a configured provider; the app fills it from the config and the live providers.
struct ProviderFacts {
    QString id;
    QString kind;
    QString name;
    QString defaultModel;
    bool vision{false};         // the provider sees images (IAiProvider::Capabilities)
    QStringList imageModels;    // or these of its models do (opencode knows per model)
    bool isFirstChoice{false};
};

// Which provider and model an agent of each role runs on (Strategy table, pure). Settings → Mission wins while its
// provider is still configured; otherwise: code goes to a coding program (Codex, opencode, agy, in that order), the
// screen and the phone to a provider that can see the pictures they work from, and the web and everything else stay
// Auto — the first choice with its fallbacks.
class ModelFit {
public:
    static ModelChoice resolve(AgentRole role, const MissionConfig &mission, const QList<ProviderFacts> &providers);
    static ModelChoice defaultFor(AgentRole role, const QList<ProviderFacts> &providers);
    // A program (agy, opencode, Codex) answers one conversation at a time per instance: a task on one needs its own.
    static bool needsOwnInstance(const QString &kind);
};

} // namespace nexa
