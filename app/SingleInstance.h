#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QLocalServer;

namespace nexa {

// One Nexa per user. The first copy listens on a socket in $XDG_RUNTIME_DIR, a directory only the
// user can enter; a later copy hands over what it was started to do — show the window, or the
// requests on its command line — and exits. A launcher or hotkey that starts Nexa again then brings
// the running one forward, instead of a second copy with its own microphone, helper and web server.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(const QString &name, QObject *parent = nullptr);
    ~SingleInstance() override;

    // True when a running copy took the message: this one should exit.
    bool handOver(const QJsonObject &message);
    // Become the running copy and listen for later ones.
    bool listen();
    QString path() const { return path_; }

signals:
    void messageReceived(const QJsonObject &message);

private:
    void accept();

    QString path_;
    QLocalServer *server_{nullptr};
};

} // namespace nexa
