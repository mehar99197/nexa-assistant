#pragma once

#include "voice/tts/ISpeechSynthesizer.h"
#include "voice/tts/Phonemizer.h"
#include "voice/tts/VoiceSample.h"

#include <QHash>
#include <QThread>

#include <atomic>
#include <memory>

namespace nexa {

class SherpaRuntime;

// Nexa speaking in the owner's own voice (Strategy): ZipVoice, through sherpa-onnx, given the sample the owner
// recorded in Settings → Voice as its reference and the sentence to say, both as phoneme tokens from Piper's
// phonemizer. The model (about 130 MB, loaded at the first sentence or warmUp()) and every generation run on a
// worker thread of their own; a sentence takes a second or two on the CPU, and cancel() stops the one under way
// through the model's progress callback. The sample never leaves the machine: the voice is made here.
class ClonedVoiceSynthesizer : public ISpeechSynthesizer {
    Q_OBJECT
public:
    ClonedVoiceSynthesizer(SherpaRuntime *sherpa, VoiceSample sample, Phonemizer phonemizer, double rate,
                           QObject *parent = nullptr);
    ~ClonedVoiceSynthesizer() override;

    QString name() const override;
    void synthesize(const QString &text, Done done) override;
    void cancel() override;
    void warmUp() override;

private:
    class Worker;
    struct Job {
        Done done;
        std::shared_ptr<std::atomic_bool> cancelled;
    };

    void deliver(quint64 id, Result<PcmAudio> clip);

    QThread thread_;
    Worker *worker_{nullptr};       // lives in thread_
    QHash<quint64, Job> jobs_;
    quint64 nextId_{1};
    QString sampleWords_;
};

} // namespace nexa
