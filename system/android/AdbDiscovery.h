#pragma once

#include "system/android/AdbParsers.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

namespace nexa {

class AdbClient;
class MdnsBrowser;

// Observer of what adb can see: phones connected (`adb devices -l`) and phones offering wireless
// debugging on the network (`adb mdns services`, and Nexa's own MdnsBrowser, because Debian's adb
// has no mDNS at all).
//
// With a remembered phone it also reconnects to it when it reappears. The wireless debugging port
// changes every time the phone switches it on, so a remembered address would go stale; the phone's
// hardware serial, which is part of its mDNS service name, does not.
class AdbDiscovery : public QObject {
    Q_OBJECT
public:
    explicit AdbDiscovery(AdbClient *adb, MdnsBrowser *browser = nullptr, QObject *parent = nullptr);

    // Polling costs two short adb calls, so it runs only when someone needs the answer: often
    // while Settings → Android is open, now and then while a remembered phone should be
    // reconnected, and not at all otherwise.
    void setWatched(bool watched);
    void setBackground(bool enabled);
    bool isRunning() const { return timer_.isActive(); }
    void refresh();
    // The phone to reconnect to, by hardware serial; empty for none.
    void setAutoConnect(const QString &hardwareSerial);

    QList<AdbDevice> devices() const { return devices_; }
    QList<MdnsService> services() const { return services_; }

    // Pure: the address to connect to, if the remembered phone advertises one and is not already
    // connected through it.
    static QString reconnectAddress(const QString &hardwareSerial, const QList<AdbDevice> &devices,
                                    const QList<MdnsService> &services);
    // Pure: adb's list and the browser's, without the duplicates.
    static QList<MdnsService> merged(QList<MdnsService> first, const QList<MdnsService> &second);

signals:
    void changed();
    void reconnecting(const QString &address);

private:
    void poll();
    void finishPoll(QList<AdbDevice> devices, QList<MdnsService> services);
    void reschedule();

    AdbClient *adb_;
    MdnsBrowser *browser_;
    QTimer timer_;
    QString autoConnect_;
    QList<AdbDevice> devices_;
    QList<MdnsService> services_;
    bool polling_{false};
    bool watched_{false};
    bool background_{false};
    QString lastAttempt_;       // the address last tried, so a failing one is not hammered
};

} // namespace nexa
