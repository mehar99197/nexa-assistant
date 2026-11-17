#pragma once

#include <QJsonObject>
#include <QString>

namespace nexa {

// Verified server-signed entitlements for Nexa Assistant (`entitlementsFor(plan, "assistant")`).
// Defaults are strictly Free so offline/unauthenticated/malformed states fail closed.
struct Entitlements {
    bool nexaAi{false};
    int nexaAiMonthlyRequests{0};
    bool ownVoice{false};
    bool awayFromHome{false};
    int seats{1};

    [[nodiscard]] static Entitlements forPlan(const QString &plan);
    [[nodiscard]] static Entitlements fromClaims(const QString &plan, const QJsonObject &features);

    bool operator==(const Entitlements &) const = default;
};

} // namespace nexa
