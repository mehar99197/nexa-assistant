#include "system/android/PhoneLookup.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa::phonelookup {

namespace {

constexpr double kMinAppScore = 0.8;

const QString kMainWith = u"-a android.intent.action.MAIN -c android.intent.category."_s;

struct Alias {
    QStringList names;
    QString package;            // preferred when the phone has it
    QString intentArguments;    // otherwise this, which any phone can resolve
    QString label;
};

// Only names whose package is certain. A wrong alias opens the wrong app, which is worse than
// letting the model look through the list. Brand names carry only a package: "open Gmail" on a phone
// without Gmail must not quietly open some other mail app. Generic names ("messages", "camera")
// carry an intent too, because every vendor ships its own app for those.
const QList<Alias> &aliases() {
    static const QList<Alias> list{
        {{u"whatsapp"_s}, u"com.whatsapp"_s, {}, u"WhatsApp"_s},
        {{u"whatsapp business"_s}, u"com.whatsapp.w4b"_s, {}, u"WhatsApp Business"_s},
        {{u"youtube"_s}, u"com.google.android.youtube"_s, {}, u"YouTube"_s},
        {{u"youtube music"_s}, u"com.google.android.apps.youtube.music"_s, {}, u"YouTube Music"_s},
        {{u"gmail"_s}, u"com.google.android.gm"_s, {}, u"Gmail"_s},
        {{u"email"_s, u"mail"_s}, {}, kMainWith + u"APP_EMAIL"_s, u"Email"_s},
        {{u"chrome"_s, u"google chrome"_s}, u"com.android.chrome"_s, {}, u"Chrome"_s},
        {{u"browser"_s, u"internet"_s, u"web browser"_s}, {}, kMainWith + u"APP_BROWSER"_s, u"the browser"_s},
        {{u"maps"_s, u"google maps"_s}, u"com.google.android.apps.maps"_s, kMainWith + u"APP_MAPS"_s, u"Maps"_s},
        {{u"play store"_s, u"google play"_s, u"app store"_s}, u"com.android.vending"_s, kMainWith + u"APP_MARKET"_s,
         u"Play Store"_s},
        {{u"camera"_s}, {}, u"-a android.media.action.STILL_IMAGE_CAMERA"_s, u"the camera"_s},
        {{u"video camera"_s}, {}, u"-a android.media.action.VIDEO_CAMERA"_s, u"the video camera"_s},
        {{u"settings"_s}, {}, u"-a android.settings.SETTINGS"_s, u"Settings"_s},
        {{u"wifi settings"_s, u"wi fi settings"_s}, {}, u"-a android.settings.WIFI_SETTINGS"_s, u"Wi-Fi settings"_s},
        {{u"bluetooth settings"_s}, {}, u"-a android.settings.BLUETOOTH_SETTINGS"_s, u"Bluetooth settings"_s},
        {{u"phone"_s, u"dialer"_s, u"dial pad"_s, u"keypad"_s}, {}, u"-a android.intent.action.DIAL"_s, u"the dialer"_s},
        {{u"messages"_s, u"messaging"_s, u"sms"_s, u"texts"_s}, u"com.google.android.apps.messaging"_s,
         kMainWith + u"APP_MESSAGING"_s, u"Messages"_s},
        {{u"contacts"_s, u"people"_s}, {}, kMainWith + u"APP_CONTACTS"_s, u"Contacts"_s},
        {{u"calendar"_s}, u"com.google.android.calendar"_s, kMainWith + u"APP_CALENDAR"_s, u"Calendar"_s},
        {{u"gallery"_s, u"pictures"_s, u"photos"_s}, {}, kMainWith + u"APP_GALLERY"_s, u"the gallery"_s},
        {{u"google photos"_s}, u"com.google.android.apps.photos"_s, {}, u"Google Photos"_s},
        {{u"music"_s}, {}, kMainWith + u"APP_MUSIC"_s, u"the music app"_s},
        {{u"calculator"_s}, {}, kMainWith + u"APP_CALCULATOR"_s, u"the calculator"_s},
        {{u"clock"_s, u"alarm"_s, u"alarms"_s}, {}, u"-a android.intent.action.SHOW_ALARMS"_s, u"the clock"_s},
        {{u"files"_s, u"file manager"_s, u"my files"_s}, {}, kMainWith + u"APP_FILES"_s, u"Files"_s},
        {{u"messenger"_s, u"facebook messenger"_s}, u"com.facebook.orca"_s, {}, u"Messenger"_s},
        {{u"facebook"_s}, u"com.facebook.katana"_s, {}, u"Facebook"_s},
        {{u"instagram"_s}, u"com.instagram.android"_s, {}, u"Instagram"_s},
        {{u"tiktok"_s, u"tik tok"_s}, u"com.zhiliaoapp.musically"_s, {}, u"TikTok"_s},
        {{u"telegram"_s}, u"org.telegram.messenger"_s, {}, u"Telegram"_s},
        {{u"snapchat"_s}, u"com.snapchat.android"_s, {}, u"Snapchat"_s},
        {{u"spotify"_s}, u"com.spotify.music"_s, {}, u"Spotify"_s},
        {{u"netflix"_s}, u"com.netflix.mediaclient"_s, {}, u"Netflix"_s},
        {{u"twitter"_s, u"x"_s}, u"com.twitter.android"_s, {}, u"X"_s},
        {{u"linkedin"_s}, u"com.linkedin.android"_s, {}, u"LinkedIn"_s},
        {{u"zoom"_s}, u"us.zoom.videomeetings"_s, {}, u"Zoom"_s},
        {{u"teams"_s, u"microsoft teams"_s}, u"com.microsoft.teams"_s, {}, u"Teams"_s},
        {{u"outlook"_s}, u"com.microsoft.office.outlook"_s, {}, u"Outlook"_s},
        {{u"drive"_s, u"google drive"_s}, u"com.google.android.apps.docs"_s, {}, u"Drive"_s},
        {{u"translate"_s, u"google translate"_s}, u"com.google.android.apps.translate"_s, {}, u"Translate"_s},
        {{u"keep"_s, u"google keep"_s}, u"com.google.android.keep"_s, {}, u"Keep"_s},
        {{u"google"_s}, u"com.google.android.googlequicksearchbox"_s, {}, u"Google"_s},
    };
    return list;
}

// Words that appear in half of all package names and say nothing about the app.
const QSet<QString> &genericWords() {
    static const QSet<QString> words{u"com"_s,    u"org"_s,     u"net"_s,  u"io"_s,     u"co"_s,
                                     u"android"_s, u"google"_s, u"app"_s, u"apps"_s,   u"mobile"_s,
                                     u"client"_s, u"main"_s,    u"www"_s, u"launcher"_s, u"activity"_s};
    return words;
}

// "Open the YouTube app!" → "youtube".
QString normalizedName(const QString &spoken) {
    QString name = spoken.toLower();
    name.replace(QRegularExpression(u"[^\\p{L}\\p{N}]+"_s), u" "_s);
    name = name.simplified();
    static const QRegularExpression filler(u"^(?:the|my)\\s+|\\s+(?:app|application)$"_s);
    for (QString previous; previous != name;) {
        previous = name;
        name.replace(filler, QString());
        name = name.simplified();
    }
    return name;
}

QStringList packageWords(const QString &component) {
    const QString package = component.section(u'/', 0, 0).toLower();
    QStringList words;
    for (const QString &word : package.split(QRegularExpression(u"[._]"_s), Qt::SkipEmptyParts)) {
        if (!genericWords().contains(word)) words.append(word);
    }
    return words;
}

double appScore(const QString &name, const QString &component) {
    const QStringList words = packageWords(component);
    if (words.isEmpty() || name.isEmpty()) return 0;
    const QString joined = QString(name).remove(u' ');
    double best = 0;
    for (const QString &word : words) {
        if (word == joined) return 1.0;
        if (joined.size() >= 4 && word.startsWith(joined)) best = std::max(best, 0.9);
        else if (joined.size() >= 4 && word.contains(joined)) best = std::max(best, 0.85);
    }
    // "files by google" → every meaningful word must appear in the package.
    const QStringList spokenWords = name.split(u' ', Qt::SkipEmptyParts);
    if (spokenWords.size() > 1) {
        int meaningful = 0;
        int found = 0;
        for (const QString &spokenWord : spokenWords) {
            if (spokenWord.size() < 3 || genericWords().contains(spokenWord)) continue;
            ++meaningful;
            if (std::any_of(words.cbegin(), words.cend(), [&](const QString &w) { return w.contains(spokenWord); })) {
                ++found;
            }
        }
        if (meaningful > 0 && found == meaningful) best = std::max(best, 0.85);
    }
    return best;
}

QString packageOf(const QString &component) {
    return component.section(u'/', 0, 0);
}

} // namespace

