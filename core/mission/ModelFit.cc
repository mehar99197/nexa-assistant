#include "core/mission/ModelFit.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

bool sees(const ProviderFacts &provider) {
    return provider.vision || !provider.imageModels.isEmpty();
}

// A model of `provider` that sees: its default when that one does (or when it cannot tell), else its first that does.
QString seeingModel(const ProviderFacts &provider) {
    if (provider.imageModels.isEmpty() || provider.imageModels.contains(provider.defaultModel)) return provider.defaultModel;
    return provider.imageModels.first();
}

} // namespace

ModelChoice ModelFit::resolve(AgentRole role, const MissionConfig &mission, const QList<ProviderFacts> &providers) {
    const auto chosen = mission.roleModels.constFind(roleKey(role));
    if (chosen != mission.roleModels.cend() && !chosen->isAuto()) {
        const bool present = std::any_of(providers.cbegin(), providers.cend(),
                                         [&chosen](const ProviderFacts &p) { return p.id == chosen->provider; });
        if (present) return *chosen;
    }
    return defaultFor(role, providers);
}

ModelChoice ModelFit::defaultFor(AgentRole role, const QList<ProviderFacts> &providers) {
    switch (role) {
    case AgentRole::Code:
        for (const QString &kind : {u"codex"_s, u"opencode"_s, u"agy"_s}) {
            for (const ProviderFacts &provider : providers) {
                if (provider.kind == kind) return ModelChoice{provider.id, provider.defaultModel};
            }
        }
        return {};
    case AgentRole::Screen:
    case AgentRole::Phone: {
        const auto first = std::find_if(providers.cbegin(), providers.cend(),
                                        [](const ProviderFacts &p) { return p.isFirstChoice; });
        if (first != providers.cend() && sees(*first) && seeingModel(*first) == first->defaultModel) return {};
        for (const ProviderFacts &provider : providers) {
            if (sees(provider)) return ModelChoice{provider.id, seeingModel(provider)};
        }
        return {};
    }
    case AgentRole::Web:
    case AgentRole::General:
        return {};
    }
    return {};
}

bool ModelFit::needsOwnInstance(const QString &kind) {
    return kind == u"agy"_s || kind == u"opencode"_s || kind == u"codex"_s;
}

} // namespace nexa
