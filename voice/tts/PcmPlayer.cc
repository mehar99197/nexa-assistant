#include "voice/tts/PcmPlayer.h"

#include "core/Logging.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QMediaDevices>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kPumpIntervalMs = 15;
constexpr int kBufferMs = 200;
constexpr int kReleaseAfterMs = 3'000;      // keep the device a moment for the next sentence

QAudioFormat audioFormat(const PcmAudio &clip) {
    QAudioFormat format;
    format.setSampleRate(clip.sampleRate);
    format.setChannelCount(clip.channels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

} // namespace

PcmPlayer::PcmPlayer(QObject *parent) : QObject(parent) {
    pumpTimer_.setInterval(kPumpIntervalMs);
    pumpTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pumpTimer_, &QTimer::timeout, this, &PcmPlayer::pump);
    releaseTimer_.setSingleShot(true);
    releaseTimer_.setInterval(kReleaseAfterMs);
    connect(&releaseTimer_, &QTimer::timeout, this, &PcmPlayer::closeSink);
}

PcmPlayer::~PcmPlayer() {
    closeSink();
}

void PcmPlayer::enqueue(PcmAudio clip) {
    if (clip.samples.isEmpty() || clip.sampleRate <= 0 || clip.channels <= 0) return;
    releaseTimer_.stop();
    queue_.push_back(std::move(clip));
    if (!playing_) {
        playing_ = true;
        emit started();
    }
    if (!pumpTimer_.isActive()) pumpTimer_.start();
    pump();
}

void PcmPlayer::stop() {
    queue_.clear();
    current_.clear();
    offset_ = 0;
    pumpTimer_.stop();
    playing_ = false;
    closeSink();    // drops what the sink still buffers
}

void PcmPlayer::setVolume(double volume) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    if (sink_) sink_->setVolume(volume_);
}

void PcmPlayer::pump() {
    for (;;) {
        if (offset_ >= current_.size()) {
            if (queue_.empty()) break;
            if (sink_ && !queue_.front().sameFormat(sinkFormat_)) {
                if (!drained()) return;     // let the previous format play out first
                closeSink();
            }
            if (!sink_ && !openSink(queue_.front())) return;
            current_ = std::move(queue_.front().samples);
            queue_.pop_front();
            offset_ = 0;
        }
        const qsizetype frameBytes = 2 * sinkFormat_.channels;
        qsizetype chunk = std::min<qsizetype>(sink_->bytesFree(), current_.size() - offset_);
        chunk -= chunk % frameBytes;
        if (chunk <= 0) return;
        const qint64 written = io_->write(current_.constData() + offset_, chunk);
        if (written <= 0) return;
        offset_ += written;
        writtenUs_ += written * 1'000'000 / (qint64(sinkFormat_.sampleRate) * frameBytes);
    }

    // Everything is in the sink: finished once it has played out.
    if (!drained()) return;
    pumpTimer_.stop();
    current_.clear();
    offset_ = 0;
    if (playing_) {
        playing_ = false;
        releaseTimer_.start();
        emit finished();
    }
}

bool PcmPlayer::openSink(const PcmAudio &format) {
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        fail(u"No audio output device found."_s);
        return false;
    }
    sink_ = std::make_unique<QAudioSink>(device, audioFormat(format));
    sink_->setBufferSize(audioFormat(format).bytesForDuration(kBufferMs * 1000));
    sink_->setVolume(volume_);
    io_ = sink_->start();
    if (!io_ || sink_->error() != QAudio::NoError) {
        sink_.reset();
        io_ = nullptr;
        fail(u"Cannot open the audio output (%1)."_s.arg(device.description()));
        return false;
    }
    sinkFormat_ = PcmAudio{.sampleRate = format.sampleRate, .channels = format.channels};
    writtenUs_ = 0;
    qCDebug(lcVoice) << "audio output:" << device.description() << format.sampleRate << "Hz" << format.channels
                     << "ch";
    return true;
}

void PcmPlayer::closeSink() {
    releaseTimer_.stop();
    if (!sink_) return;
    sink_->stop();
    sink_.reset();
    io_ = nullptr;
    writtenUs_ = 0;
}

bool PcmPlayer::drained() const {
    if (!sink_) return true;
    // Idle = the sink ran out of data; processedUSecs covers backends that stay Active.
    return sink_->state() == QAudio::IdleState || sink_->processedUSecs() >= writtenUs_;
}

void PcmPlayer::fail(const QString &message) {
    stop();
    emit failed(Error{ErrorCode::Io, message});
}

} // namespace nexa
