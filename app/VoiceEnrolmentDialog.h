#pragma once

#include "voice/listen/VoiceProfile.h"

#include <QDialog>
#include <QList>

#include <optional>

class QLabel;
class QPushButton;

namespace nexa {

class SpeakerGate;
class VoicePipeline;

// "Enrol my voice" (Settings → Voice): the owner reads three sentences aloud, each recorded for a few seconds and
// turned into a speaker embedding; their mean is the profile. A fourth recording checks the match before it is saved.
// The recordings themselves are never kept — only the profile, a list of numbers, in voice-profile.json (0600).
class VoiceEnrolmentDialog : public QDialog {
    Q_OBJECT
public:
    VoiceEnrolmentDialog(VoicePipeline *voice, SpeakerGate *gate, const QString &wakeName, QWidget *parent = nullptr);

    std::optional<VoiceProfile> profile() const { return profile_; }

private:
    void record();
    void recorded(const QByteArray &pcm);
    void check();
    void refresh();

    VoicePipeline *voice_;
    SpeakerGate *gate_;
    QStringList sentences_;
    QList<QList<float>> embeddings_;
    std::optional<VoiceProfile> profile_;
    bool busy_{false};
    QLabel *prompt_;
    QLabel *status_;
    QPushButton *record_;
    QPushButton *check_;
    QPushButton *save_;
};

} // namespace nexa
