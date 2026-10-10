#include "system/android/AdbParsers.h"

#include <QRegularExpression>
#include <QSet>

using namespace Qt::StringLiterals;

namespace nexa {

bool AdbDevice::isWireless() const {
    return serial.contains(u':') || serial.contains(u"._adb-tls-connect."_s);
}

QString AdbDevice::displayModel() const {
    QString name = model;
    name.replace(u'_', u' ');
    return name.isEmpty() ? serial : name;
}

QString MdnsService::address() const {
    return host.contains(u':') ? u"[%1]:%2"_s.arg(host).arg(port) : u"%1:%2"_s.arg(host).arg(port);
}

namespace adb {

namespace {

QStringList lines(const QString &output) {
    QString text = output;
    text.replace(u"\r\n"_s, u"\n"_s);
    return text.split(u'\n');
}

// A dumped CharSequence: "String (Hello)", "SpannableString (Hi)", "null", or — when the dump was
// redacted — "String [length=5]", which says nothing about the words.
QString charSequenceValue(const QString &value) {
    const QString trimmed = value.trimmed();
    const qsizetype open = trimmed.indexOf(u'(');
    if (open < 0 || !trimmed.endsWith(u')')) return {};
    return trimmed.mid(open + 1, trimmed.size() - open - 2).trimmed();
}

} // namespace

QList<AdbDevice> parseDevices(const QString &output) {
    static const QRegularExpression field(u"^(usb|product|model|device|transport_id):(.*)$"_s);
    QList<AdbDevice> devices;
    for (const QString &raw : lines(output)) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(u'*') || line.startsWith(u"List of devices"_s)) continue;
        const QStringList tokens = line.split(QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts);
        if (tokens.size() < 2) continue;

        AdbDevice device;
        device.serial = tokens.first();
        // "no permissions (missing udev rules? …); see [http://…]" is a state with spaces in it.
        const QString rest = line.mid(device.serial.size()).trimmed();
        device.state = rest.startsWith(u"no permissions"_s) ? u"no permissions"_s : tokens.at(1);
        for (qsizetype i = 2; i < tokens.size(); ++i) {
            const QRegularExpressionMatch m = field.match(tokens.at(i));
            if (!m.hasMatch()) continue;
            const QString key = m.captured(1);
            if (key == u"usb") device.usb = true;
            else if (key == u"product") device.product = m.captured(2);
            else if (key == u"model") device.model = m.captured(2);
            else if (key == u"device") device.deviceName = m.captured(2);
            else if (key == u"transport_id") device.transportId = m.captured(2);
        }
        devices.append(device);
    }
    return devices;
}

QList<MdnsService> parseMdnsServices(const QString &output) {
    QList<MdnsService> services;
    for (const QString &raw : lines(output)) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(u"List of discovered"_s)) continue;
        const QStringList tokens = line.split(QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts);
        if (tokens.size() < 3) continue;

        const QString address = tokens.at(2);
        const qsizetype colon = address.lastIndexOf(u':');
        if (colon <= 0) continue;
        bool numeric = false;
        const int port = address.mid(colon + 1).toInt(&numeric);
        if (!numeric || port <= 0 || port > 65535) continue;

        MdnsService service;
        service.name = tokens.at(0);
        const QString type = tokens.at(1);
        service.kind = type.contains(u"_adb-tls-pairing"_s)   ? MdnsService::Kind::Pairing
                       : type.contains(u"_adb-tls-connect"_s) ? MdnsService::Kind::Connect
                                                              : MdnsService::Kind::Other;
        QString host = address.left(colon);
        if (host.startsWith(u'[') && host.endsWith(u']')) host = host.mid(1, host.size() - 2);
        service.host = host;
        service.port = port;
        services.append(service);
    }
    return services;
}

std::optional<QSize> parseWmSize(const QString &output) {
    static const QRegularExpression size(u"^\\s*(Physical|Override) size:\\s*(\\d+)x(\\d+)"_s,
                                         QRegularExpression::MultilineOption);
    std::optional<QSize> physical;
    std::optional<QSize> override;
    QRegularExpressionMatchIterator it = size.globalMatch(output);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QSize value(m.captured(2).toInt(), m.captured(3).toInt());
        if (value.isEmpty()) continue;
        (m.captured(1) == u"Override" ? override : physical) = value;
    }
    return override ? override : physical;
}

