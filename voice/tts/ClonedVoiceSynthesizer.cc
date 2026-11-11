#include "voice/tts/ClonedVoiceSynthesizer.h"

#include "core/Logging.h"
#include "voice/wake/SherpaRuntime.h"

#include <QPointer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kFlowSteps = 4;           // the distilled model's own

int32_t progress(const float *, int32_t, float, void *arg) {
    return static_cast<std::atomic_bool *>(arg)->load() ? 0 : 1;      // 0 stops the generation
}

} // namespace

// The model and the generations, on the worker thread: loads ZipVoice once, phonemizes the sample once, then
// turns each sentence into audio in the sample's voice.
class ClonedVoiceSynthesizer::Worker : public QObject {
public:
    Worker(SherpaRuntime *sherpa, VoiceSample sample, Phonemizer phonemizer, double rate)
        : sherpa_(sherpa), sample_(std::move(sample)), phonemizer_(std::move(phonemizer)),
          speed_(float(std::pow(1.3, std::clamp(rate, -1.0, 1.0)))) {}

    ~Worker() override {
        if (tts_) sherpa_->destroyOfflineTts(tts_);
    }

    Result<void> load() {
        if (tts_) return {};
        if (!sherpa_ || !sherpa_->canCloneVoice()) {
            return Error{ErrorCode::NotFound, u"ZipVoice is not installed — run scripts/download-sherpa.sh."_s};
        }
        if (!phonemizer_.available()) return Error{ErrorCode::NotFound, phonemizer_.problem()};
        const Result<QStringList> words = phonemizer_.tokens(sample_.text);
        if (!words) return words.error();
        referenceTokens_ = words.value();
        reference_.resize(size_t(sample_.audio.frameCount()));
        const auto *samples = reinterpret_cast<const qint16 *>(sample_.audio.samples.constData());
        const int channels = std::max(1, sample_.audio.channels);
        for (size_t i = 0; i < reference_.size(); ++i) reference_[i] = samples[i * size_t(channels)] / 32768.0f;
        referenceText_ = sample_.text.toUtf8();

        const QString dir = sherpa_->zipVoiceDir();
        paths_ = {(dir + u"/tokens.txt"_s).toUtf8(),   (dir + u"/encoder.int8.onnx"_s).toUtf8(),
                  (dir + u"/decoder.int8.onnx"_s).toUtf8(), (dir + u"/vocos_24khz.onnx"_s).toUtf8(),
                  (dir + u"/lexicon.txt"_s).toUtf8()};
        SherpaOnnxOfflineTtsConfig config;
        std::memset(&config, 0, sizeof(config));
        config.model.zipvoice.tokens = paths_[0].constData();
        config.model.zipvoice.encoder = paths_[1].constData();
        config.model.zipvoice.decoder = paths_[2].constData();
        config.model.zipvoice.vocoder = paths_[3].constData();
        config.model.zipvoice.lexicon = paths_[4].constData();
        config.model.zipvoice.feat_scale = 0.1f;
        config.model.zipvoice.t_shift = 0.5f;
        config.model.zipvoice.target_rms = 0.1f;
        config.model.zipvoice.guidance_scale = 1.0f;
        config.model.num_threads = std::clamp(QThread::idealThreadCount() - 1, 1, 4);
        config.model.provider = "cpu";
        config.max_num_sentences = 1;
        config.silence_scale = 0.2f;
        tts_ = sherpa_->createOfflineTts(&config);
        if (!tts_) return Error{ErrorCode::Io, u"ZipVoice could not be loaded (its files may be damaged — run scripts/download-sherpa.sh again)."_s};
        qCDebug(lcVoice) << "ZipVoice loaded; the sample is" << sample_.audio.durationMs() << "ms,"
                         << referenceTokens_.size() << "tokens";
        return {};
    }

