#include "SpeechPlayer.h"

#include "voice/Wav.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

using namespace Qt::StringLiterals;

namespace nexa {

SpeechPlayer::SpeechPlayer(QObject *parent) : QObject(parent) {}

SpeechPlayer::~SpeechPlayer() {
    stop();
}

void SpeechPlayer::play(const QByteArray &wav) {
    stop();
    Result<PcmAudio> audio = wav::decode(wav);
    if (!audio) {
        emit failed(audio.error().message);
        return;
    }
    QAudioFormat format;
    format.setSampleRate(audio.value().sampleRate);
    format.setChannelCount(audio.value().channels);
    format.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull() || !output.isFormatSupported(format)) {
        emit failed(tr("This phone cannot play the reply."));
        return;
    }
    buffer_.setData(std::move(audio).value().samples);
    buffer_.open(QIODevice::ReadOnly);
    sink_ = std::make_unique<QAudioSink>(output, format);
    connect(sink_.get(), &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::IdleState || state == QAudio::StoppedState) stop();
    });
    sink_->start(&buffer_);
    emit started();
}

void SpeechPlayer::stop() {
    if (!sink_) return;
    std::unique_ptr<QAudioSink> sink = std::move(sink_);
    sink->stop();
    buffer_.close();
    emit finished();
}

} // namespace nexa
