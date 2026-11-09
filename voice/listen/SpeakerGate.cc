#include "voice/listen/SpeakerGate.h"

#include "core/Logging.h"
#include "voice/wake/SherpaRuntime.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>

#include <cstring>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kShortest = 16'000 * 2 * 8 / 10;    // 0.8 s: shorter is let through, not judged
// A request may be the owner's words under a TV that talks the whole time: its best stretch of 1.5 s is the owner's own.
constexpr qsizetype kWindow = 16'000 * 2 * 3 / 2;
constexpr qsizetype kHop = 16'000 * 2 * 3 / 4;

} // namespace

// Makes embeddings on the gate's own thread; the extractor is made there on first use and freed there.
class SpeakerWorker : public QObject {
public:
    explicit SpeakerWorker(SherpaRuntime *runtime) : runtime_(runtime) {}
    ~SpeakerWorker() override {
        if (extractor_) runtime_->destroySpeakerExtractor(extractor_);
    }

    std::optional<QList<float>> embed(const QByteArray &pcm16k) {
        if (!runtime_->canTellSpeakers()) return std::nullopt;
        if (!extractor_) {
            const QByteArray model = runtime_->speakerModel().toUtf8();
            SherpaOnnxSpeakerEmbeddingExtractorConfig config;
            std::memset(&config, 0, sizeof(config));
            config.model = model.constData();
            config.num_threads = 2;
            config.provider = "cpu";
            extractor_ = runtime_->createSpeakerExtractor(&config);
            if (!extractor_) {
                qCWarning(lcVoice) << "cannot load the speaker model" << runtime_->speakerModel();
                return std::nullopt;
            }
        }
        QElapsedTimer timer;
        timer.start();
        std::vector<float> samples(size_t(pcm16k.size() / 2));
        const auto *input = reinterpret_cast<const qint16 *>(pcm16k.constData());
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = float(input[i]) / 32768.0f;
        const SherpaOnnxOnlineStream *stream = runtime_->speakerCreateStream(extractor_);
        runtime_->acceptWaveform(stream, 16'000, samples.data(), int32_t(samples.size()));
        runtime_->inputFinished(stream);
        std::optional<QList<float>> embedding;
        if (runtime_->speakerIsReady(extractor_, stream)) {
            if (const float *vector = runtime_->speakerCompute(extractor_, stream)) {
                const int dim = runtime_->speakerDim(extractor_);
                embedding = QList<float>(vector, vector + dim);
                runtime_->speakerDestroyEmbedding(vector);
            }
        }
        runtime_->destroyOnlineStream(stream);
        qCDebug(lcVoice) << "speaker embedding of" << pcm16k.size() / 32 << "ms in" << timer.elapsed() << "ms";
        return embedding;
    }

    // How like `profile` the recording is: the whole of it, and its best 1.5 s window when it is longer.
    std::optional<double> bestScore(const QByteArray &pcm16k, const QList<float> &profile) {
        const std::optional<QList<float>> whole = embed(pcm16k);
        if (!whole) return std::nullopt;
        double best = VoiceProfile::cosine(*whole, profile);
        if (pcm16k.size() > kWindow + kHop) {
            for (qsizetype at = 0; at + kWindow <= pcm16k.size(); at += kHop) {
                if (const std::optional<QList<float>> part = embed(pcm16k.mid(at, kWindow))) {
                    best = std::max(best, VoiceProfile::cosine(*part, profile));
                }
            }
        }
        return best;
    }

private:
    SherpaRuntime *runtime_;
    const SherpaOnnxSpeakerEmbeddingExtractor *extractor_{nullptr};
};

SpeakerGate::SpeakerGate(SherpaRuntime *runtime, QObject *parent) : ISpeakerGate(parent), runtime_(runtime) {
    worker_ = new SpeakerWorker(runtime_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName(u"nexa-speaker"_s);
    thread_.start();
}

SpeakerGate::~SpeakerGate() {
    thread_.quit();
    thread_.wait();     // the worker frees the extractor on its own thread
}

bool SpeakerGate::canTell() const {
    return runtime_ && runtime_->load() && runtime_->canTellSpeakers();
}

QString SpeakerGate::modelName() const {
    return runtime_ ? QFileInfo(runtime_->speakerModel()).fileName() : QString();
}

void SpeakerGate::setProfile(std::optional<VoiceProfile> profile) {
    profile_ = std::move(profile);
}

double SpeakerGate::thresholdFor(const QString &strictness) {
    // Measured with ERes2Net on three voices (tests/SpeakerGateTest, M16's recordings): the same voice scores 0.59–0.86
    // — clean, over a fan at 5 dB, under a TV at −6 dB — and another voice −0.19–0.20. Real voices vary more than
    // recorded ones (another microphone, a cold), so the thresholds sit low in the gap.
    if (strictness == u"low"_s) return 0.30;
    if (strictness == u"high"_s) return 0.50;
    return 0.40;
}

bool SpeakerGate::active() const {
    return ownerOnly_ && profile_ && profile_->model == modelName() && canTell();
}

void SpeakerGate::check(const QByteArray &pcm16k, std::function<void(bool owner, double score)> done) {
    if (!active() || pcm16k.size() < kShortest) {
        done(true, 1.0);
        return;
    }
    score(pcm16k, [this, done = std::move(done)](std::optional<double> scored) {
        if (!scored) {
            done(true, 1.0);        // it could not be told: not turned away for that
            return;
        }
        const double score = *scored;
        const bool owner = score >= threshold_;
        if (!owner) ++turnedAway_;
        qCDebug(lcVoice).nospace() << "speaker: " << (owner ? "the owner" : "not the owner") << ", score " << score
                                   << " (threshold " << threshold_ << ")";
        done(owner, score);
    });
}

void SpeakerGate::score(const QByteArray &pcm16k, std::function<void(std::optional<double>)> done) {
    if (!profile_ || !canTell()) {
        done(std::nullopt);
        return;
    }
    const QPointer<SpeakerGate> self(this);
    QMetaObject::invokeMethod(worker_, [self, worker = worker_, pcm16k, profile = profile_->embedding,
                                        done = std::move(done)]() mutable {
        const std::optional<double> scored = worker->bestScore(pcm16k, profile);
        QMetaObject::invokeMethod(self.data(), [self, scored, done = std::move(done)] {
            if (self) done(scored);
        }, Qt::QueuedConnection);
    });
}

void SpeakerGate::embed(const QByteArray &pcm16k, std::function<void(std::optional<QList<float>>)> done) {
    if (!canTell() || pcm16k.size() < kShortest) {
        done(std::nullopt);
        return;
    }
    const QPointer<SpeakerGate> self(this);
    QMetaObject::invokeMethod(worker_, [self, worker = worker_, pcm16k, done = std::move(done)]() mutable {
        const std::optional<QList<float>> embedding = worker->embed(pcm16k);
        QMetaObject::invokeMethod(self.data(), [self, embedding, done = std::move(done)] {
            if (self) done(embedding);
        }, Qt::QueuedConnection);
    });
}

} // namespace nexa
