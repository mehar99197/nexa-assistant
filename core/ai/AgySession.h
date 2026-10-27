#pragma once

#include "core/Result.h"
#include "core/ai/AgyEvents.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTimer>

namespace nexa {

// One warm agy process (`--input-format stream-json`), which keeps the conversation between turns, so
// a turn costs the model's time and not agy's start. One turn at a time; the process lives in its
// own process group, in an empty folder of its own, and goes after ten idle minutes. A session ends
// with its process: a new one is a new session.
//
// agy runs with a home folder of Nexa's own (`<dir>/home`): its settings there deny agy's own tools —
// print mode already refuses the acting ones, but would read any file or page unasked (measured with
// agy 1.4.8), outside Nexa's audit. The sign-in is in the system keyring, so it carries over; the
// user's own agy and its settings are left alone.
class AgySession : public QObject {
    Q_OBJECT
public:
    AgySession(QString program, QString model, QString workingDir, QObject *parent = nullptr);
    ~AgySession() override;

    void start();                       // idempotent
    void send(const QString &text);     // one turn
    void stop();
    bool isUsable() const { return !broken_; }
    bool isBusy() const { return busy_; }
    int turnsTaken() const { return turns_; }
    const QString &model() const { return model_; }

    // Prepares `dir` for agy — `dir/home` with the deny rules (keeping whatever agy wrote there
    // itself), `dir/work` as the empty workspace — and returns the environment agy runs in.
    static QProcessEnvironment prepare(const QString &dir);
    static QString workspace(const QString &dir);
    static const QStringList &deniedTools();

signals:
    void text(const QString &delta);
    void toolStep(const QString &tool, bool allowed);
    void turnFinished(const nexa::AgyEvent &result);
    void turnFailed(const nexa::Error &error);

private:
    void readEvents();
    void ended(const Error &error);

    QString program_;
    QString model_;
    QString workingDir_;
    QProcess process_;
    QTimer idle_;
    QByteArray buffer_;
    QString stderrTail_;
    bool started_{false};
    bool broken_{false};
    bool busy_{false};
    int turns_{0};
};

} // namespace nexa
