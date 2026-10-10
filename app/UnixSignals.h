#pragma once

#include <QObject>

class QSocketNotifier;

namespace nexa {

// Turns SIGTERM, SIGINT and SIGHUP into a Qt signal (self-pipe trick), so `kill`, Ctrl+C or a
// logout end Nexa through a normal quit: destructors run and the process groups of running
// commands are killed instead of orphaned. One instance per process.
class UnixSignals : public QObject {
    Q_OBJECT
public:
    explicit UnixSignals(QObject *parent = nullptr);
    ~UnixSignals() override;

signals:
    void received(int signal);

private:
    static void handle(int signal);

    static inline int fds_[2] = {-1, -1};
    QSocketNotifier *notifier_{nullptr};
};

} // namespace nexa
