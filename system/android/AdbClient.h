#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QProcess;

namespace nexa {

// Facade over the `adb` program: one short-lived adb client process per call, talking to the one
// adb server that stays running. Asynchronous — nothing waits on the GUI thread — with a timeout
// per call, and cancelAll() for the panic button.
class AdbClient : public QObject {
    Q_OBJECT
public:
    struct Output {
        QByteArray standardOutput;
        QByteArray standardError;
        int exitCode{0};

        QString text() const;           // stdout as UTF-8, without the trailing newline
        QString errorText() const;      // stderr likewise
    };
    using Done = std::function<void(Result<Output>)>;

    explicit AdbClient(QObject *parent = nullptr);
    ~AdbClient() override;

    static bool isInstalled();

    // `adb <arguments>` — for commands about adb itself: devices, mdns, pair, connect.
    void run(const QStringList &arguments, int timeoutMs, Done done);
    // `adb -s <serial> <arguments>`.
    void runOn(const QString &serial, const QStringList &arguments, int timeoutMs, Done done);
    void cancelAll();
    int runningCount() const { return int(jobs_.size()); }

    // adb's own complaints ("adb: device unauthorized") as an error that says what to do about
    // them. Empty for a command that ran — even one that failed on the phone, which is the
    // caller's to judge.
    static std::optional<Error> errorFrom(const Output &output);

private:
    struct Job;
    void finish(QProcess *process, Result<Output> result);

    QHash<QProcess *, std::shared_ptr<Job>> jobs_;
};

} // namespace nexa
