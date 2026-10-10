#pragma once

#include <QByteArray>
#include <QObject>

#include <memory>

class QAudioSource;
class QIODevice;

namespace nexa {

class AudioConverter;

// The microphone while Talk is held: whatever the phone records in, converted to the 16 kHz mono
// 16-bit PCM the web remote takes, at most a minute. Nothing is recorded at any other time.
class Recorder : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxSeconds = 60;
    static constexpr int kMinMs = 250;      // shorter is a tap, not speech

    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    bool isRecording() const { return source_ != nullptr; }
    void start();
    void stop();        // recorded() follows, unless it was only a tap
    void discard();

signals:
    void recorded(const QByteArray &pcm16kMonoInt16);
    void failed(const QString &reason);

private:
    void readMore();
    void finish(bool keep);

    std::unique_ptr<QAudioSource> source_;
    std::unique_ptr<AudioConverter> converter_;
    QIODevice *device_{nullptr};
    QByteArray pcm_;
};

} // namespace nexa
