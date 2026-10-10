#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa {

// One application .desktop file (freedesktop Desktop Entry spec). Parsing is pure.
struct DesktopEntry {
    QString id;                 // desktop file ID, e.g. "org.gnome.Nautilus.desktop"
    QString path;
    QString name;
    QString genericName;
    QString comment;
    QStringList keywords;
    QStringList categories;
    QString exec;               // Exec value with string escapes resolved (quoting still applies)
    QString tryExec;
    QString icon;
    QStringList onlyShowIn;
    QStringList notShowIn;
    bool terminal{false};
    bool dbusActivatable{false};
    bool noDisplay{false};
    bool hidden{false};

    // nullopt unless the file has a [Desktop Entry] group with Type=Application. `locales` is the
    // preference list for localized keys, best first (see preferredLocales()).
    static std::optional<DesktopEntry> parse(const QByteArray &content, const QString &id, const QString &path,
                                             const QStringList &locales = {});
    // Exec → argv: quoting and escapes resolved, field codes (%f %U %i …) removed, %% → %.
    // Empty when the line is malformed (e.g. an unterminated quote).
    static QStringList execArguments(const QString &exec);
    static QStringList preferredLocales();      // from the system locale: {"de_DE", "de"}

    // Basename of the program Exec starts, skipping `env` and VAR=value prefixes ("firefox-esr").
    QString executableName() const;
    // The application ID for `flatpak run <id>` entries; empty otherwise.
    QString flatpakAppId() const;
    // Honours OnlyShowIn / NotShowIn for the current desktops (XDG_CURRENT_DESKTOP).
    bool isShownIn(const QStringList &desktops) const;
};

} // namespace nexa
