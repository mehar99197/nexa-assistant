#include "system/info/SystemInfo.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString readText(const QString &path, qint64 limit = 64 * 1024) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.read(limit));
}

QString osName() {
    static const QRegularExpression pretty(u"^PRETTY_NAME=\"?([^\"\\n]*)\"?$"_s, QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = pretty.match(readText(u"/etc/os-release"_s));
    return match.hasMatch() ? match.captured(1) : QSysInfo::prettyProductName();
}

QString cpuModel() {
    static const QRegularExpression model(u"^model name\\s*:\\s*(.+)$"_s, QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = model.match(readText(u"/proc/cpuinfo"_s, 16 * 1024));
    return match.hasMatch() ? match.captured(1).trimmed() : QSysInfo::currentCpuArchitecture();
}

qint64 meminfoMiB(const QString &meminfo, const QString &key) {
    const QRegularExpression line(u"^"_s + key + u":\\s*(\\d+)\\s*kB"_s, QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = line.match(meminfo);
    return match.hasMatch() ? match.captured(1).toLongLong() / 1024 : 0;
}

QJsonObject disk(const QString &path) {
    const QStorageInfo storage(path);
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    return QJsonObject{
        {u"path"_s, path},
        {u"device"_s, QString::fromLocal8Bit(storage.device())},
        {u"totalGiB"_s, qRound(storage.bytesTotal() / kGiB * 10) / 10.0},
        {u"freeGiB"_s, qRound(storage.bytesAvailable() / kGiB * 10) / 10.0},
    };
}

QJsonArray batteries() {
    QJsonArray result;
    const QDir supplies(u"/sys/class/power_supply"_s);
    for (const QString &name : supplies.entryList({u"BAT*"_s}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = supplies.filePath(name);
        result.append(QJsonObject{
            {u"name"_s, name},
            {u"percent"_s, readText(base + u"/capacity"_s).trimmed().toInt()},
            {u"status"_s, readText(base + u"/status"_s).trimmed()},
        });
    }
    return result;
}

} // namespace

QJsonObject SystemInfo::collect() {
#ifdef Q_OS_WIN
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    const bool hasMem = ::GlobalMemoryStatusEx(&mem) != 0;
    const qint64 totalMiB = hasMem ? qint64(mem.ullTotalPhys / (1024 * 1024)) : 0;
    const qint64 availMiB = hasMem ? qint64(mem.ullAvailPhys / (1024 * 1024)) : 0;

    QJsonObject memory;
    memory.insert(u"total_mib"_s, totalMiB);
    memory.insert(u"available_mib"_s, availMiB);

    QJsonObject disks;
    const QString root = QDir::rootPath();
    disks.insert(root, disk(root));
    const QString home = QDir::homePath();
    if (!home.isEmpty()) disks.insert(home, disk(home));

    const qint64 upSecs = static_cast<qint64>(::GetTickCount64() / 1000ULL);
    return QJsonObject{
        {u"os"_s, QSysInfo::prettyProductName()},
        {u"kernel"_s, QSysInfo::kernelVersion()},
        {u"hostname"_s, QSysInfo::machineHostName()},
        {u"user"_s, qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"))},
        {u"desktop"_s, u"Windows"_s},
        {u"session"_s, QSysInfo::currentCpuArchitecture()},
        {u"cpu"_s, QSysInfo::currentCpuArchitecture()},
        {u"cores"_s, QThread::idealThreadCount()},
        {u"memory"_s, memory},
        {u"uptime_seconds"_s, upSecs},
        {u"disks"_s, disks},
    };
#else
    const QString meminfo = readText(u"/proc/meminfo"_s);
    const double uptimeSeconds = readText(u"/proc/uptime"_s).section(u' ', 0, 0).toDouble();
    QJsonArray disks{disk(u"/"_s)};
    if (QStorageInfo(QDir::homePath()).rootPath() != u"/") disks.append(disk(QDir::homePath()));

    return QJsonObject{
        {u"os"_s, osName()},
        {u"kernel"_s, QSysInfo::kernelVersion()},
        {u"architecture"_s, QSysInfo::currentCpuArchitecture()},
        {u"hostname"_s, QSysInfo::machineHostName()},
        {u"user"_s, qEnvironmentVariable("USER")},
        {u"home"_s, QDir::homePath()},
        {u"desktop"_s, qEnvironmentVariable("XDG_CURRENT_DESKTOP")},
        {u"session"_s, qEnvironmentVariable("XDG_SESSION_TYPE")},
        {u"cpu"_s, cpuModel()},
        {u"cores"_s, QThread::idealThreadCount()},
        {u"memoryTotalMiB"_s, meminfoMiB(meminfo, u"MemTotal"_s)},
        {u"memoryAvailableMiB"_s, meminfoMiB(meminfo, u"MemAvailable"_s)},
        {u"load"_s, readText(u"/proc/loadavg"_s).section(u' ', 0, 2)},
        {u"uptimeHours"_s, qRound(uptimeSeconds / 360.0) / 10.0},
        {u"disks"_s, disks},
        {u"batteries"_s, batteries()},
    };
#endif
}

QString SystemInfo::summary() {
#ifdef Q_OS_WIN
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    const qint64 memoryMiB = ::GlobalMemoryStatusEx(&mem) ? qint64(mem.ullTotalPhys / (1024 * 1024)) : 0;
    const QString user = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
    return u"%1; Windows desktop (%2); user %3, home %4; %5 CPU cores, %6 GB RAM."_s
        .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), user, QDir::homePath())
        .arg(QThread::idealThreadCount())
        .arg(memoryMiB / 1024.0, 0, 'f', 1);
#else
    const qint64 memoryMiB = meminfoMiB(readText(u"/proc/meminfo"_s), u"MemTotal"_s);
    return u"%1; %2 desktop on %3; user %4, home %5; %6 CPU cores, %7 GB RAM."_s
        .arg(osName(), qEnvironmentVariable("XDG_CURRENT_DESKTOP", u"unknown"_s),
             qEnvironmentVariable("XDG_SESSION_TYPE", u"unknown"_s), qEnvironmentVariable("USER"), QDir::homePath())
        .arg(QThread::idealThreadCount())
        .arg(memoryMiB / 1024.0, 0, 'f', 1);
#endif
}

} // namespace nexa
