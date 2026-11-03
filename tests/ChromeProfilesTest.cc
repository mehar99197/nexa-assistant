// system/browser/ChromeProfiles: the Chrome-family browsers on the PC, their profiles by name, and
// where the Playwright extension is — read from a made-up config folder.

#include "system/browser/ChromeProfiles.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

void writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(content);
}

const QByteArray kLocalState = R"({"profile":{"last_used":"Profile 3","info_cache":{
    "Default":{"name":"Work"},"Profile 3":{"name":"Ahmad"},"Profile 12":{"name":"Zaigham"},
    "System Profile":{"name":"system"},"../escape":{"name":"bad"}}}})";

} // namespace

TEST_CASE("Local State gives the profiles by name, Chrome's own folder names only, and the one used last") {
    const ChromeProfiles::LocalState state = ChromeProfiles::parseLocalState(kLocalState);
    CHECK(state.lastUsed == u"Profile 3"_s);
    REQUIRE(state.profiles.size() == 3);
    CHECK(state.profiles.at(0) == BrowserProfile{u"Default"_s, u"Work"_s, false});
    CHECK(state.profiles.at(1).directory == u"Profile 3"_s);
    CHECK(state.profiles.at(2).directory == u"Profile 12"_s);         // by number, not as text
    CHECK(ChromeProfiles::parseLocalState("not json").profiles.isEmpty());
    CHECK(ChromeProfiles::parseLocalState(R"({"profile":{"last_used":"../x"}})").lastUsed.isEmpty());
}

TEST_CASE("Browsers are found by their program, with their profiles and where the extension is") {
    QTemporaryDir home;
    const QString chrome = home.filePath(u"google-chrome"_s);
    writeFile(chrome + u"/Local State"_s, kLocalState);
    // Profile 3: the extension's folder. Profile 12: only its preferences know it. Default: neither.
    QDir().mkpath(chrome + u"/Profile 3/Extensions/"_s + QString(ChromeProfiles::kExtensionId));
    writeFile(chrome + u"/Profile 12/Secure Preferences"_s,
              QJsonDocument(QJsonObject{{u"extensions"_s, QJsonObject{{u"settings"_s, QJsonObject{
                  {QString(ChromeProfiles::kExtensionId), QJsonObject{}}}}}}}).toJson());
    writeFile(chrome + u"/Default/Preferences"_s, R"({"extensions":{"settings":{"someotherextension":{}}}})");

    const auto find = [](const QString &program) {
        if (program == u"google-chrome-stable"_s) return u"/usr/bin/google-chrome-stable"_s;
        if (program == u"chromium"_s) return u"/usr/bin/chromium"_s;
        return QString();
    };
    const QList<BrowserInstall> installs = ChromeProfiles::detect(home.path(), find);
    REQUIRE(installs.size() == 2);
    const BrowserInstall &google = installs.first();
    CHECK(google.id == u"chrome"_s);
    CHECK(google.displayName == u"Google Chrome"_s);
    CHECK(google.executable == u"/usr/bin/google-chrome-stable"_s);
    CHECK(google.supportsExtension);
    CHECK(google.lastUsed == u"Profile 3"_s);
    CHECK_FALSE(google.profile(u"Default"_s)->extension);
    CHECK(google.profile(u"Profile 3"_s)->extension);
    CHECK(google.profile(u"Profile 12"_s)->extension);
    CHECK(installs.at(1).id == u"chromium"_s);
    CHECK_FALSE(installs.at(1).supportsExtension);
    CHECK(installs.at(1).profiles.isEmpty());          // never run: no Local State, still usable

    CHECK(ChromeProfiles::firefoxInstalled([](const QString &p) { return p == u"firefox-esr"_s ? u"/usr/bin/firefox-esr"_s : QString(); }));
    CHECK_FALSE(ChromeProfiles::firefoxInstalled([](const QString &) { return QString(); }));
}

TEST_CASE("The profile Playwright will use follows its own order: the chosen one, last used, Default, then by number") {
    BrowserInstall install{u"chrome"_s, u"Google Chrome"_s, u"/usr/bin/google-chrome"_s, u"/x"_s, u"Profile 3"_s,
                           {{u"Default"_s, u"Work"_s, true}, {u"Profile 3"_s, u"Ahmad"_s, false}, {u"Profile 7"_s, u"Z"_s, true}},
                           true};
    CHECK(ChromeProfiles::extensionProfile(install, u"Profile 7"_s) == u"Profile 7"_s);
    CHECK(ChromeProfiles::extensionProfile(install, u"Profile 3"_s) == u"Default"_s);     // chosen one lacks it
    CHECK(ChromeProfiles::extensionProfile(install, {}) == u"Default"_s);
    install.profiles[1].extension = true;
    CHECK(ChromeProfiles::extensionProfile(install, {}) == u"Profile 3"_s);              // last used first
    for (BrowserProfile &profile : install.profiles) profile.extension = false;
    CHECK(ChromeProfiles::extensionProfile(install, {}).isEmpty());
    CHECK(ChromeProfiles::webStoreUrl().endsWith(QString(ChromeProfiles::kExtensionId)));
}
