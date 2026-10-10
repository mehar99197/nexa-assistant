#include "voice/capture/AudioCapture.h"

#include "core/Logging.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>

using namespace Qt::StringLiterals;

namespace nexa {

AudioCapture::AudioCapture(QObject *parent) : IAudioInput(parent) {}

AudioCapture::~AudioCapture() {
    stop();
}

QString AudioCapture::name() const {
    return deviceName_.isEmpty() ? u"Microphone"_s : deviceName_;
}

Result<void> AudioCapture::start() {
    if (source_) return {};
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) return Error{ErrorCode::NotFound, u"No microphone found."_s};

    QAudioFormat wanted;
    wanted.setSampleRate(AudioConverter::kOutputRate);
    wanted.setChannelCount(1);
    wanted.setSampleFormat(QAudioFormat::Int16);
    const QAudioFormat format = device.isFormatSupported(wanted) ? wanted : device.preferredFormat();

    source_ = std::make_unique<QAudioSource>(device, format);
    source_->setBufferSize(format.bytesForDuration(100'000));
    io_ = source_->start();
    if (!io_ || source_->error() != QAudio::NoError) {
        source_.reset();
        io_ = nullptr;
        return Error{ErrorCode::Io, u"Cannot open the microphone (%1)."_s.arg(device.description())};
    }
    // The backend may still adjust the format when the stream opens.
    converter_.emplace(source_->format());
    deviceName_ = device.description();
    qCDebug(lcVoice) << "microphone:" << deviceName_ << source_->format() << "passthrough"
                     << converter_->isPassthrough();

    connect(io_, &QIODevice::readyRead, this, &AudioCapture::readAvailable);
    connect(source_.get(), &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (state != QAudio::StoppedState || !source_ || source_->error() == QAudio::NoError) return;
        const QString reason = source_->error() == QAudio::OpenError ? u"cannot be opened"_s : u"stopped working"_s;
        stop();
        emit failed(Error{ErrorCode::Io, u"The microphone %1."_s.arg(reason)});
    });
    return {};
}

void AudioCapture::stop() {
    if (!source_) return;
    source_->disconnect(this);
    if (io_) io_->disconnect(this);
    source_->stop();
    source_.reset();
    io_ = nullptr;
    converter_.reset();
}

void AudioCapture::readAvailable() {
    if (!io_ || !converter_) return;
    const QByteArray pcm = converter_->convert(io_->readAll());
    if (!pcm.isEmpty()) emit audio(pcm);
}

} // namespace nexa
