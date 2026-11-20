#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>

namespace nexa {

struct BrowserProfile {
    QString directory;          // "Default", "Profile 1": the folder, and what Playwright is told
    QString name;               // "Ahmad": what the user calls it
    bool extension{false};      // the Playwright extension is installed in it

    bool operator==(const BrowserProfile &) const = default;
};

// A Chrome-family browser on this PC.
struct BrowserInstall {
    QString id;                 // chrome | msedge | brave | chromium
    QString displayName;        // "Google Chrome"
    QString executable;         // absolute path
    QString userDataDir;        // ~/.config/google-chrome
    QString lastUsed;           // the profile directory it opened last
    QList<BrowserProfile> profiles;
    bool supportsExtension{false};  // Playwright's extension mode: Chrome and Edge only

    const BrowserProfile *profile(const QString &directory) const;
};

// Which Chrome-family browsers are installed, their profiles by name, and in which of them the
// Playwright extension is — the extension Nexa needs to work in the user's own browser, signed in
// as them. Reads the browsers' own files (Local State, Preferences); never writes them. Firefox has
// no such extension. Pure but for the file reads, which take the folders as arguments. (Utility)
class ChromeProfiles {
public:
    static constexpr QLatin1StringView kExtensionId{"mmlmfjhmonkocbjadbfplnigmagldckm"};
    using FindExecutable = std::function<QString(const QString &program)>;

    // The browsers whose program is found, with their profiles from <configHome>/<their folder>.
    static QList<BrowserInstall> detect(const QString &configHome, const FindExecutable &findExecutable);
    static QList<BrowserInstall> detect();      // this user's

    struct LocalState {
        QString lastUsed;
        QList<BrowserProfile> profiles;         // Default first, then Profile N by number
    };
    // Chrome's "Local State" file: profile.info_cache and profile.last_used. Only the folder names
    // Chrome itself gives (Default, Profile N) are kept — they become paths.
    static LocalState parseLocalState(const QByteArray &json);
    // In <userDataDir>/<profile>: an Extensions/<id> folder, or the id among the extensions its
    // Preferences or Secure Preferences know (Playwright checks the same two).
    static bool extensionInstalled(const QString &profileDir);
    // The profile Playwright will use: `wanted` if the extension is there, otherwise the first that
    // has it in Playwright's own order — the one used last, Default, then Profile N. Empty: none.
    static QString extensionProfile(const BrowserInstall &install, const QString &wanted);
    // Where the user adds the extension, in the browser's own store page.
    static QString webStoreUrl();
    static bool firefoxInstalled(const FindExecutable &findExecutable);
    static bool firefoxInstalled();
};

} // namespace nexa
