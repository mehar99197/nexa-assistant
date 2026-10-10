#pragma once

#include <QHash>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa {

// One line of `adb devices -l`.
struct AdbDevice {
    QString serial;             // what `adb -s` takes: "R58M…", "192.168.18.50:37135", "adb-R58M…-x._adb-tls-connect._tcp"
    QString state;              // device, offline, unauthorized, "no permissions", …
    QString model;              // as adb prints it, underscores for spaces ("SM_A525F")
    QString product;
    QString deviceName;
    QString transportId;
    bool usb{false};

    bool isReady() const { return state == QStringLiteral("device"); }
    bool isWireless() const;
    // The model as a person would write it.
    QString displayModel() const;
};

// One line of `adb mdns services`: a phone advertising wireless debugging.
struct MdnsService {
    enum class Kind { Pairing, Connect, Other };
    QString name;               // "adb-R58M12345-AbCdEf" — the hardware serial is in the middle
    Kind kind{Kind::Other};
    QString host;
    int port{0};

    QString address() const;    // host:port, with brackets around an IPv6 host
};

struct PhoneNotification {
    QString package;
    QString title;
    QString text;
    qint64 timeMs{0};           // 0 when the dump did not say
};

struct StreamVolume {
    int current{0};
    int minimum{0};
    int maximum{0};
};

// Pure parsers for what adb and the phone's shell print (Android 7 to 15, several OEMs). They are
// tolerant on purpose: output varies between versions and vendors, and a line that cannot be read
// is skipped rather than allowed to spoil the rest.
namespace adb {

QList<AdbDevice> parseDevices(const QString &output);
QList<MdnsService> parseMdnsServices(const QString &output);
// `wm size`: the override size when one is set (that is what the screen uses), else the physical.
std::optional<QSize> parseWmSize(const QString &output);
QList<PhoneNotification> parseNotifications(const QString &output);
// `content query … --projection a:b:c`: one list of values per row, in the order of `columns`.
// Values may contain ", " and newlines, so the last column takes everything to the end of the row —
// put the free-text column last. NULL becomes an empty string.
QList<QStringList> parseContentRows(const QString &output, const QStringList &columns);
// `cmd package query-activities … LAUNCHER`: "package/activity" components, in order, no duplicates.
QStringList parseLauncherComponents(const QString &output);
// `pm list packages`: package names.
QStringList parsePackages(const QString &output);
// `media volume --get` and `cmd media_session volume --get`: "volume is 7 in range [0..15]".
std::optional<StreamVolume> parseVolume(const QString &output);
// `dumpsys audio`: the block of one stream ("STREAM_MUSIC"), which every Android prints. The current
// value is the `streamVolume:` line where there is one, else the entry for the device in use.
std::optional<StreamVolume> parseDumpsysVolume(const QString &output, const QString &stream);
// `dumpsys battery`: the "level:" line.
std::optional<int> parseBatteryLevel(const QString &output);
// Lines of the form key=value; the first occurrence of a key wins.
QHash<QString, QString> parseProperties(const QString &output);
// The hardware serial inside an mDNS service or device name ("adb-<serial>-<suffix>…"), or empty.
QString serialFromServiceName(const QString &name);

} // namespace adb

} // namespace nexa