    Result<PcmAudio> generate(const QString &text, std::atomic_bool *cancelled) {
        if (const Result<void> loaded = load(); !loaded) return loaded.error();
        if (cancelled->load()) return Error{ErrorCode::Cancelled, u"Cancelled"_s};
        const Result<QStringList> words = phonemizer_.tokens(text);
        if (!words) return words.error();
        // The tokens of two sentences, flattened: the sample's words first, then the sentence to say.
        std::vector<QByteArray> utf8;
        for (const QString &token : referenceTokens_) utf8.push_back(token.toUtf8());
        for (const QString &token : words.value()) utf8.push_back(token.toUtf8());
        std::vector<const char *> tokens;
        for (const QByteArray &token : utf8) tokens.push_back(token.constData());
        const int32_t lengths[2] = {int32_t(referenceTokens_.size()), int32_t(words.value().size())};
        const QByteArray textUtf8 = text.simplified().toUtf8();

        SherpaOnnxGenerationConfig config;
        std::memset(&config, 0, sizeof(config));
        config.speed = speed_;
        config.silence_scale = 0.2f;
        config.num_steps = kFlowSteps;
        config.reference_audio = reference_.data();
        config.reference_audio_len = int32_t(reference_.size());
        config.reference_sample_rate = sample_.audio.sampleRate;
        config.reference_text = referenceText_.constData();
        config.tokens = tokens.data();
        config.tokens_lens = lengths;
        config.tokens_num_sentences = 2;
        const SherpaOnnxGeneratedAudio *audio =
            sherpa_->offlineTtsGenerateWithConfig(tts_, textUtf8.constData(), &config, progress, cancelled);
        if (cancelled->load()) {
            if (audio) sherpa_->destroyGeneratedAudio(audio);
            return Error{ErrorCode::Cancelled, u"Cancelled"_s};
        }
        if (!audio || audio->n <= 0 || !audio->samples) {
            if (audio) sherpa_->destroyGeneratedAudio(audio);
            return Error{ErrorCode::Io, u"ZipVoice produced no audio for that sentence."_s};
        }
        PcmAudio clip{{}, audio->sample_rate, 1};
        clip.samples.resize(qsizetype(audio->n) * 2);
        auto *out = reinterpret_cast<qint16 *>(clip.samples.data());
        for (int32_t i = 0; i < audio->n; ++i) {
            out[i] = qint16(std::lround(std::clamp(audio->samples[i], -1.0f, 1.0f) * 32767.0f));
        }
        sherpa_->destroyGeneratedAudio(audio);
        return clip;
    }

private:
    SherpaRuntime *sherpa_;
    VoiceSample sample_;
    Phonemizer phonemizer_;
    float speed_;
    const SherpaOnnxOfflineTts *tts_{nullptr};
    std::vector<QByteArray> paths_;
    QStringList referenceTokens_;
    std::vector<float> reference_;
    QByteArray referenceText_;
};

ClonedVoiceSynthesizer::ClonedVoiceSynthesizer(SherpaRuntime *sherpa, VoiceSample sample, Phonemizer phonemizer,
                                               double rate, QObject *parent)
    : ISpeechSynthesizer(parent), sampleWords_(sample.text) {
    worker_ = new Worker(sherpa, std::move(sample), std::move(phonemizer), rate);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName(u"nexa-own-voice"_s);
    thread_.start();
}

ClonedVoiceSynthesizer::~ClonedVoiceSynthesizer() {
    cancel();
    thread_.quit();
    thread_.wait();
}

QString ClonedVoiceSynthesizer::name() const {
    return u"my voice (ZipVoice)"_s;
}

void ClonedVoiceSynthesizer::warmUp() {
    QMetaObject::invokeMethod(worker_, [worker = worker_] { (void)worker->load(); }, Qt::QueuedConnection);
}

void ClonedVoiceSynthesizer::synthesize(const QString &text, Done done) {
    if (text.simplified().isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"Nothing to say."_s});
        return;
    }
    const quint64 id = nextId_++;
    Job job{std::move(done), std::make_shared<std::atomic_bool>(false)};
    const std::shared_ptr<std::atomic_bool> cancelled = job.cancelled;
    jobs_.insert(id, std::move(job));
    const QPointer<ClonedVoiceSynthesizer> self(this);
    QMetaObject::invokeMethod(
        worker_,
        [self, worker = worker_, id, text, cancelled] {
            Result<PcmAudio> clip = cancelled->load() ? Result<PcmAudio>(Error{ErrorCode::Cancelled, u"Cancelled"_s})
                                                      : worker->generate(text, cancelled.get());
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, id, clip = std::move(clip)] {
                if (self) self->deliver(id, clip);
            }, Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ClonedVoiceSynthesizer::cancel() {
    const QHash<quint64, Job> jobs = std::exchange(jobs_, {});
    for (const Job &job : jobs) job.cancelled->store(true);
    for (const Job &job : jobs) job.done(Error{ErrorCode::Cancelled, u"Cancelled"_s});
}

void ClonedVoiceSynthesizer::deliver(quint64 id, Result<PcmAudio> clip) {
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) return;      // cancelled, and told so already
    Done done = std::move(it->done);
    jobs_.erase(it);
    done(std::move(clip));
}

} // namespace nexa
