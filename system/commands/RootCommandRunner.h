#pragma once

#include "system/commands/LocalCommandRunner.h"

#include <QObject>

#include <functional>
#include <map>

namespace nexa {

class HelperClient;

// Runs commands as root through nexa-helper, and reports them in exactly the shape
// LocalCommandRunner uses — so a tool does not care which of the two ran its command.
//
// The helper executes argv directly, so a command line that needs shell syntax is wrapped in
// `bash -c` here, by the same rules as the unprivileged runner.
class RootCommandRunner : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<CommandOutput>)>;

    explicit RootCommandRunner(HelperClient *helper, QObject *parent = nullptr);

    // `done` runs exactly once. The first call of a session waits for polkit's password prompt.
    void run(const QString &command, const QString &workingDirectory, int timeoutMs, Done done);
    void cancelAll();

private:
    struct Job {
        Done done;
        OutputCollector output;
        QElapsedTimer clock;
        quint64 id{0};          // set once the helper has accepted the job
    };

    HelperClient *helper_;
    std::map<quint64, std::shared_ptr<Job>> jobs_;
};

} // namespace nexa
