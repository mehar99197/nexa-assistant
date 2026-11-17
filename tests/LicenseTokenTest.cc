#include "core/account/Entitlements.h"
#include "core/account/KeyIntegrity.h"
#include "core/account/LicenseToken.h"

#include "LicenseTestUtil.h"
#include "QtStringMakers.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("LicenseToken: verifies valid assistant token and rejects forgeries", "[account][license]") {
    REQUIRE(licensetoken::usingDevelopmentKey());
    REQUIRE(licensetoken::publicKeyIntact());

    const qint64 now = 1'760'000'000;
    const QString device(64, u'a');
    const QJsonObject features{
        {u"nexaAi"_s, true},
        {u"nexaAiMonthlyRequests"_s, 1000},
        {u"ownVoice"_s, true},
        {u"awayFromHome"_s, true},
        {u"seats"_s, 5},
    };
    const QString token = test::signAssistantJwt(u"team"_s, device, now, now + 86400, features);

    licensetoken::Claims claims;
    QString err;
    REQUIRE(licensetoken::verify(token, &claims, &err));
    CHECK(err.isEmpty());
    CHECK(claims.plan == u"team"_s);
    CHECK(claims.product == u"assistant"_s);
    CHECK(claims.device == device);
    CHECK(claims.licenseKey == u"NDM-AAAA-BBBB-CCCC"_s);

    const Entitlements ent = Entitlements::fromClaims(claims.plan, claims.features);
    CHECK(ent.nexaAi);
    CHECK(ent.nexaAiMonthlyRequests == 1000);
    CHECK(ent.ownVoice);
    CHECK(ent.awayFromHome);
    CHECK(ent.seats == 5);

    SECTION("rejects wrong product claim (NDM token cannot activate Assistant)") {
        const QString ndmToken = test::signAssistantJwt(u"pro"_s, device, now, now + 86400, features, u"ndm"_s);
        REQUIRE_FALSE(licensetoken::verify(ndmToken, &claims, &err));
        CHECK(err == u"wrong product"_s);
    }

    SECTION("rejects alg=none or HMAC downgrades") {
        const QString forged = test::signJwt(
            QJsonObject{{u"alg"_s, u"none"_s}},
            QJsonObject{{u"typ"_s, u"license"_s}, {u"product"_s, u"assistant"_s}, {u"plan"_s, u"pro"_s}, {u"exp"_s, double(now + 100)}});
        REQUIRE_FALSE(licensetoken::verify(forged, &claims, &err));
        CHECK(err == u"unexpected algorithm"_s);
    }

    SECTION("rejects token signed with wrong key") {
        const QString wrongKey = test::signAssistantJwt(u"pro"_s, device, now, now + 86400, features, u"assistant"_s,
                                                        test::otherSeed());
        REQUIRE_FALSE(licensetoken::verify(wrongKey, &claims, &err));
        CHECK(err == u"signature does not verify"_s);
    }

    SECTION("rejects payload tampering after signing") {
        QStringList parts = token.split(u'.');
        REQUIRE(parts.size() == 3);
        const QJsonObject hacked{{u"typ"_s, u"license"_s}, {u"product"_s, u"assistant"_s},
                                 {u"plan"_s, u"team"_s},    {u"device"_s, device},
                                 {u"exp"_s, double(now + 9999999)}};
        parts[1] = QString::fromLatin1(test::base64Url(QJsonDocument(hacked).toJson(QJsonDocument::Compact)));
        REQUIRE_FALSE(licensetoken::verify(parts.join(u'.'), &claims, &err));
    }

    SECTION("rejects missing exp or unknown plan") {
        const QString unknownPlan = test::signAssistantJwt(u"enterprise"_s, device, now, now + 86400);
        REQUIRE_FALSE(licensetoken::verify(unknownPlan, &claims, &err));
        CHECK(err == u"unknown plan"_s);
    }
}

TEST_CASE("LicenseToken: offline grace enforces 7-day bound, device binding, and assistant product", "[account][license]") {
    const qint64 nowSec = 1'760'000'000;
    const QDateTime now = QDateTime::fromSecsSinceEpoch(nowSec, QTimeZone::utc());
    const QString device(64, u'b');

    licensetoken::Claims claims;
    claims.plan = u"pro"_s;
    claims.product = u"assistant"_s;
    claims.device = device;
    claims.issuedAt = now.addDays(-3);
    claims.expiresAt = now.addDays(30);

    CHECK(licensetoken::offlineGraceAllows(claims, device, now, 7));
    CHECK_FALSE(licensetoken::offlineGraceAllows(claims, QString(64, u'c'), now, 7));

    claims.issuedAt = now.addDays(-8);
    CHECK_FALSE(licensetoken::offlineGraceAllows(claims, device, now, 7));

    // Clock rollback (> 5 min future issuedAt) denied
    claims.issuedAt = now.addSecs(600);
    CHECK_FALSE(licensetoken::offlineGraceAllows(claims, device, now, 7));

    // Free plan never gains grace
    claims.issuedAt = now.addDays(-1);
    claims.plan = u"free"_s;
    CHECK_FALSE(licensetoken::offlineGraceAllows(claims, device, now, 7));

    // Non-assistant product never gains grace
    claims.plan = u"pro"_s;
    claims.product = u"ndm"_s;
    CHECK_FALSE(licensetoken::offlineGraceAllows(claims, device, now, 7));
}

TEST_CASE("Entitlements: plan defaults are fail-closed for free and 1000 monthly requests for paid", "[account][license]") {
    const Entitlements freeEnt = Entitlements::forPlan(u"free"_s);
    CHECK_FALSE(freeEnt.nexaAi);
    CHECK(freeEnt.nexaAiMonthlyRequests == 0);
    CHECK_FALSE(freeEnt.ownVoice);
    CHECK_FALSE(freeEnt.awayFromHome);
    CHECK(freeEnt.seats == 1);

    const Entitlements proEnt = Entitlements::forPlan(u"pro"_s);
    CHECK(proEnt.nexaAi);
    CHECK(proEnt.nexaAiMonthlyRequests == 1000);
    CHECK(proEnt.ownVoice);
    CHECK(proEnt.awayFromHome);
    CHECK(proEnt.seats == 1);

    const Entitlements teamEnt = Entitlements::forPlan(u"team"_s);
    CHECK(teamEnt.nexaAi);
    CHECK(teamEnt.nexaAiMonthlyRequests == 1000);
    CHECK(teamEnt.seats == 5);
}
