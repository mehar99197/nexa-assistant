#pragma once

#include "voice/capture/IAudioInput.h"

#include <QByteArray>
#include <QTimer>

namespace nexa {

// A recording played into Nexa as if the microphone heard it (IAudioInput), in real time, 100 ms at a time —
// what `--hear <wav>` runs the whole wake → request → reply path with. Silence follows the recording, so the
// speech detector can hear it end and a follow-up window can close.
class RecordedAudioInput : public IAudioInput {
    Q_OBJECT
public:
    explicit RecordedAudioInput(QObject *parent = nullptr);

    QString name() const override { return QStringLiteral("a recording"); }
    Result<void> start() override;
    void stop() override;
    bool isActive() const override { return active_; }

    // 16 kHz mono Int16, played after whatever is still playing.
    void play(const QByteArray &pcm16k);
    // A recording, or the silence after it, still to come.
    bool playing() const { return !queue_.isEmpty() || silenceLeft_ > 0; }

signals:
    void drained();         // 3 s of silence after the last recording

private:
    void tick();

    QTimer timer_;
    QByteArray queue_;
    bool active_{false};
    int silenceLeft_{0};    // ticks of silence still owed after the last recording
};

} // namespace nexa
