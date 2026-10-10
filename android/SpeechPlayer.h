#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QObject>

#include <memory>

class QAudioSink;

namespace nexa {

// Plays the spoken reply Nexa rendered: the WAV goes through Nexa's own decoder and out of
// QAudioSink, which behaves the same on the desktop and on the phone — no media framework, and
// nothing that would have to trust the certificate on its own.
class SpeechPlayer : public QObject {
    Q_OBJECT
public:
    explicit SpeechPlayer(QObject *parent = nullptr);
    ~SpeechPlayer() override;

    void play(const QByteArray &wav);
    void stop();

signals:
    void failed(const QString &reason);

private:
    std::unique_ptr<QAudioSink> sink_;
    QBuffer buffer_;
};

} // namespace nexa
