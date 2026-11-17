#pragma once

#include "system/commands/LocalCommandRunner.h"

#include <QObject>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace nexa {

class HelperClient;

// Runs commands as root (through nexa-helper on Linux or Win32 UAC ShellExecuteExW "runas"
// on Windows), and reports them in the same CommandOutput shape LocalCommandRunner uses.
class RootCommandRunner : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<CommandOutput>)>;

    explicit RootCommandRunner(HelperClient *helper, QObject *parent = nullptr);
    ~RootCommandRunner() override;

    // `done` runs exactly once. The first call of a session waits for polkit / UAC approval.
    void run(const QString &command, const QString &workingDirectory, int timeoutMs, Done done);
    void cancelAll();

private:
    struct Job {
        Done done;
        OutputCollector output;
        QElapsedTimer clock;
        quint64 id{0};          // set once the helper has accepted the job
    };

#ifdef Q_OS_WIN
    struct WinJob;
    std::vector<std::shared_ptr<WinJob>> winJobs_;
#endif
    HelperClient *helper_;
    std::map<quint64, std::shared_ptr<Job>> jobs_;
};

} // namespace nexa
