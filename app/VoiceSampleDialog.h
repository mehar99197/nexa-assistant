#pragma once

#include "voice/tts/VoiceSample.h"

#include <QDialog>

#include <memory>
#include <optional>

class QLabel;
class QPushButton;

namespace nexa {

class PcmPlayer;
class VoicePipeline;

// "Record my voice" (Settings → Voice → Speak in my voice): the owner reads a short passage aloud while the
// microphone stays open for fourteen seconds; the recording, trimmed of its quiet, is played back for them to judge
// and kept as the sample Nexa speaks from (VoiceSample: this computer only, 0600, never sent anywhere). Unlike the
// enrolment, the recording itself is what is kept — the dialog says so before the first Record.
class VoiceSampleDialog : public QDialog {
    Q_OBJECT
public:
    explicit VoiceSampleDialog(VoicePipeline *voice, QWidget *parent = nullptr);
    ~VoiceSampleDialog() override;

    std::optional<VoiceSample> sample() const { return sample_; }

private:
    void record();
    void recorded(const QByteArray &pcm);
    void play();
    void refresh();

    VoicePipeline *voice_;
    std::optional<VoiceSample> sample_;
    std::unique_ptr<PcmPlayer> player_;
    bool busy_{false};
    QLabel *passage_;
    QLabel *status_;
    QPushButton *record_;
    QPushButton *play_;
    QPushButton *save_;
};

} // namespace nexa
