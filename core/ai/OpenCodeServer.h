#pragma once

#include "core/Result.h"
#include "core/ai/OpenCodeWire.h"
#include "core/net/SseParser.h"

#include <QList>
#include <QNetworkRequest>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>

class QNetworkReply;

namespace nexa {

class HttpClient;

// Facade over the opencode server Nexa talks to: its own `opencode serve` — on loopback, on a port
// opencode picks, behind a password made for this start that only the child's environment holds,
// in a folder of its own, with opencode's edit, bash and web permissions denied — or, given a URL,
// one the user runs. Ready once it says where it listens, answers /global/health and has opened the
// event stream every turn reads; that stream is shared and reopened when it drops.
class OpenCodeServer : public QObject {
    Q_OBJECT
public:
    // `program` empty and no `attachUrl`: opencode is not installed. `workingDir` empty:
    // ~/.cache/nexa/opencode.
    OpenCodeServer(HttpClient *http, QString program, QUrl attachUrl, QString attachPassword,
                   QString workingDir = {}, QObject *parent = nullptr);
    ~OpenCodeServer() override;

    // `done` runs once the server is ready, or with why it is not; calls made meanwhile wait together.
    void ensureStarted(std::function<void(Result<void>)> done);
    QNetworkRequest request(const QString &path) const;
    bool isReady() const { return state_ == State::Ready; }
    void setStartTimeoutMs(int ms) { startTimeoutMs_ = ms; }
    qint64 processId() const;
    HttpClient *http() const { return http_; }

signals:
    void event(const nexa::OpenCodeEvent &event);
    void stopped(const nexa::Error &error);     // a ready server went away

private:
    enum class State { Idle, Starting, Ready };

    void startProcess();
    void checkHealth();
    void openEvents();
    void readEvents();
    void leaveMcpOut(std::function<void()> done);
    void disconnectMcp(const QStringList &names, std::function<void()> done);
    void becomeReady();
    void finishStart(const Result<void> &result);
    void stopProcess();

    HttpClient *http_;
    QString program_;
    QUrl attachUrl_;
    QString workingDir_;
    QUrl base_;
    QString password_;
    QProcess process_;
    QByteArray output_;
    QString stderrTail_;
    QTimer deadline_;
    QTimer retry_;
    QNetworkReply *events_{nullptr};
    SseParser parser_;
    State state_{State::Idle};
    int startTimeoutMs_{45'000};      // start, health, the event stream and leaving MCP out (~13 s measured)
    QList<std::function<void(Result<void>)>> waiting_;
};

} // namespace nexa
