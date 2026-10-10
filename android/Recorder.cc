#include "Recorder.h"

#include "voice/capture/AudioConverter.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kMaxBytes = Recorder::kMaxSeconds * AudioConverter::kOutputRate * 2;

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

void Recorder::readMore() {
    if (!device_) return;
    pcm_.append(converter_->convert(device_->readAll()));
    if (pcm_.size() >= kMaxBytes) {
        pcm_.truncate(kMaxBytes);
        finish(true);
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
    if (keep && speech) emit recorded(pcm);
}

} // namespace nexa
