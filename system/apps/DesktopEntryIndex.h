#pragma once

#include "core/intent/IIntentHandler.h"
#include "system/apps/DesktopEntry.h"

#include <QList>
#include <QObject>
#include <QStringList>
#include <QThreadPool>

#include <optional>

namespace nexa {

// Index of the launchable applications (.desktop files in the XDG data dirs), scanned off the GUI
// thread. Also the fast path's IAppCatalog: fuzzy name matching scored 0..1.
class DesktopEntryIndex : public QObject, public IAppCatalog {
    Q_OBJECT
public:
    struct Match {
        DesktopEntry entry;
        double score{0};
    };

    explicit DesktopEntryIndex(QStringList dataDirs = defaultDataDirs(), QStringList desktops = currentDesktops(),
                               QObject *parent = nullptr);
    ~DesktopEntryIndex() override;

    void refresh();         // asynchronous rescan; emits ready()
    void refreshNow();      // synchronous rescan (tests, or a lookup before the first scan finished)
    bool isReady() const { return ready_; }
    const QList<DesktopEntry> &entries() const { return entries_; }

    QList<Match> search(const QString &query, int limit = 5) const;
    std::optional<Match> best(const QString &query) const;
    double score(const QString &query) const override;

    static QStringList defaultDataDirs();       // $XDG_DATA_HOME first, then $XDG_DATA_DIRS
    static QStringList currentDesktops();       // $XDG_CURRENT_DESKTOP, e.g. {"GNOME"}
    // Earlier directories shadow later ones with the same desktop ID (spec precedence).
    static QList<DesktopEntry> scan(const QStringList &dataDirs, const QStringList &desktops,
                                    const QStringList &locales);
    // How well `query` names `entry`: 1 exact name/executable, 0.95 prefix ("firefox" → "Firefox ESR"),
    // 0.9 generic name, 0.85 keyword or whole words ("code" → "Visual Studio Code"), ≤0.9 typo distance.
    static double matchScore(const DesktopEntry &entry, const QString &query);

signals:
    void ready(int count);

private:
    QStringList dataDirs_;
    QStringList desktops_;
    QList<DesktopEntry> entries_;
    bool ready_{false};
    QThreadPool pool_;
};

} // namespace nexa