QList<PhoneNotification> parseNotifications(const QString &output) {
    // The first pkg= on the line: an icon further along may name another package.
    static const QRegularExpression record(u"NotificationRecord\\(.*?\\bpkg=(\\S+)"_s);
    static const QRegularExpression extra(u"^\\s*android\\.(title|text|bigText)=(.*)$"_s);
    static const QRegularExpression time(u"\\b(?:mCreationTimeMs|creationTimeMs|postTime|when)=(\\d{10,})"_s);

    QList<PhoneNotification> notifications;
    QSet<QString> seen;
    PhoneNotification current;
    QString bigText;
    bool inRecord = false;

    const auto flush = [&] {
        if (!inRecord) return;
        if (bigText.size() > current.text.size()) current.text = bigText;
        const QString key = current.package + u'\n' + current.title + u'\n' + current.text;
        if ((!current.title.isEmpty() || !current.text.isEmpty()) && !seen.contains(key)) {
            seen.insert(key);
            notifications.append(current);
        }
        current = {};
        bigText.clear();
        inRecord = false;
    };

    const QStringList all = lines(output);
    for (qsizetype i = 0; i < all.size(); ++i) {
        const QString &line = all.at(i);
        if (const QRegularExpressionMatch m = record.match(line); m.hasMatch()) {
            flush();
            inRecord = true;
            current.package = m.captured(1);
            continue;
        }
        if (!inRecord) continue;
        if (const QRegularExpressionMatch m = extra.match(line); m.hasMatch()) {
            QString value = m.captured(2);
            // A text with line breaks carries on over the following lines, up to its ')'.
            for (int extraLines = 0; !value.trimmed().endsWith(u')') && value.contains(u'(') && extraLines < 20
                                     && i + 1 < all.size();
                 ++extraLines) {
                value += u'\n' + all.at(++i);
            }
            const QString words = charSequenceValue(value);
            if (m.captured(1) == u"title" && current.title.isEmpty()) current.title = words;
            else if (m.captured(1) == u"text" && current.text.isEmpty()) current.text = words;
            else if (m.captured(1) == u"bigText" && bigText.isEmpty()) bigText = words;
            continue;
        }
        if (current.timeMs == 0) {
            if (const QRegularExpressionMatch m = time.match(line); m.hasMatch()) current.timeMs = m.captured(1).toLongLong();
        }
    }
    flush();
    return notifications;
}

QList<QStringList> parseContentRows(const QString &output, const QStringList &columns) {
    static const QRegularExpression rowStart(u"^Row:\\s*\\d+\\s+"_s);
    QList<QStringList> rows;
    if (columns.isEmpty()) return rows;

    // Gather each row's text first: a value with a line break continues on the next lines.
    QStringList texts;
    for (const QString &line : lines(output)) {
        if (const QRegularExpressionMatch m = rowStart.match(line); m.hasMatch()) {
            texts.append(line.mid(m.capturedLength()));
        } else if (!texts.isEmpty()) {
            texts.last() += u'\n' + line;
        }
    }

    for (QString text : texts) {
        while (text.endsWith(u'\n')) text.chop(1);
        QStringList values;
        qsizetype position = 0;
        bool complete = true;
        for (qsizetype c = 0; c < columns.size(); ++c) {
            const QString marker = columns.at(c) + u'=';
            if (!text.mid(position).startsWith(marker)) {
                complete = false;
                break;
            }
            const qsizetype start = position + marker.size();
            qsizetype end = text.size();
            if (c + 1 < columns.size()) {
                end = text.indexOf(u", "_s + columns.at(c + 1) + u'=', start);
                if (end < 0) {
                    complete = false;
                    break;
                }
            }
            QString value = text.mid(start, end - start);
            if (value == u"NULL") value.clear();
            values.append(value);
            position = end + 2;     // past ", "
        }
        if (complete) rows.append(values);
    }
    return rows;
}

QStringList parseLauncherComponents(const QString &output) {
    static const QRegularExpression component(u"^\\s*([A-Za-z][\\w]*(?:\\.[\\w]+)+)/([\\w.$]+)\\s*$"_s);
    QStringList components;
    for (const QString &line : lines(output)) {
        const QRegularExpressionMatch m = component.match(line);
        if (!m.hasMatch()) continue;
        const QString name = m.captured(1) + u'/' + m.captured(2);
        if (!components.contains(name)) components.append(name);
    }
    return components;
}

