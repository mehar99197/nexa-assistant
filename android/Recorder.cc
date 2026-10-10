#include "Recorder.h"

#include "voice/capture/AudioConverter.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kMaxBytes = Recorder::kMaxSeconds * AudioConverter::kOutputRate * 2;
constexpr double kSpeechRms = 0.015;    // of full scale: quiet speech, above room noise

// The loudness of a chunk of 16-bit samples, 0 … 1.
double rmsOf(const QByteArray &pcm) {
    const qsizetype count = pcm.size() / 2;
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (qsizetype i = 0; i < count; ++i) {
        qint16 sample = 0;
        std::memcpy(&sample, pcm.constData() + 2 * i, sizeof sample);
        const double value = sample / 32768.0;
        sum += value * value;
    }
    return std::sqrt(sum / double(count));
}

} // namespace

Recorder::Recorder(QObject *parent) : QObject(parent) {}

Recorder::~Recorder() {
    discard();
}

void Recorder::start() {
    if (source_) return;
    const QAudioDevice input = QMediaDevices::defaultAudioInput();
    if (input.isNull()) {
        emit failed(tr("This phone has no microphone Nexa can use."));
        return;
    }
    QAudioFormat format;
    format.setSampleRate(AudioConverter::kOutputRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!input.isFormatSupported(format)) format = input.preferredFormat();
    converter_ = std::make_unique<AudioConverter>(format);
    source_ = std::make_unique<QAudioSource>(input, format);
    pcm_.clear();
    stopOnSilence_ = false;
    heardSpeech_ = false;
    lastSpeechMs_ = 0;
    clock_.start();
    device_ = source_->start();
    if (!device_ || source_->error() != QAudio::NoError) {
        source_.reset();
        converter_.reset();
        emit failed(tr("The microphone could not be opened."));
        return;
    }
    connect(device_, &QIODevice::readyRead, this, &Recorder::readMore);
}

void Recorder::stop() {
    finish(true);
}

void Recorder::discard() {
    finish(false);
}

void Recorder::setStopOnSilence(bool stop) {
    stopOnSilence_ = stop;
}

void Recorder::readMore() {
    if (!device_) return;
    const QByteArray chunk = converter_->convert(device_->readAll());
    pcm_.append(chunk);
    const double rms = rmsOf(chunk);
    emit level(std::min(1.0, rms * 6));
    if (rms > kSpeechRms) {
        heardSpeech_ = true;
        lastSpeechMs_ = clock_.elapsed();
    }
    if (pcm_.size() >= kMaxBytes) {
        pcm_.truncate(kMaxBytes);
        finish(true);
        return;
    }
    if (!stopOnSilence_) return;
    if (heardSpeech_ && clock_.elapsed() - lastSpeechMs_ > kSilenceMs) {
        finish(true);
    } else if (!heardSpeech_ && clock_.elapsed() > kNoSpeechMs) {
        finish(false);
    }
}

void Recorder::finish(bool keep) {
    if (!source_) return;
    source_->stop();
    if (device_) {
        disconnect(device_, nullptr, this, nullptr);
        pcm_.append(converter_->convert(device_->readAll()));
        device_ = nullptr;
    }
    source_.reset();
    converter_.reset();
    QByteArray pcm;
    pcm.swap(pcm_);
    if (pcm.size() > kMaxBytes) pcm.truncate(kMaxBytes);
    const bool speech = pcm.size() >= kMinMs * AudioConverter::kOutputRate * 2 / 1000;
    emit level(0.0);
    if (keep && speech) emit recorded(pcm);
    emit stopped();
}

} // namespace nexa
