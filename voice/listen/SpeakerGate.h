#pragma once

#include "voice/listen/ISpeakerGate.h"
#include "voice/listen/VoiceProfile.h"

#include <QList>
#include <QThread>

#include <optional>

namespace nexa {

class SherpaRuntime;
class SpeakerWorker;

// The owner's voice, told by sherpa-onnx's speaker embeddings (Facade): a recording becomes a vector on a thread of its
// own (ERes2Net, about 0.1 s for a sentence on this CPU) and is compared with the enrolled profile; at or above the
// threshold it is the owner. A recording too short to tell (under 0.8 s: "stop") goes through. Without the model or a
// profile, or set to anyone, it lets everything through.
class SpeakerGate : public ISpeakerGate {
    Q_OBJECT
public:
    explicit SpeakerGate(SherpaRuntime *runtime, QObject *parent = nullptr);
    ~SpeakerGate() override;

    // The model is installed and loads (scripts/download-sherpa.sh).
    bool canTell() const;
    QString modelName() const;
    void setProfile(std::optional<VoiceProfile> profile);
    const std::optional<VoiceProfile> &profile() const { return profile_; }
    void setOwnerOnly(bool ownerOnly) { ownerOnly_ = ownerOnly; }
    void setThreshold(double threshold) { threshold_ = threshold; }
    double threshold() const { return threshold_; }
    int turnedAway() const { return turnedAway_; }      // voices not the owner's, since Nexa started

    // low | normal | high → the score a voice must reach.
    static double thresholdFor(const QString &strictness);

    bool active() const override;
    void check(const QByteArray &pcm16k, std::function<void(bool owner, double score)> done) override;
    // How like the profile a recording is (the best of the whole and its 1.5 s windows); none without a profile.
    void score(const QByteArray &pcm16k, std::function<void(std::optional<double>)> done);
    // The embedding of a recording (enrolment), or none when it is too short or the model cannot run.
    void embed(const QByteArray &pcm16k, std::function<void(std::optional<QList<float>>)> done);

private:
    SherpaRuntime *runtime_;
    QThread thread_;
    SpeakerWorker *worker_{nullptr};
    std::optional<VoiceProfile> profile_;
    bool ownerOnly_{true};
    double threshold_{thresholdFor(QStringLiteral("normal"))};
    int turnedAway_{0};
};

} // namespace nexa
