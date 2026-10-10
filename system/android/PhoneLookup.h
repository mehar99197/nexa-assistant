#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa::phonelookup {

// Pure name matching for the phone: which app "youtube" means, which contact "mom" means.

// How to start an app: a launcher component found on the phone, or — for generic names such as
// "camera" or "messages" — an intent any phone can resolve, whatever its vendor calls the app.
struct AppTarget {
    QString component;          // "com.google.android.youtube/.app.honeycomb.Shell$HomeActivity"
    QString intentArguments;    // "-a android.media.action.STILL_IMAGE_CAMERA", when there is no component
    QString label;              // what to call it in a reply
    double score{0};
};

// `components` are the phone's launcher activities ("package/activity"). Known names come first
// (WhatsApp is com.whatsapp, Gmail is com.google.android.gm), then the words of the package names.
// Empty when nothing matches well enough — better to say so than to open the wrong app.
std::optional<AppTarget> matchApp(const QString &spoken, const QStringList &components);
// Candidates for "did you mean", best first.
QStringList closestApps(const QString &spoken, const QStringList &components, int count = 5);
// "com.whatsapp" → "WhatsApp" for the apps Nexa knows by name, else the package itself.
QString labelForPackage(const QString &package);

struct Contact {
    QString name;
    QString number;
};
// The contact whose name best matches; exact before first-name before partial. Empty when none
// matches or when two different people match equally well — calling the wrong one is worse.
std::optional<Contact> matchContact(const QString &spoken, const QList<Contact> &contacts);

} // namespace nexa::phonelookup
