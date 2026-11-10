#pragma once

#include "system/net/TunnelLog.h"

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

namespace nexa {

// One public tunnel to Nexa's tunnel door (Facade over a child process — cloudflared or ngrok, told apart by the
// Spec it is started with, a Strategy for reading its log). It starts the program, reads its log for the public
// address and the connection, restarts it with growing pauses when it stops by itself, and stops trying when the log
// says only the owner can fix it (no certificate, no authtoken, no such tunnel). The program keeps its own
// credentials; Nexa never reads them.
class TunnelProcess : public QObject {
    Q_OBJECT
public:
    enum class State { Off, Missing, Starting, Running, Failed };

    struct Spec {
        QString program;                                        // the path found, or empty: not installed
        QStringList arguments;
        std::function<tunnellog::LineEvent(const QString &)> read;
        QString fixedUrl;                                       // the address when it is known before (a named tunnel)
    };

    explicit TunnelProcess(QString name, QObject *parent = nullptr);
    ~TunnelProcess() override;

    void start(Spec spec);          // again with the same Spec while running: nothing happens
    // The program is asked to end; `wait`: until it has (Nexa going away, so none is left running), otherwise it ends
    // on its own while the window goes on.
    void stop(bool wait = false);
    State state() const { return state_; }
    QString url() const { return state_ == State::Running ? url_ : QString(); }
    QString problem() const { return problem_; }
    QString name() const { return name_; }

signals:
    void changed();

private:
    void launch();
    void onLine(const QString &line);
    void onFinished();
    void setState(State state);

    QString name_;
    Spec spec_;
    std::unique_ptr<QProcess> process_;
    QTimer restart_;
    int failures_{0};
    bool fatal_{false};
    bool stopping_{false};
    State state_{State::Off};
    QString url_;
    QString problem_;
    QByteArray pending_;            // a line not ended yet
};

} // namespace nexa
