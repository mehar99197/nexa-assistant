#pragma once

#include "voice/capture/AudioConverter.h"
#include "voice/capture/IAudioInput.h"

#include <memory>
#include <optional>

class QAudioSource;
class QIODevice;

namespace nexa {

// The default microphone through QAudioSource (IAudioInput). Asks for 16 kHz mono Int16 and
// converts whatever format the device actually negotiates. The device is open only between
// start() and stop().
class AudioCapture : public IAudioInput {
    Q_OBJECT
public:
    explicit AudioCapture(QObject *parent = nullptr);
    ~AudioCapture() override;

    QString name() const override;
    Result<void> start() override;
    void stop() override;
    bool isActive() const override { return source_ != nullptr; }

private:
    void readAvailable();

    std::unique_ptr<QAudioSource> source_;
    QIODevice *io_{nullptr};
    std::optional<AudioConverter> converter_;
    QString deviceName_;
};

} // namespace nexa