QStringList parsePackages(const QString &output) {
    QStringList packages;
    for (const QString &line : lines(output)) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(u"package:"_s)) continue;
        // "package:/data/app/…/base.apk=com.example" with -f; "package:com.example" without.
        QString name = trimmed.mid(8);
        if (const qsizetype equals = name.lastIndexOf(u'='); equals >= 0) name = name.mid(equals + 1);
        name = name.section(u' ', 0, 0);
        if (!name.isEmpty() && !packages.contains(name)) packages.append(name);
    }
    return packages;
}

std::optional<StreamVolume> parseVolume(const QString &output) {
    static const QRegularExpression volume(u"volume is (\\d+) in range \\[(\\d+)\\.\\.(\\d+)\\]"_s);
    const QRegularExpressionMatch m = volume.match(output);
    if (!m.hasMatch()) return std::nullopt;
    const StreamVolume parsed{m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt()};
    if (parsed.maximum <= parsed.minimum) return std::nullopt;
    return parsed;
}

// - STREAM_MUSIC:
//    Muted: false
//    Min: 0
//    Max: 30
//    streamVolume:30                                   <- Android 11 and later
//    Current: 2 (speaker): 30, 4 (headset): 29, 40000000 (default): 15
//    Devices: speaker
std::optional<StreamVolume> parseDumpsysVolume(const QString &output, const QString &stream) {
    // The block runs from the stream's heading to the next line that starts at column 0.
    const QRegularExpression heading(u"^- %1:[ \\t]*$"_s.arg(QRegularExpression::escape(stream)),
                                     QRegularExpression::MultilineOption);
    const QRegularExpressionMatch start = heading.match(output);
    if (!start.hasMatch()) return std::nullopt;
    static const QRegularExpression nextBlock(u"^\\S"_s, QRegularExpression::MultilineOption);
    const qsizetype from = start.capturedEnd();
    const qsizetype to = output.indexOf(nextBlock, from);
    const QString block = output.mid(from, to < 0 ? -1 : to - from);

    static const QRegularExpression line(u"^\\s*(Min|Max|streamVolume):\\s*(\\d+)\\s*$"_s, QRegularExpression::MultilineOption);
    StreamVolume parsed;
    std::optional<int> current;
    for (QRegularExpressionMatchIterator it = line.globalMatch(block); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int value = m.captured(2).toInt();
        if (m.captured(1) == u"Min"_s) parsed.minimum = value;
        else if (m.captured(1) == u"Max"_s) parsed.maximum = value;
        else current = value;
    }
    if (!current) {
        // Older phones: the value for the device the stream plays on, else the default one.
        static const QRegularExpression devices(u"^\\s*Devices:\\s*([A-Za-z0-9_]+)"_s, QRegularExpression::MultilineOption);
        static const QRegularExpression entry(u"\\(([A-Za-z0-9_]+)\\):\\s*(\\d+)"_s);
        const QString device = devices.match(block).captured(1);
        std::optional<int> fallback;
        for (QRegularExpressionMatchIterator it = entry.globalMatch(block); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            if (m.captured(1) == device) current = m.captured(2).toInt();
            else if (m.captured(1) == u"default"_s || !fallback) fallback = m.captured(2).toInt();
        }
        if (!current) current = fallback;
    }
    if (!current || parsed.maximum <= parsed.minimum) return std::nullopt;
    parsed.current = std::clamp(*current, parsed.minimum, parsed.maximum);
    return parsed;
}

std::optional<int> parseBatteryLevel(const QString &output) {
    static const QRegularExpression level(u"^\\s*level:\\s*(\\d+)\\s*$"_s, QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = level.match(output);
    if (!m.hasMatch()) return std::nullopt;
    return m.captured(1).toInt();
}

QHash<QString, QString> parseProperties(const QString &output) {
    QHash<QString, QString> properties;
    for (const QString &line : lines(output)) {
        const qsizetype equals = line.indexOf(u'=');
        if (equals <= 0) continue;
        const QString key = line.left(equals).trimmed();
        if (key.isEmpty() || key.contains(u' ') || properties.contains(key)) continue;
        properties.insert(key, line.mid(equals + 1).trimmed());
    }
    return properties;
}

QString serialFromServiceName(const QString &name) {
    static const QRegularExpression pattern(u"^adb-([A-Za-z0-9]+)-[A-Za-z0-9]+"_s);
    const QRegularExpressionMatch m = pattern.match(name);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace adb

} // namespace nexa
