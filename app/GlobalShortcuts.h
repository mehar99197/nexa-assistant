#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace nexa {

// Facade over org.freedesktop.portal.GlobalShortcuts: hotkeys that reach Nexa while another
// window has focus. The compositor owns the keys — Nexa never reads the keyboard, it is only told
// when a shortcut the user accepted goes down or up. GNOME asks once which keys to grant, and the
// user can change or remove them later in Settings → Keyboard. Where the portal is missing the
// facade stays quiet and the in-window shortcuts remain the only way.
class GlobalShortcuts : public QObject {
    Q_OBJECT
public:
    struct Shortcut {
        QString id;
        QString description;        // shown in GNOME's dialog and in Settings → Keyboard
        QString preferredTrigger;   // e.g. "CTRL+ALT+space"; the user may choose another
    };

    // `appId` is the basename of Nexa's .desktop file; GNOME stores the hotkeys under it and
    // shows that app's name in its dialog and in Settings → Keyboard.
    explicit GlobalShortcuts(QString appId, QObject *parent = nullptr);
    ~GlobalShortcuts() override;

    // Opens a portal session and asks for these shortcuts; emits registered() or unavailable().
    void bind(QList<Shortcut> shortcuts);

    // The key the user assigned ("Ctrl+Alt+Space"). Empty when the shortcut is registered but has
    // no key yet — GNOME ignores preferred_trigger and expects Settings → Keyboard to set it.
    QString trigger(const QString &id) const { return triggers_.value(id); }
    bool isRegistered() const { return registered_; }

signals:
    void pressed(const QString &id);
    void released(const QString &id);
    // The shortcuts are registered; trigger() tells which keys (if any) they carry. Emitted again
    // whenever the user changes them in Settings → Keyboard.
    void registered();
    void unavailable(const QString &reason);

private slots:
    void onSessionCreated(uint response, const QVariantMap &results);
    void onShortcutsBound(uint response, const QVariantMap &results);
    void onActivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp,
                     const QVariantMap &options);
    void onDeactivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp,
                       const QVariantMap &options);
    void onShortcutsChanged(const QDBusMessage &message);
    void onSessionClosed(const QVariantMap &details);

private:
    void registerApp();
    void createSession();
    void requestShortcuts();
    QString subscribe(const char *slot);        // listens on a fresh Request path and returns it
    void resubscribe(QString &path, const QString &actual, const char *slot);
    void unsubscribe(const QString &path, const char *slot);
    void giveUp(const QString &reason);
    void readTriggers(const QVariant &shortcuts);

    QString appId_;
    QDBusConnection bus_;
    QList<Shortcut> wanted_;
    QString sessionPath_;
    QString createPath_;        // Request of CreateSession
    QString bindPath_;          // Request of BindShortcuts
    QHash<QString, QString> triggers_;
    bool registered_{false};    // the desktop accepted the shortcuts, with or without keys
};

} // namespace nexa
