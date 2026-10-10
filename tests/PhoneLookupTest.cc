// system/android/PhoneLookup: which app "youtube" means and which contact "mom" means. A wrong
// answer opens the wrong app or calls the wrong person, so "no answer" is preferred to a guess.

#include "system/android/PhoneLookup.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::phonelookup;
using namespace Qt::StringLiterals;

namespace {

const QStringList kApps{
    u"com.android.chrome/com.google.android.apps.chrome.Main"_s,
    u"com.google.android.youtube/.app.honeycomb.Shell$HomeActivity"_s,
    u"com.whatsapp/.Main"_s,
    u"com.whatsapp.w4b/.Main"_s,
    u"com.instagram.android/.activity.MainTabActivity"_s,
    u"com.sec.android.app.popupcalculator/.Calculator"_s,
    u"com.spotify.music/.MainActivity"_s,
    u"org.telegram.messenger/.DefaultIcon"_s,
    u"com.samsung.android.messaging/.ui.view.main.WithActivity"_s,
};

} // namespace

TEST_CASE("Known apps by name, whatever their package is called") {
    const auto youtube = matchApp(u"YouTube"_s, kApps);
    REQUIRE(youtube);
    CHECK(youtube->component == u"com.google.android.youtube/.app.honeycomb.Shell$HomeActivity"_s);
    CHECK(youtube->label == u"YouTube"_s);

    // WhatsApp, not WhatsApp Business.
    const auto whatsapp = matchApp(u"the WhatsApp app"_s, kApps);
    REQUIRE(whatsapp);
    CHECK(whatsapp->component == u"com.whatsapp/.Main"_s);
    const auto business = matchApp(u"whatsapp business"_s, kApps);
    REQUIRE(business);
    CHECK(business->component == u"com.whatsapp.w4b/.Main"_s);
}

TEST_CASE("Generic names become intents, so any vendor's app answers") {
    const auto camera = matchApp(u"camera"_s, kApps);
    REQUIRE(camera);
    CHECK(camera->component.isEmpty());
    CHECK(camera->intentArguments == u"-a android.media.action.STILL_IMAGE_CAMERA"_s);

    // Samsung's own messaging app is not Google's, and still opens.
    const auto messages = matchApp(u"messages"_s, kApps);
    REQUIRE(messages);
    CHECK(messages->intentArguments.endsWith(u"APP_MESSAGING"_s));
}

TEST_CASE("A brand that is not installed is not swapped for another vendor's app") {
    // Gmail is not on this phone: no quiet switch to some other mail app.
    CHECK_FALSE(matchApp(u"gmail"_s, kApps));
}

TEST_CASE("Unknown apps by the words of their package names") {
    const auto calculator = matchApp(u"popup calculator"_s, kApps);
    REQUIRE(calculator);
    CHECK(calculator->component.startsWith(u"com.sec.android.app.popupcalculator/"_s));

    const auto insta = matchApp(u"insta"_s, kApps);       // a prefix of a word
    REQUIRE(insta);
    CHECK(insta->component.startsWith(u"com.instagram.android/"_s));

    const auto telegram = matchApp(u"telegram"_s, kApps);
    REQUIRE(telegram);
    CHECK(telegram->component.startsWith(u"org.telegram.messenger/"_s));
}

TEST_CASE("Nothing close means no app, and a list of near misses") {
    CHECK_FALSE(matchApp(u"banking"_s, kApps));
    CHECK_FALSE(matchApp(u"an"_s, kApps));               // too short to match inside a word
    CHECK_FALSE(matchApp(QString(), kApps));
    CHECK(closestApps(u"spot"_s, kApps).value(0) == u"com.spotify.music"_s);
    CHECK(labelForPackage(u"com.whatsapp"_s) == u"WhatsApp"_s);
    CHECK(labelForPackage(u"com.example.unknown"_s) == u"com.example.unknown"_s);
}

TEST_CASE("Contacts: exact, then first name, then part of the name") {
    const QList<Contact> contacts{
        {u"Mom"_s, u"+923001111111"_s},
        {u"Ali Khan"_s, u"+923002222222"_s},
        {u"Ali Raza"_s, u"+923003333333"_s},
        {u"Sara Ahmed"_s, u"+923004444444"_s},
        {u"Sara Ahmed"_s, u"+923005555555"_s},      // one person, two numbers
        {u"Office"_s, QString()},                     // no number: never chosen
    };
    CHECK(matchContact(u"mom"_s, contacts)->number == u"+923001111111"_s);
    CHECK(matchContact(u"Ali Khan"_s, contacts)->number == u"+923002222222"_s);
    CHECK(matchContact(u"sara"_s, contacts)->number == u"+923004444444"_s);
    CHECK(matchContact(u"ahmed"_s, contacts)->name == u"Sara Ahmed"_s);

    // Two different people answer to "Ali": calling either would be a guess.
    CHECK_FALSE(matchContact(u"ali"_s, contacts));
    CHECK_FALSE(matchContact(u"office"_s, contacts));
    CHECK_FALSE(matchContact(u"dad"_s, contacts));
}
