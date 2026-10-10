#pragma once

#include <QJsonObject>
#include <QString>

namespace nexa {

// Facts about this PC from /etc/os-release, /proc, /sys and Qt (fast, synchronous reads).
class SystemInfo {
public:
    // os, kernel, hostname, user, desktop, session, cpu, cores, memory, load, uptime, disks, battery
    static QJsonObject collect();
    // One line for the system prompt: distro, desktop + session type, user, home, cores, RAM.
    static QString summary();
};

} // namespace nexa
