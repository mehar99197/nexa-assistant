#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>

#include <memory>

class QAudioSource;
class QIODevice;

namespace nexa {

class AudioConverter;

// The microphone while Talk is held: whatever the phone records in, converted to the 16 kHz mono
// 16-bit PCM the web remote takes, at most a minute. Nothing is recorded at any other time. After
// a tap (setStopOnSilence) it ends by itself at the pause after the request, or gives up when
// nothing is said at all.
class Recorder : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxSeconds = 60;
    static constexpr int kMinMs = 250;          // shorter is a tap, not speech
    static constexpr int kSilenceMs = 1200;     // after speech, this much quiet ends a tapped recording
    static constexpr int kNoSpeechMs = 6000;    // a tapped recording with nothing said at all

    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    bool isRecording() const { return source_ != nullptr; }
    void start();
    void stop();        // recorded() follows, unless it was only a tap
    void discard();
    void setStopOnSilence(bool stop);

signals:
    void recorded(const QByteArray &pcm16kMonoInt16);
    void stopped();                         // after recorded(), or alone when there was nothing to send
    void level(double level);               // 0 … 1 while recording
    void failed(const QString &reason);

private:
    void readMore();
    void finish(bool keep);

    std::unique_ptr<QAudioSource> source_;
    std::unique_ptr<AudioConverter> converter_;
    QIODevice *device_{nullptr};
    QByteArray pcm_;
    QElapsedTimer clock_;
    bool stopOnSilence_{false};
    bool heardSpeech_{false};
    qint64 lastSpeechMs_{0};
};

} // namespace nexa
