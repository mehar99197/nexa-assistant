#pragma once

#include "core/Result.h"
#include "voice/wake/IWakeSpotter.h"
#include "voice/wake/SherpaRuntime.h"
#include "voice/wake/WakePhrases.h"

#include <memory>

namespace nexa {

// The wake name heard as the audio streams (IWakeSpotter): sherpa-onnx's open-vocabulary keyword spotter with
// the keyword lines WakePhrases builds from the owner's name — any name, no training, under 3 % of one core.
// Runs on the listening front's thread.
class KeywordSpotter : public IWakeSpotter {
public:
    static Result<std::unique_ptr<KeywordSpotter>> create(SherpaRuntime *runtime, const QString &keywordLines,
                                                         SpotterTuning tuning);
    ~KeywordSpotter() override;

    QList<Hit> feed(QByteArrayView pcm16k) override;
    void reset() override;

    // Where the phrase began. The spotter notices a name about 0.45 s after its last piece, and its timestamps
    // tell how long the phrase was; after a reset they share no reference with the audio, so only the length
    // is used (measured 2026-10-29).
    static qint64 phraseStartSample(qint64 heardAt, float firstPieceTime, float lastPieceTime);

private:
    KeywordSpotter(SherpaRuntime *runtime, const SherpaOnnxKeywordSpotter *spotter);

    SherpaRuntime *runtime_;
    const SherpaOnnxKeywordSpotter *spotter_;
    const SherpaOnnxOnlineStream *stream_{nullptr};
    qint64 samples_{0};         // fed since the stream began
};

} // namespace nexa
