#include "core/account/Entitlements.h"

#include <QJsonValue>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

Entitlements Entitlements::forPlan(const QString &plan) {
    const bool paid = plan == u"pro"_s || plan == u"team"_s;
    if (!paid) return Entitlements{};
    return Entitlements{
        .nexaAi = true,
        .nexaAiMonthlyRequests = 1'000,
        .ownVoice = true,
        .awayFromHome = true,
        .seats = plan == u"team"_s ? 5 : 1,
    };
}

Entitlements Entitlements::fromClaims(const QString &plan, const QJsonObject &features) {
    Entitlements out = forPlan(plan);
    if (features.contains(u"nexaAi"_s)) out.nexaAi = features.value(u"nexaAi"_s).toBool(out.nexaAi);
    if (features.contains(u"nexaAiMonthlyRequests"_s)) {
        out.nexaAiMonthlyRequests = std::max(0, features.value(u"nexaAiMonthlyRequests"_s).toInt(out.nexaAiMonthlyRequests));
    }
    if (features.contains(u"ownVoice"_s)) out.ownVoice = features.value(u"ownVoice"_s).toBool(out.ownVoice);
    if (features.contains(u"awayFromHome"_s)) {
        out.awayFromHome = features.value(u"awayFromHome"_s).toBool(out.awayFromHome);
    }
    if (features.contains(u"seats"_s)) out.seats = std::max(1, features.value(u"seats"_s).toInt(out.seats));
    return out;
}

} // namespace nexa
