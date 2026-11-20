#pragma once

#include "core/Result.h"

#include <QObject>
#include <QString>

namespace nexa {

// Facade over the desktop's accessibility bus (AT-SPI2): the session bus's org.a11y.Bus says where it is, and the
// connection "nexa-a11y" is opened on first use (a screen reader's own bus; every GTK, Qt, Chromium and LibreOffice
// window describes itself on it). Also the desktop's switch that makes Qt, Chromium and Electron apps expose their
// contents — org.a11y.Status IsEnabled, which GNOME keeps as toolkit-accessibility — read and, from Settings, set.
class AtspiBus : public QObject {
    Q_OBJECT
public:
    static constexpr QLatin1StringView kConnectionName{"nexa-a11y"};

    explicit AtspiBus(QObject *parent = nullptr);

    bool isAvailable() const;                   // connects the first time it is asked (one short call)
    QString unavailableReason() const;          // why not, in words
    QString connectionName() const { return QString(kConnectionName); }

    Result<bool> appsExposeContents() const;
    Result<void> setAppsExposeContents(bool on);

private:
    void ensureConnected() const;

    mutable bool tried_{false};
    mutable bool available_{false};
    mutable QString reason_;
};

} // namespace nexa
