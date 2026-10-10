#pragma once

#include "core/Result.h"
#include "voice/Wav.h"

#include <QObject>
#include <QTimer>

#include <deque>
#include <memory>

class QAudioSink;
class QIODevice;

namespace nexa {

// Plays PCM clips back to back on the default output (QAudioSink in push mode): a timer moves
// queued audio into the sink as it frees space, so stop() silences at once. A clip in another
// format waits for the sink to drain and reopens it. The device is released shortly after the
// queue has played out.
class PcmPlayer : public QObject {
    Q_OBJECT
public:
    explicit PcmPlayer(QObject *parent = nullptr);
    ~PcmPlayer() override;

    void enqueue(PcmAudio clip);
    void stop();                            // silences now and drops the queue; no finished()
    bool isPlaying() const { return playing_; }
    void setVolume(double volume);          // 0 … 1

signals:
    void started();
    void finished();                        // everything queued has been heard
    void failed(const nexa::Error &error);

private:
    void pump();
    bool openSink(const PcmAudio &format);
    void closeSink();
    bool drained() const;
    void fail(const QString &message);

    std::unique_ptr<QAudioSink> sink_;
    QIODevice *io_{nullptr};
    PcmAudio sinkFormat_;                   // format of the open sink (samples unused)
    std::deque<PcmAudio> queue_;
    QByteArray current_;
    qsizetype offset_{0};
    qint64 writtenUs_{0};                   // audio handed to the sink since it opened
    QTimer pumpTimer_;
    QTimer releaseTimer_;
    double volume_{1.0};
    bool playing_{false};
};

} // namespace nexa
