#pragma once

#include "core/Result.h"
#include "system/android/AdbClient.h"
#include "system/android/AdbParsers.h"
#include "system/android/PhoneLookup.h"
#include "system/android/UiHierarchy.h"
#include "system/screen/ScreenCapture.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QStringList>
#include <QThreadPool>

#include <functional>
#include <optional>

namespace nexa {

// Facade over one Android phone reached through adb: which phone to use — the remembered one, else
// the only one connected — and what can be done with it. Every call is asynchronous. AndroidTools
// turns these into actions the model and the fast path can take; Settings → Android uses the
// pairing and connecting half.
class AndroidDevice : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<QString>)>;      // a sentence for the reply
    using Shot = ScreenCapture::Shot;

    struct Info {
        QString serial;             // as adb addresses it
        QString hardwareSerial;     // ro.serialno: the same phone over USB and over Wi-Fi
        QString manufacturer;
        QString model;
        QString release;            // Android version
        int battery{-1};
        QSize screen;
        bool wireless{false};

        QString summary() const;
    };

    explicit AndroidDevice(AdbClient *adb, QObject *parent = nullptr);
    ~AndroidDevice() override;

    // The phone to prefer when several are connected (its hardware serial), from the config.
    void setPreferred(const QString &hardwareSerial);
    QString preferred() const { return preferred_; }

    // Which phone: the preferred one when connected, else the only one. Remembered for a moment,
    // so a burst of tool calls costs one `adb devices`.
    void withDevice(std::function<void(Result<AdbDevice>)> done);
    void forgetDevices();
    QString currentDescription() const { return description_; }

    void info(std::function<void(Result<Info>)> done);
    // `adb shell <command>`: the output whatever the exit code, unless adb itself failed.
    void shell(const QString &command, int timeoutMs, std::function<void(Result<AdbClient::Output>)> done);
    // Runs a shell command and reports `success`, or what the phone said when it failed.
    void act(const QString &command, const QString &success, Done done);

    void openApp(const QString &name, Done done);
    void launchableApps(std::function<void(Result<QStringList>)> done);
    void screenshot(std::function<void(Result<Shot>)> done);
    // The screen as text (uiautomator dump → UiHierarchy): the outline, whose refs stay the current ones for
    // android_tap and android_type until the next.
    void uiHierarchy(const QString &filter, std::function<void(Result<uihierarchy::PhoneOutline>)> done);
    std::optional<QRect> uiBounds(const QString &ref) const;     // of a ref from the last outline
    QString uiName(const QString &ref) const;
    QSize lastScreenSize() const { return lastScreenSize_; }    // real pixels; invalid before any
    double lastScale() const { return lastScale_; }
    void screenSize(std::function<void(Result<QSize>)> done);

    // A number as given, or the number of the one contact the name matches.
    void resolveNumber(const QString &numberOrName, std::function<void(Result<phonelookup::Contact>)> done);

    void push(const QString &localPath, const QString &remotePath, Done done);
    void pull(const QString &remotePath, const QString &localPath, Done done);
    void install(const QString &apkPath, Done done);

    // Settings → Android.
    void pair(const QString &address, const QString &code, Done done);
    void connectTo(const QString &address, Done done);
    void disconnectAll(Done done);

    void cancelAll();

    // Where the phone's http://localhost:7777 should lead on the PC: the web remote's loopback port.
    // Applied to the phone in use now, and to every phone that connects later.
    void setReversePort(int pcPort);
    // Sets that route up on the phone in use right now and says whether it took.
    void reverse(Done done);

signals:
    // A different phone — or none — is now the one in use.
    void deviceChanged(const QString &description);

private:
    void chooseFrom(const QList<AdbDevice> &devices, std::function<void(Result<AdbDevice>)> done);
    void contacts(std::function<void(Result<QList<phonelookup::Contact>>)> done);
    void adopt(const AdbDevice &device);
    void applyReverse(const QString &serial);

    AdbClient *adb_;
    QString preferred_;
    QString currentSerial_;
    QString description_;
    QList<AdbDevice> devices_;
    QElapsedTimer devicesAge_;
    QStringList apps_;
    QElapsedTimer appsAge_;
    QList<phonelookup::Contact> contacts_;
    QElapsedTimer contactsAge_;
    QSize screen_;
    uihierarchy::PhoneOutline lastUi_;
    QSize lastScreenSize_;
    double lastScale_{1.0};
    int reversePort_{7778};
    QThreadPool decoder_;
};

} // namespace nexa
