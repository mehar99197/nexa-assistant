#pragma once

#include "core/Result.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>

class HelperProxy;
class QDBusError;
class QDBusPendingCall;
class QDBusServiceWatcher;

namespace nexa {

// Facade over org.nexa.Helper: one place that knows the helper is reached over D-Bus, so the rest
// of Nexa just calls methods and gets callbacks.
//
// Every privileged call can take as long as the user takes to answer polkit's password prompt, so
// nothing here is synchronous and the timeouts are generous. If the helper is not installed the
// calls fail with a message that says how to install it, rather than hanging.
class HelperClient : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<void>)>;
    using TextDone = std::function<void(Result<QString>)>;
    using FlagDone = std::function<void(Result<bool>)>;
    using JobOutput = std::function<void(quint8 stream, const QByteArray &data)>;
    using JobFinished = std::function<void(int exitCode, const QString &status)>;

    explicit HelperClient(QObject *parent = nullptr);
    ~HelperClient() override;

    // Whether the helper's D-Bus activation file is on disk. A cheap check that does not start
    // anything: Nexa uses it to decide what it can do, and only activates the root daemon when a
    // privileged action is actually asked for.
    static bool isInstalled();
    static QString installCommand();

    // Starts the helper if needed and reports its version, or why it could not be reached.
    void probe(TextDone done);

    // Whether this login has already given the helper its password, so the GUI can say whether the
    // next action will ask for one. Asking does not prompt for anything.
    void isAuthorized(FlagDone done);
    // Gives up this login's authorisation: the next action asks for the password again. Logging out
    // does the same.
    void forgetAuthorization(Done done);
    // What probe() last found: empty while it has not run or the helper is fine.
    QString unavailableReason() const { return unavailable_; }
    bool isReady() const { return ready_; }

    // Runs argv as root. `started` gets the job id (or the reason it could not start), then
    // `output` is called for each chunk and `finished` exactly once.
    void runCommand(const QStringList &argv, const QString &cwd, const QStringList &env, int timeoutMs,
                    JobOutput output, JobFinished finished, std::function<void(Result<quint64>)> started);
    void cancelJob(quint64 jobId);
    void cancelAllJobs();
    bool hasJobs() const { return !jobs_.empty(); }

    // Keyboard and pointer. typeText reports the characters that could not be typed.
    void typeText(const QString &text, TextDone done);
    void pressKeys(const QList<int> &codes, Done done);
    void mouseMove(int dx, int dy, Done done);
    void mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done);
    void mouseButton(int button, bool down, Done done);
    void mouseScroll(int dx, int dy, Done done);

    // Hardcore mode: the laptop's own keyboards and pointers off (the names of those held), and back. Locking again
    // while locked is the heartbeat the helper needs every few seconds.
    void lockInput(std::function<void(Result<QStringList>)> done);
    void unlockInput(Done done);

signals:
    void readyChanged(bool ready);
    // The helper ended the lock by itself (Nexa missed its heartbeat, its login left the screen).
    void inputUnlocked(const QString &reason);
    // The helper left the bus (an idle exit, a stop, a crash).
    void helperGone();

private:
    struct Job {
        JobOutput output;
        JobFinished finished;
    };

    // The reply shapes the contract uses: nothing, a string, a flag, and RunCommand's job id.
    void awaitVoid(const QDBusPendingCall &call, Done done);
    void awaitString(const QDBusPendingCall &call, TextDone done);
    void awaitFlag(const QDBusPendingCall &call, FlagDone done);
    void noteOutcome(bool failed, const Error &error);

    void onOutputChunk(qulonglong jobId, uchar stream, const QByteArray &data);
    void onFinished(qulonglong jobId, int exitCode, const QString &status);
    void onHelperGone();
    // Turns a D-Bus failure into a Nexa error, naming the install script when nothing is there.
    Error errorFrom(const QDBusError &error) const;
    void setReady(bool ready, const QString &reason);

    std::unique_ptr<HelperProxy> proxy_;
    QDBusServiceWatcher *watcher_{nullptr};
    std::map<quint64, Job> jobs_;
    QString unavailable_;
    bool ready_{false};
};

} // namespace nexa
