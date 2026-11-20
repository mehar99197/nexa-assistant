#include "voice/wake/KeywordSpotter.h"

#include <QFile>

#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kSampleRate = 16'000;
constexpr double kNoticeDelaySeconds = 0.45;    // from the phrase's last piece to the hit (0.3–0.6 s measured)

} // namespace

Result<std::unique_ptr<KeywordSpotter>> KeywordSpotter::create(SherpaRuntime *runtime, const QString &keywordLines,
                                                               SpotterTuning tuning) {
    if (!runtime || !runtime->load()) {
        return Error{ErrorCode::NotFound, runtime ? runtime->problem() : u"No sherpa-onnx."_s};
    }
    const QString dir = runtime->kwsDir();
    const QByteArray encoder = QFile::encodeName(dir + u"/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s);
    const QByteArray decoder = QFile::encodeName(dir + u"/decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s);
    const QByteArray joiner = QFile::encodeName(dir + u"/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s);
    const QByteArray tokens = QFile::encodeName(dir + u"/tokens.txt"_s);
    const QByteArray keywords = keywordLines.toUtf8();

    SherpaOnnxKeywordSpotterConfig config{};
    config.feat_config.sample_rate = kSampleRate;
    config.feat_config.feature_dim = 80;
    config.model_config.transducer.encoder = encoder.constData();
    config.model_config.transducer.decoder = decoder.constData();
    config.model_config.transducer.joiner = joiner.constData();
    config.model_config.tokens = tokens.constData();
    config.model_config.num_threads = 1;
    config.model_config.provider = "cpu";
    config.max_active_paths = 4;
    config.num_trailing_blanks = 1;
    config.keywords_score = tuning.boost;
    config.keywords_threshold = tuning.threshold;
    config.keywords_buf = keywords.constData();
    config.keywords_buf_size = int32_t(keywords.size());

    const SherpaOnnxKeywordSpotter *spotter = runtime->createKeywordSpotter(&config);
    if (!spotter) {
        return Error{ErrorCode::Unknown, u"The keyword spotter did not start (its model or the keywords were refused)."_s};
    }
    return std::unique_ptr<KeywordSpotter>(new KeywordSpotter(runtime, spotter));
}

KeywordSpotter::KeywordSpotter(SherpaRuntime *runtime, const SherpaOnnxKeywordSpotter *spotter)
    : runtime_(runtime), spotter_(spotter), stream_(runtime->createKeywordStream(spotter)) {}

KeywordSpotter::~KeywordSpotter() {
    if (stream_) runtime_->destroyOnlineStream(stream_);
    runtime_->destroyKeywordSpotter(spotter_);
}

qint64 KeywordSpotter::phraseStartSample(qint64 heardAt, float firstPieceTime, float lastPieceTime) {
    const double before = kNoticeDelaySeconds + double(lastPieceTime) - double(firstPieceTime);
    return std::max<qint64>(0, heardAt - qint64(std::lround(before * kSampleRate)));
}

QList<IWakeSpotter::Hit> KeywordSpotter::feed(QByteArrayView pcm16k) {
    const qsizetype count = pcm16k.size() / 2;
    std::vector<qint16> input(static_cast<size_t>(count));
    std::memcpy(input.data(), pcm16k.data(), size_t(count) * 2);
    std::vector<float> samples(static_cast<size_t>(count));
    for (size_t i = 0; i < input.size(); ++i) samples[i] = float(input[i]) / 32768.0f;
    runtime_->acceptWaveform(stream_, kSampleRate, samples.data(), int32_t(count));
    samples_ += count;

    QList<Hit> hits;
    while (runtime_->isKeywordStreamReady(spotter_, stream_)) {
        runtime_->decodeKeywordStream(spotter_, stream_);
        const SherpaOnnxKeywordResult *result = runtime_->getKeywordResult(spotter_, stream_);
        if (result && result->keyword && result->keyword[0] != '\0') {
            const float first = result->count > 0 ? result->timestamps[0] : 0.0f;
            const float last = result->count > 0 ? result->timestamps[result->count - 1] : 0.0f;
            hits.append(Hit{.label = QString::fromUtf8(result->keyword),
                            .phraseStart = phraseStartSample(samples_, first, last),
                            .heardAt = samples_});
            runtime_->resetKeywordStream(spotter_, stream_);
        }
        if (result) runtime_->destroyKeywordResult(result);
    }
    return hits;
}

void KeywordSpotter::reset() {
    // A new stream: positions count from 0 again.
    if (stream_) runtime_->destroyOnlineStream(stream_);
    stream_ = runtime_->createKeywordStream(spotter_);
    samples_ = 0;
}

} // namespace nexa