std::optional<AppTarget> matchApp(const QString &spoken, const QStringList &components) {
    const QString name = normalizedName(spoken);
    if (name.isEmpty()) return std::nullopt;

    for (const Alias &alias : aliases()) {
        if (!alias.names.contains(name)) continue;
        if (!alias.package.isEmpty()) {
            for (const QString &component : components) {
                if (packageOf(component) == alias.package) return AppTarget{component, {}, alias.label, 1.0};
            }
        }
        if (!alias.intentArguments.isEmpty()) return AppTarget{{}, alias.intentArguments, alias.label, 0.95};
        break;      // a known app that is not installed: fall through to the words, quietly
    }

    const QString *best = nullptr;
    double bestScore = 0;
    for (const QString &component : components) {
        const double score = appScore(name, component);
        // Ties go to the shorter package: com.whatsapp before com.whatsapp.w4b.
        if (score > bestScore
            || (score == bestScore && best && score > 0 && packageOf(component).size() < packageOf(*best).size())) {
            bestScore = score;
            best = &component;
        }
    }
    if (!best || bestScore < kMinAppScore) return std::nullopt;
    return AppTarget{*best, {}, packageOf(*best), bestScore};
}

QStringList closestApps(const QString &spoken, const QStringList &components, int count) {
    const QString name = normalizedName(spoken);
    QList<std::pair<double, QString>> scored;
    for (const QString &component : components) {
        const QString package = packageOf(component);
        double score = appScore(name, component);
        // Something to offer even when nothing is close: any shared letters of the first word.
        if (score == 0 && !name.isEmpty() && package.contains(name.left(3))) score = 0.1;
        if (score > 0) scored.append({score, package});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList names;
    for (const auto &[score, package] : scored) {
        if (names.size() >= count) break;
        if (!names.contains(package)) names.append(package);
    }
    return names;
}

QString labelForPackage(const QString &package) {
    for (const Alias &alias : aliases()) {
        if (!alias.package.isEmpty() && alias.package == package) return alias.label;
    }
    return package;
}

std::optional<Contact> matchContact(const QString &spoken, const QList<Contact> &contacts) {
    const QString wanted = normalizedName(spoken);
    if (wanted.isEmpty()) return std::nullopt;

    double bestScore = 0;
    QList<const Contact *> best;
    for (const Contact &contact : contacts) {
        const QString name = normalizedName(contact.name);
        if (name.isEmpty() || contact.number.isEmpty()) continue;
        double score = 0;
        if (name == wanted) score = 1.0;
        else if (name.section(u' ', 0, 0) == wanted) score = 0.9;     // first name
        else if (wanted.size() >= 3 && name.startsWith(wanted)) score = 0.75;
        else if (wanted.size() >= 3 && name.contains(wanted)) score = 0.7;
        if (score == 0) continue;
        if (score > bestScore) {
            bestScore = score;
            best = {&contact};
        } else if (score == bestScore) {
            best.append(&contact);
        }
    }
    if (best.isEmpty()) return std::nullopt;
    // Several numbers for one person is fine (the first is taken); two people is not.
    const QString first = normalizedName(best.first()->name);
    for (const Contact *contact : best) {
        if (normalizedName(contact->name) != first) return std::nullopt;
    }
    return *best.first();
}

} // namespace nexa::phonelookup
